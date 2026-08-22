#include "debug_log_ring.h"

#include "atomspectra.h"
#include "debug_log_level_filter.h"
#include "http_io_gate.h"
#include "spectrogram.h"   // #FW-64: снимок состояния водопада в heartbeat
#include "wf_offload.h"    // #FW-64: счётчики выгрузки сегментов
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

static const char *TAG = "dbglog";

#define RING_SIZE       (384 * 1024)
#define NVS_NS          "dbglog"
#define HEARTBEAT_MS    60000
#define MAX_LINE        512

static char              *s_ring;
static size_t             s_cap;
static size_t             s_head;      // next write offset
static size_t             s_used;
static uint32_t           s_next_seq;  // monotonic line sequence
static uint32_t           s_dropped;    // вытеснено кольцом (место кончилось)
static uint32_t           s_lost_busy;  // отброшено: мьютекс был занят
static uint32_t           s_gen;
static bool               s_enabled;
static dbglog_level_t     s_level;
static SemaphoreHandle_t  s_mtx;
static vprintf_like_t     s_prev_vprintf;
static TaskHandle_t       s_hb_task;
static volatile bool      s_hb_run;
static bool               s_hook_installed;

static const char *const s_own_info_tags[] = {
    "main", "wf", "spectrum", "spec_cache", "web", "wf_web", "wf_ofl",
    "usb_cdc", "http_io", "bootcfg", "tcp_bridge", "monitor", "wifi_mgr", "dbglog",
};
// Чужие теги, поднимаемые только на detailed: сеть и HTTP-стек.
static const char *const s_sys_info_tags[] = {
    "httpd", "httpd_txrx", "httpd_uri", "httpd_parse",
    "wifi", "wifi_init", "esp_netif_handlers", "dhcpc", "dhcps", "lwip",
};
static const char *const s_own_debug_tags[] = {
    "usb_cdc", "spectrum", "wf_ofl",
};

static void nvs_load(bool *en, dbglog_level_t *lv)
{
    *en = false;
    *lv = DBGLOG_LEVEL_STANDARD;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t e = 0, l = 0;
    if (nvs_get_u8(h, "en", &e) == ESP_OK) *en = (e != 0);
    if (nvs_get_u8(h, "lv", &l) == ESP_OK && l <= DBGLOG_LEVEL_DEBUG) *lv = (dbglog_level_t)l;
    nvs_close(h);
}

static esp_err_t nvs_save(bool en, dbglog_level_t lv)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_u8(h, "en", en ? 1 : 0);
    e |= nvs_set_u8(h, "lv", (uint8_t)lv);
    e |= nvs_commit(h);
    nvs_close(h);
    return e;
}

static void apply_layer_a(bool enabled, dbglog_level_t level)
{
    if (!enabled) {
        // Restore IDF default (INFO) — no WARN floor lingering.
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }
    // Лестница: чем выше уровень, тем шире круг тегов, а не только глубина.
    // «*» держим на WARN — чужие подсистемы шумят больше, чем помогают.
    esp_log_level_set("*", ESP_LOG_WARN);

    // standard: свои теги на INFO. Без этого включённое кольцо писало бы
    // только WARN/ERROR, то есть меньше, чем видно на UART по умолчанию, —
    // «включил диагностику и стало хуже видно».
    for (size_t i = 0; i < sizeof(s_own_info_tags) / sizeof(s_own_info_tags[0]); i++)
        esp_log_level_set(s_own_info_tags[i], ESP_LOG_INFO);

    // detailed: добавляем системные теги сети — именно они объясняют разрывы
    // и таймауты, которые с одними своими тегами выглядят как «просто пропал».
    if (level >= DBGLOG_LEVEL_DETAILED) {
        for (size_t i = 0; i < sizeof(s_sys_info_tags) / sizeof(s_sys_info_tags[0]); i++)
            esp_log_level_set(s_sys_info_tags[i], ESP_LOG_INFO);
    }
    if (level >= DBGLOG_LEVEL_DEBUG) {
        for (size_t i = 0; i < sizeof(s_own_debug_tags) / sizeof(s_own_debug_tags[0]); i++)
            esp_log_level_set(s_own_debug_tags[i], ESP_LOG_DEBUG);
    }
}

static void ring_append_locked(const char *data, size_t len)
{
    if (!s_ring || !s_cap || len == 0) return;
    if (len > s_cap) {
        data += (len - s_cap);
        len = s_cap;
        s_dropped++;
    }
    while (s_used + len > s_cap) {
        // Drop one line from the oldest side (scan to next \n).
        size_t tail = (s_head + s_cap - s_used) % s_cap;
        size_t drop = 0;
        while (drop < s_used) {
            char c = s_ring[(tail + drop) % s_cap];
            drop++;
            if (c == '\n') break;
        }
        if (drop == 0) drop = 1;
        s_used -= drop;
        s_dropped++;
    }
    size_t first = s_cap - s_head;
    if (first > len) first = len;
    memcpy(s_ring + s_head, data, first);
    if (len > first) memcpy(s_ring, data + first, len - first);
    s_head = (s_head + len) % s_cap;
    s_used += len;
    s_next_seq++;
}

static void ring_append(const char *data, size_t len)
{
    if (!s_mtx) return;
    // Таймаут строго 0: ESP_LOGx может прийти из региона с подавленным
    // планировщиком, а xQueueSemaphoreTake содержит configASSERT, запрещающий
    // там блокирующее ожидание (FreeRTOS queue.c, «Cannot block if the
    // scheduler is suspended»). Инструмент для ловли зависаний не должен сам
    // быть источником паники. Цена — строки, потерянные при занятом мьютексе;
    // они считаются отдельно от вытеснения по кольцу, иначе «логов не было» не
    // отличить от «потеряны в интересный момент».
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) {
        s_lost_busy++;
        return;
    }
    // s_ring мог быть уже освобождён: free_ring() успел отработать и отдать
    // мьютекс до нашего входа. Проверки s_enabled в hooked_vprintf для этого
    // недостаточно — она пройдена раньше, чем начался teardown.
    if (s_ring)
        ring_append_locked(data, len);
    xSemaphoreGive(s_mtx);
}

void debug_log_ring_write_raw(const char *line)
{
    if (!s_enabled || !line) return;
    size_t n = strlen(line);
    char tmp[MAX_LINE];
    if (n >= sizeof(tmp) - 1) n = sizeof(tmp) - 2;
    memcpy(tmp, line, n);
    if (n == 0 || tmp[n - 1] != '\n') {
        tmp[n++] = '\n';
    }
    tmp[n] = '\0';
    ring_append(tmp, n);
}

static bool task_is_stack_tight(void)
{
    // #FW-52: default event loop + WiFi driver run with small stacks. Our hook
    // normally spends ~512 B on a format buffer before UART; nesting that under
    // GOT_IP (wifi_mgr + esp_netif_handlers) overflowed sys_evt @ 2304 and
    // rebooted the lab test board in a loop (2026-08-05). Skip ring + local format.
    //
    // Task names "sys_evt" / "wifi" are ESP-IDF v5.4 FreeRTOS conventions, not a
    // public API contract — if a future IDF renames them, this pass-through silently
    // stops and the overflow can return. Prefer uxTaskGetStackHighWaterMark if IDF
    // exposes a stable alternative; until then keep names tied to the pinned IDF.
    const char *name = pcTaskGetName(NULL);
    if (!name) return false;
    return strcmp(name, "sys_evt") == 0 || strcmp(name, "wifi") == 0;
}

static int hooked_vprintf(const char *fmt, va_list args)
{
    // Tight-stack tasks: pass through only (no MAX_LINE frame, no ring). UART
    // still gets the line via IDF's vprintf; ring loses those few WiFi/IP lines.
    if (task_is_stack_tight()) {
        if (s_prev_vprintf) return s_prev_vprintf(fmt, args);
        return vprintf(fmt, args);
    }

    char buf[MAX_LINE];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

    // Layer B: always mirror into ring when enabled (Layer A already filtered generation).
    if (s_enabled && n > 0)
        ring_append(buf, (size_t)n);

    // UART: only ≤ INFO — правило и разбор CSI живут в debug_log_level_filter.h,
    // который покрыт host-тестом tests/host/test_debug_log_filter.c. Там же
    // оговорка, почему пропуск CSI — упрочнение, а не починка дефекта.
    bool to_uart = dbglog_line_goes_to_uart(buf, n);

    int ret = 0;
    if (to_uart && s_prev_vprintf)
        ret = s_prev_vprintf(fmt, args);
    else if (to_uart)
        ret = vprintf(fmt, args);
    else
        ret = n; // swallow D on UART
    return ret;
}

static void install_hook(void)
{
    if (s_hook_installed) return;
    s_prev_vprintf = esp_log_set_vprintf(hooked_vprintf);
    s_hook_installed = true;
}

static void uninstall_hook(void)
{
    if (!s_hook_installed) return;
    if (s_prev_vprintf)
        esp_log_set_vprintf(s_prev_vprintf);
    else
        esp_log_set_vprintf(vprintf);
    s_prev_vprintf = NULL;
    s_hook_installed = false;
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (s_hb_run) {
        uint32_t free_h = esp_get_free_heap_size();
        uint32_t min_h = esp_get_minimum_free_heap_size();
        int rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

        // httpd / heavy gate snapshot
        bool heavy = http_io_gate_busy();
        int waiters = http_io_gate_waiters();
        uint32_t rej = http_io_gate_reject_count();

        const spectrum_data_t *sp = spectrum_get_current();
        uint32_t hok = 0, hd = 0;
        spectrum_get_hist_stats(&hok, &hd);

        // #FW-64: состояние водопада тем же способом, что и остальные поля -
        // снимок через публичный геттер. spectrogram_get_status() берёт только
        // свой LOCK и держит его на время memcpy (порядок захвата: FS -> status,
        // никогда наоборот, см. spectrogram.c), поэтому из heartbeat безопасен.
        // До spectrogram_init() геттер отдаёт нули, а не падает: heartbeat
        // стартует раньше инициализации водопада.
        wf_status_t ws;
        spectrogram_get_status(&ws);
        // Выгрузка инициализируется только в indoor-режиме; в остальных
        // случаях геттер отдаёт нулевую структуру.
        wf_offload_stat_t os;
        wf_offload_get_stat(&os);

        // 512 = MAX_LINE кольца: писать длиннее бессмысленно,
        // debug_log_ring_write_raw() всё равно обрежет.
        char line[512];
        snprintf(line, sizeof(line),
                 "HB up=%llu free=%lu min=%lu fill=%d%% drop=%lu gen=%lu "
                 "rssi=%d wifi=%s usb=%s cps=%lu t1=%lu heavy=%d wait=%d rej=%lu "
                 "hok=%" PRIu32 " hd=%" PRIu32 " rxe=%" PRIu32 " rrd=%" PRIu32
                 // #FW-64: водопад. Поля дописаны В КОНЕЦ строки намеренно -
                 // дамп кольца забирает внешний сборщик, живущий вне этого
                 // репозитория; порядок и имена прежних полей для него контракт.
                 " rec=%d seg=%" PRIu32 " lost=%" PRIu32 " evic=%" PRIu32
                 " wrows=%" PRIu32 " full=%d ofok=%" PRIu32 " offail=%" PRIu32,
                 (unsigned long long)(esp_timer_get_time() / 1000000ULL),
                 (unsigned long)free_h, (unsigned long)min_h,
                 debug_log_ring_fill_pct(),
                 (unsigned long)s_dropped, (unsigned long)s_gen,
                 rssi,
                 wifi_is_connected() ? "1" : "0",
                 usb_host_cdc_is_connected() ? "1" : "0",
                 sp ? (unsigned long)sp->cps : 0UL,
                 sp ? (unsigned long)sp->total_time_sec : 0UL,
                 heavy ? 1 : 0, waiters, (unsigned long)rej,
                 hok, hd,
                 usb_host_cdc_rx_errors(),
                 usb_host_cdc_rx_ring_drops(),
                 ws.recording ? 1 : 0, ws.seg_count, ws.seg_lost, ws.seg_evicted,
                 ws.flash_rows, ws.flash_full ? 1 : 0,
                 os.sent_ok, os.failed);
        debug_log_ring_write_raw(line);

        for (int i = 0; i < HEARTBEAT_MS / 200 && s_hb_run; i++)
            vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_hb_task = NULL;
    vTaskDelete(NULL);
}

static void stop_heartbeat(void)
{
    s_hb_run = false;
    for (int i = 0; i < 50 && s_hb_task; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
}

static void start_heartbeat(void)
{
    if (s_hb_task) return;
    s_hb_run = true;
    xTaskCreate(heartbeat_task, "dbglog_hb", 3072, NULL, 3, &s_hb_task);
}

static esp_err_t alloc_ring(void)
{
    if (s_ring) return ESP_OK;
    s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "PSRAM ring alloc failed (%d bytes)", RING_SIZE);
        return ESP_ERR_NO_MEM;
    }
    s_cap = RING_SIZE;
    s_head = 0;
    s_used = 0;
    s_next_seq = 0;
    s_dropped = 0;
    s_gen++;
    return ESP_OK;
}

// Освобождение буфера обязано идти под тем же мьютексом, что и запись:
// ring_append_locked() делает memcpy в s_ring, а uninstall_hook() не ждёт
// задачи, уже вошедшие в hooked_vprintf. Снятия s_enabled тоже недостаточно —
// задача могла пройти проверку флага до начала teardown и дойти до мьютекса
// уже после free(). Поэтому мьютекс здесь берётся безусловно (portMAX_DELAY:
// вызов идёт из HTTP-обработчика, блокироваться ему можно), а s_ring
// перепроверяется под мьютексом во всех потребителях — append, дампе, flush.
static void free_ring(void)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    free(s_ring);
    s_ring = NULL;
    s_cap = s_head = s_used = 0;
    xSemaphoreGive(s_mtx);
}

static esp_err_t enable_runtime(dbglog_level_t level)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (alloc_ring() != ESP_OK) {
        s_enabled = false;
        apply_layer_a(false, level);
        return ESP_ERR_NO_MEM;
    }
    s_enabled = true;
    s_level = level;
    apply_layer_a(true, level);
    install_hook();
    start_heartbeat();
    ESP_LOGI(TAG, "debug log ring ON level=%d gen=%lu", (int)level, (unsigned long)s_gen);
    return ESP_OK;
}

static void disable_runtime(void)
{
    // Порядок обязателен: сначала закрываем вход в кольцо (флаг + Layer A),
    // потом убираем поставщиков строк (heartbeat, hook), и только затем
    // освобождаем буфер — free_ring() возьмёт мьютекс и дождётся тех, кто уже
    // внутри ring_append.
    s_enabled = false;
    apply_layer_a(false, s_level);
    stop_heartbeat();
    uninstall_hook();
    free_ring();
    ESP_LOGI(TAG, "debug log ring OFF (full teardown)");
}

void debug_log_ring_boot(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    bool en = false;
    dbglog_level_t lv = DBGLOG_LEVEL_STANDARD;
    nvs_load(&en, &lv);
    s_level = lv;
    s_enabled = false;
    if (en) {
        if (enable_runtime(lv) != ESP_OK)
            ESP_LOGW(TAG, "NVS enabled but ring alloc failed — staying off");
    } else {
        apply_layer_a(false, lv);
    }
}

bool debug_log_ring_enabled(void) { return s_enabled; }
dbglog_level_t debug_log_ring_level(void) { return s_level; }
uint32_t debug_log_ring_next_seq(void) { return s_next_seq; }
uint32_t debug_log_ring_dropped(void) { return s_dropped; }
uint32_t debug_log_ring_lost_busy(void) { return s_lost_busy; }
uint32_t debug_log_ring_gen(void) { return s_gen; }

int debug_log_ring_fill_pct(void)
{
    if (!s_cap) return 0;
    return (int)((s_used * 100) / s_cap);
}

esp_err_t debug_log_ring_set_config(bool enabled, dbglog_level_t level)
{
    if (level > DBGLOG_LEVEL_DEBUG) level = DBGLOG_LEVEL_DEBUG;
    esp_err_t se = nvs_save(enabled, level);
    if (se != ESP_OK) return se;

    if (!enabled) {
        disable_runtime();
        s_level = level;
        return ESP_OK;
    }
    if (s_enabled) {
        s_level = level;
        apply_layer_a(true, level);
        return ESP_OK;
    }
    return enable_runtime(level);
}

esp_err_t debug_log_ring_flush(uint32_t upto_seq, uint32_t gen)
{
    if (!s_enabled || !s_ring) return ESP_OK;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (gen != 0 && gen != s_gen) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ring) {                        // мог уйти в free_ring, пока мы ждали
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }

    // upto_seq == 0 — полный сброс: нумерация начинается заново, поэтому
    // поколение растёт и клиент понимает, что его since больше не применим.
    if (upto_seq == 0) {
        s_head = 0;
        s_used = 0;
        s_next_seq = 0;
        s_dropped = 0;
        s_gen++;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }

    // Частичный сброс: выкидываем строки со seq < upto_seq, остальные остаются
    // с прежними номерами — значит next_seq и gen не трогаем, иначе клиент,
    // подтвердивший забранный кусок, потеряет ещё не прочитанный хвост.
    uint32_t lines = 0;
    for (size_t i = 0; i < s_used; i++)
        if (s_ring[(s_head + s_cap - s_used + i) % s_cap] == '\n') lines++;
    uint32_t first_seq = (s_next_seq >= lines) ? (s_next_seq - lines) : 0;

    if (upto_seq >= s_next_seq) {         // подтверждено всё, что есть
        s_used = 0;
    } else if (upto_seq > first_seq) {
        uint32_t skip = upto_seq - first_seq;
        size_t drop = 0;
        while (skip > 0 && drop < s_used) {
            if (s_ring[(s_head + s_cap - s_used + drop) % s_cap] == '\n') skip--;
            drop++;
        }
        s_used -= drop;                   // start выводится из head и used
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t debug_log_ring_http_dump(httpd_req_t *req, uint32_t since)
{
    // httpd_resp_set_hdr keeps pointers — must be static storage.
    static char hdr_next[16], hdr_drop[16], hdr_gen[16];
    httpd_resp_set_type(req, "text/plain");

    // Заголовки и тело обязаны описывать одно состояние кольца, поэтому
    // счётчики снимаются в том же критическом участке, что и копия данных.
    // Раньше X-Log-Next-Seq читался до мьютекса и мог отстать от тела на
    // строки, добавленные во время ожидания — клиент терял их навсегда,
    // продолжив следующий забор с since = устаревшего next.
    uint32_t next = 0, dropped = 0, gen = 0;
    char *tmp = NULL;
    size_t used = 0;
    bool have_ring = false;

    if (s_enabled && s_ring) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE)
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        if (s_ring) {                     // мог уйти в free_ring, пока мы ждали
            used = s_used;
            size_t start = (s_head + s_cap - used) % s_cap;
            tmp = malloc(used + 1);
            if (!tmp) {
                xSemaphoreGive(s_mtx);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            }
            size_t first = s_cap - start;
            if (first > used) first = used;
            memcpy(tmp, s_ring + start, first);
            if (used > first) memcpy(tmp + first, s_ring, used - first);
            tmp[used] = '\0';
            have_ring = true;
        }
        next = s_next_seq;
        dropped = s_dropped;
        gen = s_gen;
        xSemaphoreGive(s_mtx);
    } else {
        next = s_next_seq;
        dropped = s_dropped;
        gen = s_gen;
    }

    snprintf(hdr_next, sizeof(hdr_next), "%lu", (unsigned long)next);
    snprintf(hdr_drop, sizeof(hdr_drop), "%lu", (unsigned long)dropped);
    snprintf(hdr_gen, sizeof(hdr_gen), "%lu", (unsigned long)gen);
    httpd_resp_set_hdr(req, "X-Log-Next-Seq", hdr_next);
    httpd_resp_set_hdr(req, "X-Log-Dropped", hdr_drop);
    httpd_resp_set_hdr(req, "X-Log-Gen", hdr_gen);

    if (!have_ring) {
        free(tmp);
        return httpd_resp_send(req, "", 0);
    }

    // Skip lines until we've passed `since` worth of line ends counted from
    // (next - line_count). Approximate: count lines in buffer, skip oldest.
    uint32_t lines = 0;
    for (size_t i = 0; i < used; i++) if (tmp[i] == '\n') lines++;
    uint32_t first_seq = (next >= lines) ? (next - lines) : 0;
    const char *out = tmp;
    size_t out_len = used;
    if (since > first_seq && since < next) {
        uint32_t skip = since - first_seq;
        size_t i = 0;
        while (skip > 0 && i < used) {
            if (tmp[i++] == '\n') skip--;
        }
        out = tmp + i;
        out_len = used - i;
    } else if (since >= next) {
        out_len = 0;
    }

    esp_err_t err = httpd_resp_send(req, out_len ? out : "", out_len);
    free(tmp);
    return err;
}

int debug_log_ring_meta_json(char *buf, size_t bufsz)
{
    // Только состояние кольца. fw_version / uptime / heap сюда не кладём —
    // без auth это фингерпринт платы; CSRF GET /api/csrf-token не закрывает
    // соседа в LAN. Настоящий барьер — будущая аутентификация; рядом те же
    // поля всё ещё отдаёт открытый /api/system (гигиена, не защита).
    const char *lv =
        s_level == DBGLOG_LEVEL_DEBUG ? "debug" :
        s_level == DBGLOG_LEVEL_DETAILED ? "detailed" : "standard";
    return snprintf(buf, bufsz,
        "{\"enabled\":%s,\"level\":\"%s\",\"next_seq\":%lu,\"dropped\":%lu,"
        "\"lost_busy\":%lu,\"gen\":%lu,\"fill_pct\":%d}",
        s_enabled ? "true" : "false", lv,
        (unsigned long)s_next_seq, (unsigned long)s_dropped,
        (unsigned long)s_lost_busy,
        (unsigned long)s_gen, debug_log_ring_fill_pct());
}
