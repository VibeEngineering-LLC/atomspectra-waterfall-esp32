#include "socket_budget.h"  // 1.2.36 P1: #error при устаревшем sdkconfig (LWIP_MAX_SOCKETS/ACTIVE_TCP < 24)
#include "atomspectra.h"
#include "calib_autoread.h"  // #AWF-12b R4: calib_is_missing — тот же снимок, что "calibration"
#include "calib_export.h"    // R7 (sweep-A): признак «калибровка есть» в экспорте XML/N42/SPE
#include "http_gate_budget.h" // P-009 (sweep-A): единый бюджет ожидания HEAVY-слота
#include "spectrum_t1.h"
#include "shproto.h"
#include "web_waterfall.h"
#include "web_util.h"
#include "boot_config.h"
#include "http_activity_plan.h"   // AWF-2a captive: http_uri_is_activity()
#include "spectrogram.h"    // #FIELD-5: spectrogram_time_synced() при установке времени
#include "net_time.h"       // #FIELD-5: guard-логика источника времени
#include "monitor.h"        // #MON-1: серия CPS-мониторинга (/api/monitor/series)
#include "meta_w_handler.h" // З-5 (gate 1.2.36 pass1) / З-3 (pass2): обработчик meta.json?w= (разбор meta_w_parse.h)
#include "spectrum_http_cache.h"  // #PERF-1: 2s snapshot cache + meta/binary
#include "task_load_sampler.h"    // D1: загрузка по задачам/ядрам за интервал между вызовами /api/status
#include "spectrum_gz.h"          // #GZ-T (1.2.37-rc90): тест сжатия gzip тел спектра (задача, метрики)
#include "spectrum_gz_plan.h"     // #GZ-T: spec_gz_level_valid (POST /api/boot-config)
#include "spectrum_calib_meta.h"  // 1.2.36 H12: /api/device без буфера 33 КБ
#include "http_io_gate.h"         // #PERF-2: HEAVY lane gate
#include "flash_quiet.h"          // М7: координация записи слепка DSP с autosave (#FW-8)
#include "backup_plan.h"          // issue #52: разбор имени снимка в /api/backup/*
#include "debug_log_ring.h"
#include "esp_heap_caps.h"  // #MON-1: PSRAM-буфер чанка серии
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>   // issue #52: list_append() с зажимом длины
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include <math.h>
#include <sys/time.h>       // #FIELD-5: gettimeofday/settimeofday
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"   // #FW-28: esp_app_get_description() → версия прошивки
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "esp_ota_ops.h"          // AWF-4: POST /api/ota (Wi-Fi OTA)
#include "esp_app_format.h"       // AWF-4: ESP_CHIP_ID_ESP32S3
#include "ota_image_check.h"      // AWF-4: проверка заголовка образа (host-тест)
#include "ota_github_client.h"    // AWF-5: обновление с GitHub
#include "ota_busy.h"              // AWF-5 P1-фикс: общий замок с GitHub-install
#include "ota_timeout_budget.h"    // D4 (sweep-B задача 1): host-тест границы
#include "spectrum_import_plan.h"  // 1.2.31: импорт фона (разбор тела ASI1) + путь удаления записи (host-тест)
#include "n42_time_plan.h"           // #AWF-F2: StartDateTime N42 в UTC с Z (host-тест)
#include "ttl_plan.h"            // 1.2.36 H5: срок жизни медленной части /api/system (host-тест)
#include "cmdlog_glue.h"          // 1.2.39-t1: журнал команд к прибору
#define SYS_SLOW_PERIOD_US (10LL * 1000000LL)   // 1.2.36 H5: период обхода кучи/стеков (решение оператора 2026-10-10)
#include "import_body_plan.h"      // 1.2.32: D-1131-1/#AWF-F3 — план приёма тела /api/import (host-тест)
_Static_assert(IMPORT_BODY_SIZE == SPEC_IMPORT_SIZE, "import_body_plan.h: IMPORT_BODY_SIZE != SPEC_IMPORT_SIZE");
#include "snapshot_file.h"         // У4 (раунд 3): слепок DSP — tmp + rename (host-тест)
#include <dirent.h>

static const char *TAG = "web";

// #FIELD-9 (A11): часы платы «синхронизированы», если время не near-epoch (< 2023-11).
// В полевом AP до прихода браузерного времени часы = 1970; экспорт помечает это и НЕ
// падает (localtime_r сам не падает). Пометка добавляется ТОЛЬКО при рассинхроне →
// нормальные записи не получают лишних строк относительно эталонных форматов (#EXP-1 N42/#CSV-1);
// исключение — StartDateTime N42: ISO UTC с Z (#AWF-F2), эталон BecqMoni писал dd.mm.yyyy местного времени.
// Теста «T14» в репозитории нет; дату N42 проверяет tests/host/test_n42_time_plan.c.
#define WF_TIME_SYNCED_EPOCH 1700000000L   // 2023-11-14 UTC
static inline bool time_is_synced(time_t t) { return t >= WF_TIME_SYNCED_EPOCH; }

// #3/Codeaudit P1: ожидание HEAVY-слота для сохранённых-спектров эндпоинтов
// (спектры/save/list/export/delete на Flash): пользователь ждёт клик, мгновенный 503
// на задержке слота — плохой UX. P-009 (sweep-A): 250 мс было короче одного удержания
// фоновым автосейвом/бэкапом (~0,6–0,7 с) — теперь общий бюджет с web_waterfall.c,
// значение и обоснование в http_gate_budget.h (2000 мс).
#define SAVED_FLASH_GATE_WAIT_MS HTTP_GATE_WAIT_MS

static void json_add_temp(cJSON *o, const char *key, float t)
{
    if (isnan(t)) cJSON_AddNullToObject(o, key);
    else cJSON_AddNumberToObject(o, key, (double)t);
}

// CSRF-токен: генерируется при старте, выдаётся по GET /api/csrf-token,
// требуется в заголовке X-CSRF-Token на всех мутирующих POST. Защищает
// открытый-в-LAN Web UI от drive-by cross-origin POST: сторонняя страница
// не может прочитать токен (same-origin policy) → не может подделать запрос.
static char s_csrf[33];

static void csrf_generate(void)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        s_csrf[i] = hx[esp_random() & 0x0F];
    s_csrf[32] = '\0';
}

// P3-9: сравнение токена за постоянное время — не утекает позиция
// первого несовпавшего байта по времени ответа.
static bool csrf_ct_eq(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return d == 0;
}

// Проверяет X-CSRF-Token, ответа не шлёт (импорт сначала сливает тело, потом отвечает 403).
static bool csrf_ok(httpd_req_t *req)
{
    char hdr[40] = {0};
    return httpd_req_get_hdr_value_str(req, "X-CSRF-Token", hdr, sizeof(hdr)) == ESP_OK
        && s_csrf[0] && strlen(hdr) == 32 && csrf_ct_eq(hdr, s_csrf, 32);
}
// Проверяет X-CSRF-Token. При несовпадении сам шлёт 403 и возвращает false.
static bool csrf_check(httpd_req_t *req)
{
    if (csrf_ok(req)) return true;
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Bad or missing CSRF token");
    return false;
}

// Экспорт CSRF-проверки для web_waterfall.c (csrf_check здесь static).
bool web_csrf_check(httpd_req_t *req)
{
    return csrf_check(req);
}

static esp_err_t handle_csrf_token(httpd_req_t *req)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", s_csrf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static int parse_saved_index(const char *uri)
{
    const char *p = strstr(uri, "/api/saved/");
    if (!p) return -1;
    return atoi(p + 11);
}

// #AUD-DIAG-1 R1: причина последнего сброса. Только имена, что есть во всех IDF 5.x;
// прочее — "OTHER" + числовой reset_reason_code рядом.
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    default:                return "OTHER";
    }
}

// AWF-3 (#7): наблюдаемость слияния база+прибор в /api/status.
static void status_add_base_info(cJSON *root)
{
    // #AUD-DIAG-1 R1/R2: причина сброса, номер загрузки (сессия платы, +1 за загрузку,
    // issue #52), время работы и SHA256 ELF — версия "1.2.29" разные сборки не различает.
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str(rr));
    cJSON_AddNumberToObject(root, "reset_reason_code", (int)rr);
    cJSON_AddNumberToObject(root, "boot_count", boot_config_get_boot_session());   // #AUD-DIAG-1: номер загрузки
    cJSON_AddNumberToObject(root, "session", boot_config_get_session());           // 1.2.31: растёт и на Сбросе
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    char elf_sha[17];
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    cJSON_AddStringToObject(root, "elf_sha", elf_sha);
    uint32_t base_time = 0, base_counts = 0, dev_resets = 0;
    spectrum_get_base_info(&base_time, &base_counts, &dev_resets);
    cJSON_AddNumberToObject(root, "base_time", base_time);
    cJSON_AddNumberToObject(root, "base_counts", base_counts);
    cJSON_AddNumberToObject(root, "dev_resets", dev_resets);
    // У-2: Reset, которые прибор не выполнил (его набор сохранён по таймауту гейта #58).
    cJSON_AddNumberToObject(root, "reset_unconfirmed", spectrum_reset_unconfirmed_count());
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    const device_info_t   *di = spectrum_get_device_info();

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    cJSON_AddBoolToObject(root, "analyzer_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddNumberToObject(root, "total_counts", sp->total_counts);
    cJSON_AddNumberToObject(root, "cpu_load", sp->cpu_load);
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());
    // AWF-1 (#3): ФС отформатирована при этой загрузке — видимость сброса flash.
    cJSON_AddBoolToObject(root, "fs_formatted", spectrum_fs_was_formatted());
    // AWF-4: версия/раздел — UI опрашивает /api/status после OTA-заливки,
    // чтобы показать новую версию/раздел без /api/system (там fw_version уже
    // был, handle_system, #FW-28; здесь — тот же app_desc, для той же цели).
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app_desc ? app_desc->version : "?");
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "?");
    // AWF-3 (#8): раньше "time" был di->time_sec (из -inf, опрос раз в ~30 мин —
    // застывало между опросами, пока spectrum.json со STAT рос каждую секунду;
    // наблюдение 25.09: time=3611 неподвижно против растущего spectrum.json).
    cJSON_AddNumberToObject(root, "time", sp->total_time_sec);
    status_add_base_info(root);
    // D1: интервалы без завершённого коммита свипа (не hist_drop) и загрузка CPU по ядрам/задачам за окно между вызовами.
    cJSON_AddNumberToObject(root, "sweep_missed", spectrum_sweep_missed());
    cJSON *load = task_load_json();
    if (load) cJSON_AddItemToObject(root, "load", load);
    {   // #DOSE-A: живая МД за окно 10 с (умолчание SMA главной); нет данных / режим none — dose_rate:null
        spec_dose_t dz;
        monitor_dose_live(10, &dz);
        cJSON_AddStringToObject(root, "dose_mode", dz.mode);
        if (dz.have && isfinite(dz.rate)) cJSON_AddNumberToObject(root, "dose_rate", (double)dz.rate);
        else cJSON_AddNullToObject(root, "dose_rate");
        cJSON_AddNumberToObject(root, "dose_w", (double)dz.w);
    }

    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        json_add_temp(root, "t1", di->t1);
        json_add_temp(root, "t2", di->t2);
        json_add_temp(root, "t3", di->t3);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "max", di->max_integral);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (!json) httpd_resp_set_status(req, "503 Service Unavailable");   // F-09: нет памяти на JSON
    httpd_resp_sendstr(req, json ? json : "{\"ok\":false,\"err\":\"oom\"}");
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_spectrum(httpd_req_t *req)
{
    // #PERF-1: shared 2s cache — binary hot path for index.html
    return spectrum_http_send_bins(req);
}

static esp_err_t handle_spectrum_meta(httpd_req_t *req)
{
    // #PERF-1: meta without 8192 bins; 1.2.36 #MX-14 ?w= - SMA окна W. Разбор и ответы - meta_w_handler.h (gate pass2 З-3: исполняется host-тестом склейки)
    return meta_w_handle(req);
}

// #DT-4 / 1.2.36 OPT-1: живое время — метод BecqMoni (эталон Am6er, Utils/LiveTime.cs): мёртвое время на
// импульс tau = (RISE+FALL+1)/F (RISE/FALL/F из -inf прибора), dead = (counts+lost)·tau, live = total - dead.
// Формула одна на всё (main/live_time.h; обёртка spectrum_compute_live_time() в spectrum.c): раньше здесь и в
// spectrum_http_cache.c лежали две копии, а этот комментарий описывал pulse_width (STAT offset 14),
// которого в расчёте нет (поле pulse_width — диагностика, atomspectra.h).

static esp_err_t render_spectrum_json(httpd_req_t *req, const spectrum_data_t *sp)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"bins\":[");
    int pos = 0;
    for (int i = 0; i < SPECTRUM_CHANNELS; i++) {
        pos += snprintf(buf + pos, 4096 - pos, "%s%" PRIu32, (i > 0) ? "," : "", sp->bins[i]);
        if (pos > 3800) {
            httpd_resp_send_chunk(req, buf, pos);
            pos = 0;
        }
    }
    if (pos > 0) httpd_resp_send_chunk(req, buf, pos);
    uint32_t hist_ok = 0, hist_drop = 0;
    spectrum_get_hist_stats(&hist_ok, &hist_drop);
    int dead = usb_host_cdc_spectrometer_dead() ? 1 : 0;   // #FW-43: «определился, но не запитан»
    int as_age = spectrum_autosave_age_sec();
    int as_streak = spectrum_autosave_fail_streak();
    char jt1[16], jt2[16], jt3[16];
    spectrum_temp_json(jt1, sizeof jt1, sp->temperature[0]);
    spectrum_temp_json(jt2, sizeof jt2, sp->temperature[1]);
    spectrum_temp_json(jt3, sizeof jt3, sp->temperature[2]);
    int n = snprintf(buf, 4096,
        "],\"total\":%" PRIu32 ",\"cpu\":%u,\"cps\":%" PRIu32 ",\"lost\":%" PRIu32 ",\"time\":%" PRIu32 ",\"live\":%.1f,"
        "\"bridge_drop\":%" PRIu32 ",\"usb_rx_err\":%" PRIu32 ",\"rx_ring_drops\":%" PRIu32 ","
        "\"hist_ok\":%" PRIu32 ",\"hist_drop\":%" PRIu32 ","
        "\"autosave_age_sec\":%d,\"autosave_fail_streak\":%d,"
        "\"t1\":%s,\"t2\":%s,\"t3\":%s,\"serial\":\"%s\",\"dead\":%d",
        sp->total_counts, sp->cpu_load, sp->cps, sp->lost_impulses,
        sp->total_time_sec, spectrum_compute_live_time(sp),
        tcp_bridge_dropped_bytes(), usb_host_cdc_rx_errors(), usb_host_cdc_rx_ring_drops(),
        hist_ok, hist_drop, as_age, as_streak,
        jt1, jt2, jt3,
        sp->serial_number[0] ? sp->serial_number : "", dead);
    httpd_resp_send_chunk(req, buf, n);
    if (sp->calib_valid) {
        int p = snprintf(buf, 4096, ",\"calib\":[");
        for (int i = 0; i <= sp->calib_order; i++)
            p += snprintf(buf + p, 4096 - p, "%s%.15g", i ? "," : "", sp->calibration[i]);
        snprintf(buf + p, 4096 - p, "]");
        httpd_resp_sendstr_chunk(req, buf);
    }
    // #AWF-12b R7 (release-gate-1.2.28-code-rc2.md): тот же предикат, что
    // /api/device и /api/spectrum*.json — эта функция отдаёт сохранённые
    // спектры и автоснимки (/api/saved/<i>/spectrum.json, /api/backup/<name>/
    // spectrum.json), раньше только "calib" без "calib_set".
    {
        // 1.2.31: saved_at — дата конца набора (экспорт с одной платы → импорт на другую сохраняет дату)
        int q = snprintf(buf, 4096, ",\"calib_set\":%s,\"saved_at\":%ld",
            calib_is_missing(sp->calibration, CALIB_COEFFS, sp->calib_valid) ? "false" : "true", (long)sp->saved_at);
        httpd_resp_send_chunk(req, buf, q);
    }
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_spectrum_json(httpd_req_t *req)
{
    // #PERF-1: single-flight JSON cache (TTL 2s). X-Spectrometer-Dead on 404
    // handled inside spectrum_http_send_json.
    return spectrum_http_send_json(req);
}

static esp_err_t handle_command(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    // Cap below usb_host_send_text_command pkt_buf[512] headroom (escaped SHPROTO).
    char body[512] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (recv_len > 480) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command too long");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    // UI posts raw text ("-sta"); keep that contract. #FW-43: use send_text so
    // last_tx_cmd is visible on /api/usb-diag when Start appears to "do nothing".
    int ret = usb_host_send_text_command(body);
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

// #UI-1: лог текстовых ответов прибора для веб-UI. GET /api/devlog?since=N
// Read-only (CSRF не нужен). Отдаёт {"lines":[{"seq":N,"text":"..."}],"next":M}.
static esp_err_t handle_devlog(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    char *buf = malloc(9000);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    usb_host_cdc_devlog_json(since, buf, 9000);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

// 1.2.39-t1: GET /api/debug/cmdlog?since=N -- журнал последних команд к прибору (кольцо на 32).
// CSRF обязателен, как у /api/debug/log (в тексте команд видны параметры набора).
// Без флага сборки AWF_CMDLOG ручка отвечает 404.
static esp_err_t handle_debug_cmdlog(httpd_req_t *req)
{
#ifdef AWF_CMDLOG
    if (!csrf_check(req)) return ESP_FAIL;
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    size_t cap = cmdlog_glue_json(since, NULL, 0) + 1 + 256;   // +256: запись могла прийти между двумя вызовами
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    size_t n = cmdlog_glue_json(since, buf, cap);
    if (n == 0 || n >= cap) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cmdlog"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
#else
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no cmdlog");
    return ESP_FAIL;
#endif
}

// #FW-50: GET /api/debug/log/meta — JSON status of PSRAM log ring (no CSRF).
// Он же отвечает на GET /api/debug/log/config: тело ответа — один и тот же
// meta_json (настройки живут в тех же полях enabled/level), два отдельных
// обработчика с одинаковым текстом расходились бы при первой же правке.
static esp_err_t handle_debug_log_meta(httpd_req_t *req)
{
    char buf[512];
    debug_log_ring_meta_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// #FW-50: GET /api/debug/log?since=N — text/plain dump.
// Требует X-CSRF-Token, хотя это GET: в дампе лога видно SSID, IP и URL выгрузки,
// а без заголовка любая открытая в браузере сторонняя страница может дёрнуть
// эндпоинт с адреса пользователя. Токен читается только с самой платы
// (GET /api/csrf-token), поэтому drive-by странице он недоступен.
static esp_err_t handle_debug_log_get(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    return debug_log_ring_http_dump(req, since);
}

// #FW-50: POST /api/debug/log/flush — мутирующий запрос, CSRF как у остальных.
// Optional JSON body: {"upto_seq":N,"gen":G}
static esp_err_t handle_debug_log_flush(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint32_t upto = 0, gen = 0;
    int total = req->content_len;
    bool parse_failed = false;

    // Если тело запроса больше 255 байт — не читаем, это ошибка.
    // Если тело отсутствует (total <= 0) — это допустимо, сбрасываем всё.
    if (total >= 256) {
        parse_failed = true;
    } else if (total > 0 && total < 256) {
        char body[256];
        int r = httpd_req_recv(req, body, total);
        if (r <= 0) {
            parse_failed = true;
        } else {
            body[r < 255 ? r : 255] = '\0';
            cJSON *root = cJSON_Parse(body);
            if (root) {
                cJSON *u = cJSON_GetObjectItem(root, "upto_seq");
                cJSON *g = cJSON_GetObjectItem(root, "gen");
                if (cJSON_IsNumber(u)) upto = (uint32_t)u->valuedouble;
                if (cJSON_IsNumber(g)) gen = (uint32_t)g->valuedouble;
                cJSON_Delete(root);
            } else {
                parse_failed = true;
            }
        }
    }
    // total <= 0 — тело отсутствует, это нормально, сбрасываем всё

    if (parse_failed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad flush body");
        return ESP_FAIL;
    }

    esp_err_t e = debug_log_ring_flush(upto, gen);
    if (e == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"gen_mismatch\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flush failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"gen\":%lu}",
             (unsigned long)debug_log_ring_gen());
    return httpd_resp_sendstr(req, buf);
}

// #FW-50: POST /api/debug/log/config — settings. CSRF like other mutating UI.
// GET того же URI обслуживает handle_debug_log_meta (см. выше).
static esp_err_t handle_debug_log_config_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    int total = req->content_len;
    if (total <= 0 || total >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char body[256];
    int r = httpd_req_recv(req, body, total);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        return ESP_FAIL;
    }
    body[r < 255 ? r : 255] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    cJSON *lv = cJSON_GetObjectItem(root, "level");
    bool enabled = debug_log_ring_enabled();
    dbglog_level_t level = debug_log_ring_level();
    if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);
    if (cJSON_IsString(lv) && lv->valuestring) {
        if (!strcmp(lv->valuestring, "debug")) level = DBGLOG_LEVEL_DEBUG;
        else if (!strcmp(lv->valuestring, "detailed")) level = DBGLOG_LEVEL_DETAILED;
        else level = DBGLOG_LEVEL_STANDARD;
    }
    cJSON_Delete(root);
    esp_err_t e = debug_log_ring_set_config(enabled, level);
    if (e != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"alloc_or_nvs\"}");
    }
    char buf[512];
    debug_log_ring_meta_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// #MON-1: серия CPS-мониторинга с платы. GET /api/monitor/series?since=<seq>
// Read-only (CSRF не нужен, как /api/devlog). Ответ:
//   {"epoch":E,"next_seq":N,"first_seq":F,"interval_base":1,
//    "samples":[[end_sec,dur,counts,t_c|null],...]}
// samples — от max(since+1, старейший в кольце), чанк <= 2000 шт; клиент
// дотягивает циклом, пока не догонит next_seq. Стрим httpd_resp_send_chunk
// (паттерн h_export_aswf) — без гигантского JSON-дерева в куче.
#define MON_SERIES_CHUNK 2000   // 2000 × 12 Б = ~24 КБ PSRAM на время запроса

static esp_err_t handle_monitor_series(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);

    size_t cap = MON_SERIES_CHUNK;
    monitor_sample_t *smp = heap_caps_malloc(cap * sizeof(*smp), MALLOC_CAP_SPIRAM);
    if (!smp) { cap = 400; smp = malloc(cap * sizeof(*smp)); }  // fallback: internal, чанк меньше
    char *buf = malloc(2048);
    if (!smp || !buf) {
        free(smp);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    uint32_t first = 0, next = 0, epoch = 0;
    size_t n = monitor_copy_since(since, smp, cap, &first, &next, &epoch);

    httpd_resp_set_type(req, "application/json");
    int pos = snprintf(buf, 2048,
        "{\"epoch\":%" PRIu32 ",\"next_seq\":%" PRIu32 ",\"first_seq\":%" PRIu32
        ",\"interval_base\":1,\"samples\":[", epoch, next, first);
    for (size_t i = 0; i < n; i++) {
        // #MX-12 (1.2.31): 4-й элемент — температура T1, °C (null — нет данных)
        if (smp[i].t_dc == MON_T_NONE)
            pos += snprintf(buf + pos, 2048 - pos, "%s[%" PRIu32 ",%u,%" PRIu32 ",null]",
                            i ? "," : "", smp[i].end_sec, (unsigned)smp[i].dur, smp[i].counts);
        else
            pos += snprintf(buf + pos, 2048 - pos, "%s[%" PRIu32 ",%u,%" PRIu32 ",%s%d.%d]",
                            i ? "," : "", smp[i].end_sec, (unsigned)smp[i].dur, smp[i].counts,
                            smp[i].t_dc < 0 ? "-" : "", abs(smp[i].t_dc) / 10, abs(smp[i].t_dc) % 10);
        if (pos > 1900) { httpd_resp_send_chunk(req, buf, pos); pos = 0; }
    }
    pos += snprintf(buf + pos, 2048 - pos, "]}");
    httpd_resp_send_chunk(req, buf, pos);
    httpd_resp_send_chunk(req, NULL, 0);   // терминатор чанк-стрима
    free(smp);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    const char *cmd = "-rst";
    for (int i = 0; cmd[i]; i++) shproto_packet_add_data(&pkt, cmd[i]);
    shproto_packet_add_data(&pkt, '\0');
    shproto_packet_complete(&pkt);
    // У-2: sent=false — прибор -rst не получил (не подключён/ошибка USB); сброс
    // платы выполнен. Н-4/Н-Д1: -rst дошлётся на ближайшем коннекте, если к тому
    // времени набор прибора не принят по таймауту гейта #58 и не было нового Сброса.
    (void)spectrogram_flush_tail(1200);   // #RST-TAIL: хвост с последней строки до Сброса — строкой водопада, ДО -rst
    uint32_t cl_seq = cmdlog_glue_record(2, cmd, strlen(cmd), 1);   // t1: src 2 rest_reset
    int cl_rc = usb_host_cdc_send(pkt.data, pkt.len);
    cmdlog_glue_result(cl_seq, cl_rc == 0, cl_rc);
    bool sent = cl_rc == 0;
    if (sent) {
        spectrum_reset();
    } else {
        spectrum_reset_undelivered();
        usb_host_cdc_request_rst();   // Н-4: дослать -rst на ближайшем коннекте
    }
    httpd_resp_sendstr(req, sent ? "{\"ok\":true,\"sent\":true}" : "{\"ok\":true,\"sent\":false}");
    return ESP_OK;
}

// AWF-5: GET /api/ota/github/check -- сверка последнего релиза GitHub.
// Ответ строит ota_gh_check() (main/ota_github_client.c).
static esp_err_t handle_ota_gh_check(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;   // S-01 (1.2.30): тяжёлый GET — токен обязателен (drive-by с чужой страницы)
    char resp[560];   // #AWF-6: +html_url (до 200 Б) поверх прежних полей; LK-07: +"state"
    ota_gh_check_async(resp, sizeof(resp));   // LK-07: проверка в фоне, httpd не ждёт GitHub
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// AWF-5: POST /api/ota/github/install -- запускает фоновую задачу
// (ota_github_client.c: install_task); httpd-worker не блокируется на минуты
// сетевого приёма/прошивки.
static esp_err_t handle_ota_gh_install(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    esp_err_t e = ota_gh_install_start();
    if (e == ESP_ERR_NO_MEM) {   // #63: нет памяти/задача не создана — не «уже идёт», UI показывает своё сообщение (503)
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Install task unavailable: out of memory");
        return ESP_FAIL;
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Install already running");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// AWF-5: GET /api/ota/github/progress -- опрос UI во время установки.
static esp_err_t handle_ota_gh_progress(httpd_req_t *req)
{
    ota_gh_progress_t p = ota_gh_get_progress();
    static const char *names[] = {"idle","checking","downloading","verifying",
                                   "installing","done","error"};
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"bytes\":%" PRIu32 ",\"total\":%" PRIu32 ",\"error\":\"%s\"}",
             names[p.state], p.bytes, p.total, p.error);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// AWF-5: GET/POST /api/ota/github/channel -- тумблер "получать предрелизы".
static esp_err_t handle_ota_gh_channel_get(httpd_req_t *req)
{
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"prerelease\":%s}",
             ota_gh_prerelease_channel_get() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_ota_gh_channel_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[64] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(body)) httpd_req_recv(req, body, total);
    bool on = strstr(body, "\"prerelease\":true") != NULL;
    ota_gh_prerelease_channel_set(on);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// AWF-4: POST /api/ota — приём .bin потоком (буфер 4 КБ), запись в неактивный
// OTA-раздел. CSRF как у остальных мутирующих POST. Тело — application/octet-stream
// (не multipart): UI шлёт raw ArrayBuffer, curl — `--data-binary @file.bin`.
// D4 (P3): httpd v5.4 — один task на приём (см. verify-awf4-2026-09-27.md
// D5) — медленный клиент, вечно попадающий в HTTPD_SOCK_ERR_TIMEOUT, держал
// бы его неограниченно, блокируя ВСЕ остальные HTTP-запросы. Порог — число
// ПОДРЯД идущих таймаутов (сброс на каждый успешный recv), не время: точное
// значение recv-таймаута — О9 (release-gate-firmware-v1.2.28-code.md):
// ПЕРЕОПРЕДЕЛЁН этим проектом, `config.recv_wait_timeout = 3` (ниже,
// web_server_init) — 3 с, не дефолт esp_http_server; бюджет простоя httpd
// D4 = 30 × 3 с = 90 с. 30 подряд отказов приёма — явно не "штатная сеть
// подождала", а зависший/враждебный клиент.
#define OTA_MAX_CONSECUTIVE_TIMEOUTS 30u

// P1-фикс (verify-awf5-github-ota-2026-09-27.md разд.2.4): переименована в
// _locked, тело не тронуто -- ota_busy_acquire/release снаружи, одной точкой
// на функцию (иначе пришлось бы трогать все ~8 return-путей ниже).
static esp_err_t handle_ota_locked(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    if ((size_t)total > update->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image too large for partition");
        return ESP_FAIL;
    }
    // В отличие от spectrum_reset(): прервать автосохранение, НЕ удаляя файлы спектра (П-6)
    // (writer не должен драться с OTA-write за flash-freeze/шину). Снимок
    // спектра/водопада на flash — не трогаем, OTA его не касается.
    spectrum_autosave_abort_keep();   // П-6: current.bin не трогать

    esp_ota_handle_t ota = 0;
    // D1: OTA_SIZE_UNKNOWN стирает ВЕСЬ слот сразу (не только Content-Length
    // байт от клиента) — обрезанная запись не оставляет "хвост" старого
    // образа структурно смешанным со свежими байтами.
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AWF-4: esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");
        return ESP_FAIL;
    }

    // Буфер в куче, не на стеке httpd-worker'а (config.stack_size=8192,
    // web_server_init) — тот же приём, что handle_devlog (malloc(9000)).
    const size_t bufsz = 4096;
    uint8_t *buf = malloc(bufsz);
    if (!buf) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int remaining = total, received = 0;
    bool header_checked = false, image_ok = true;
    ota_image_walker_t walker;             // D1: структурная сверка сегментов
    ota_image_walker_init(&walker);
    uint32_t timeout_streak = 0;           // D4: подряд идущие таймауты
    while (remaining > 0) {
        int to_read = remaining < (int)bufsz ? remaining : (int)bufsz;
        int rd = httpd_req_recv(req, (char *)buf, to_read);
        if (rd == HTTPD_SOCK_ERR_TIMEOUT) {
            if (ota_timeout_budget_exceeded(++timeout_streak, OTA_MAX_CONSECUTIVE_TIMEOUTS)) {
                free(buf);
                esp_ota_abort(ota);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Timeout budget exceeded");
                return ESP_FAIL;
            }
            continue;   // повтор, не абор (задача 2)
        }
        timeout_streak = 0;
        if (rd <= 0) {
            free(buf);
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_FAIL;
        }
        if (!header_checked) {
            header_checked = true;
            // main/ota_image_check.h (host-тест tests/host/test_ota_image_check.c):
            // magic 0xE9 + chip_id ДО esp_ota_end() — не пропускаем битый/чужой образ.
            if (!ota_image_header_is_valid((const uint8_t *)buf, (size_t)rd, ESP_CHIP_ID_ESP32S3))
                image_ok = false;
        }
        if (!image_ok) break;
        // D1: подать те же байты, что уходят в esp_ota_write(), в структурный
        // walker — magic/segment_count/пройденность сегментов независимо от
        // Content-Length (см. main/ota_image_check.h).
        if (!ota_image_walker_feed(&walker, (const uint8_t *)buf, (size_t)rd)) {
            image_ok = false;
            break;
        }
        err = esp_ota_write(ota, buf, rd);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "AWF-4: esp_ota_write: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
            return ESP_FAIL;
        }
        remaining -= rd;
        received += rd;
    }
    free(buf);
    if (!image_ok) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad image header (magic/chip_id/segments)");
        return ESP_FAIL;
    }
    // D1 (verify-awf4-2026-09-27.md, дефект D1/T3): Content-Length клиента
    // может совпасть с фактически принятым числом байт — это НЕ доказывает,
    // что образ ЦЕЛ (оба числа под контролем клиента). Структурная сверка:
    // все сегменты, заявленные САМИМ образом, обязаны быть пройдены целиком.
    if (!ota_image_walker_is_complete(&walker)) {
        esp_ota_abort(ota);
        ESP_LOGW(TAG, "AWF-4: D1 guard: image incomplete (%d/%d bytes, walker state=%d)",
                 received, total, (int)walker.state);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete image (truncated)");
        return ESP_FAIL;
    }

    // esp_ota_end() сама проверяет образ (SHA-256/подпись при secure boot) и
    // отвергает мусор эффективнее нашего header-чека выше — тот отбраковывает
    // раньше (не тратит запись всего .bin), а не заменяет эту проверку.
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AWF-4: esp_ota_end (image validation failed): %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ota_end: invalid image");
        return ESP_FAIL;
    }
    // #OTA-VR (ota_busy.h): образ уже прошёл полную проверку в esp_ota_end() выше; повторная проверка
    // внутри set_boot изредка ложно отказывает — до 3 попыток. Отказ esp_ota_end() остаётся окончательным.
    err = ota_set_boot_verified(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AWF-4: esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot_partition");
        return ESP_FAIL;
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%d,\"partition\":\"%s\"}",
             received, update->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    ESP_LOGW(TAG, "AWF-4: OTA written %d bytes to '%s', rebooting", received, update->label);
    // #AUD-F01 (P-016): не терять открытый сегмент. После ответа: httpd однопоточный,
    // пока идёт финализация, UI (system.html pollOtaReboot) ответа старой прошивки не получит.
    spectrogram_prepare_reboot();
    // Как handle_reboot_esp/handle_wifi_reset: ответ уже отдан httpd_resp_sendstr
    // (блокирующий send() успел уйти в TCP-буфер), задержка — дать WiFi/LWIP
    // время реально протолкнуть его в эфир до esp_restart().
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// P1-фикс: общий замок с GitHub-install_task (main/ota_busy.h) -- занято ->
// 409, esp_ota_begin() (внутри _locked) вообще не вызывается.
static esp_err_t handle_ota(httpd_req_t *req)
{
    if (!ota_busy_acquire(OTA_BUSY_MANUAL)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "GitHub OTA install in progress");
        return ESP_FAIL;
    }
    esp_err_t r = handle_ota_locked(req);
    ota_busy_release(OTA_BUSY_MANUAL);   // на успехе _locked уже esp_restart() и сюда не дойдёт
    return r;
}

// #FW-2/#FW-3: GET текущих настроек «Поведение при старте платы» (NVS).
static esp_err_t handle_boot_config_get(httpd_req_t *req)
{
    boot_config_t bc;
    boot_config_load(&bc);
    char resp[420];
    // #FW-42: name_prefix санитизирован в NVS ([A-Za-z0-9_-]) → JSON-escape не нужен.
    // issue #52: + настройки резервных снимков и текущий номер сессии (read-only).
    // AWF-2a финал: field_ap_fallback — прямая семантика (structура тоже
    // положительная теперь, см. boot_config.h), отсутствие ключа в NVS = ВЫКЛ.
    snprintf(resp, sizeof(resp),
        "{\"autostart_spectrum\":%s,\"autostart_waterfall\":%s,"
        "\"clear_spectrum\":%s,\"clear_waterfall\":%s,\"name_prefix\":\"%s\","
        "\"backup_keep\":%u,\"backup_hours\":%u,\"backup_test_minutes\":%s,"
        "\"field_ap_fallback\":%s,\"calib_always\":%s,\"dose_live\":%s,"
        "\"session\":%" PRIu32 "}",
        bc.autostart_spectrum  ? "true" : "false",
        bc.autostart_waterfall ? "true" : "false",
        bc.clear_spectrum      ? "true" : "false",
        bc.clear_waterfall     ? "true" : "false",
        bc.name_prefix,
        (unsigned)bc.backup_keep, (unsigned)bc.backup_hours,
        bc.backup_test_minutes ? "true" : "false",
        bc.field_ap_fallback_enabled ? "true" : "false",
        bc.calib_always_from_device ? "true" : "false",
        bc.dose_live_enabled ? "true" : "false",

        boot_config_get_session());
    {   // #GZ-T (1.2.37-rc90): ключ gzip_level — вместо закрывающей '}' (формат выше не меняется; ответ <= ~310 Б при resp[420])
        size_t rl = strlen(resp);
        if (rl > 0 && resp[rl - 1] == '}') {
            snprintf(resp + rl - 1, sizeof(resp) - (rl - 1), SPEC_GZ_BOOTCFG_TAIL, (unsigned)bc.gzip_level);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// #FW-2/#FW-3: POST настроек. Отсутствующие в теле ключи сохраняют текущее значение.
static esp_err_t handle_boot_config_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    boot_config_t bc;
    boot_config_load(&bc);   // стартуем от текущего, перекрываем переданными ключами
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "autostart_spectrum")))  bc.autostart_spectrum  = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "autostart_waterfall"))) bc.autostart_waterfall = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "clear_spectrum")))      bc.clear_spectrum      = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "clear_waterfall")))     bc.clear_waterfall     = cJSON_IsTrue(it);
    // #FW-42: префикс имён экспортов. boot_config_save() санитизирует ([A-Za-z0-9_-], усечение).
    if ((it = cJSON_GetObjectItem(root, "name_prefix")) && cJSON_IsString(it)) {
        strncpy(bc.name_prefix, it->valuestring, sizeof(bc.name_prefix) - 1);
        bc.name_prefix[sizeof(bc.name_prefix) - 1] = '\0';
    }
    // issue #52: X и Y. Вне диапазона — 400, а не тихий зажим: человек, набравший
    // «48» при пределе 20, должен увидеть отказ, а не молча получить другое число.
    if ((it = cJSON_GetObjectItem(root, "backup_keep")) && cJSON_IsNumber(it)) {
        int v = it->valueint;
        if (v < 0 || v > BOOT_BACKUP_KEEP_MAX) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "backup_keep out of range");
            return ESP_FAIL;
        }
        bc.backup_keep = (uint8_t)v;
    }
    if ((it = cJSON_GetObjectItem(root, "backup_hours")) && cJSON_IsNumber(it)) {
        int v = it->valueint;
        if (v < BOOT_BACKUP_HOURS_MIN || v > BOOT_BACKUP_HOURS_MAX) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "backup_hours out of range");
            return ESP_FAIL;
        }
        bc.backup_hours = (uint16_t)v;
    }
    // Стендовый режим: Y читается как минуты. В UI поля нет намеренно — только API.
    if ((it = cJSON_GetObjectItem(root, "backup_test_minutes")))
        bc.backup_test_minutes = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "field_ap_fallback")))
        bc.field_ap_fallback_enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "calib_always")))     // #59 (1.2.30)
        bc.calib_always_from_device = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "dose_live")))        // #DOSE-TOGGLE (1.2.37): «Считать живую МД»
        bc.dose_live_enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "gzip_level"))) {   // #GZ-T (1.2.37-rc90): тест сжатия спектра, только 0 / 1 / 6
        if (!cJSON_IsNumber(it) || it->valuedouble != (double)it->valueint || !spec_gz_level_valid(it->valueint)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gzip_level must be 0, 1 or 6");
            return ESP_FAIL;
        }
        bc.gzip_level = (uint8_t)it->valueint;
    }

    cJSON_Delete(root);
    int rc = boot_config_save(&bc);
    spectrum_gz_sync();   // #GZ-T (1.2.37-rc90): уровень сжатия применяется сразу (задача сжатия, без перезагрузки)
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    // #3/Codeaudit P1: этот хендлер пишет на Flash (spectrum_save_to_flash), но
    // до сих пор шёл БЕЗ гейта — мог конкурировать с autosave/offload/segment-pull.
    // Короткое ожидание слота (не мгновенный 503): «Сохранить» — клик пользователя,
    // редкий и не автоматический ретрай не нужен, но и мгновенный отказ на пустяковой
    // задержке слота — плохой UX.
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    int idx = spectrum_save_to_flash();
    http_io_gate_leave();
    char resp[64];
    if (idx >= 0)
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d}", idx);
    else {
        // #FW-24: конкретная причина отказа в UI. #FW-59: -4 = слоты исчерпаны.
        const char *err = (idx == -1) ? "novalid" : (idx == -2) ? "nospace"
                         : (idx == -4) ? "exhausted" : "fserr";
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"%s\"}", err);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// 1.2.31: импорт фонового спектра. POST /api/import (НЕ под /api/saved/: ветка POST там — удаление), тело ASI1 = 32896 Б
// собирает браузер (cJSON-разбор на плате невозможен: внутренняя RAM ~16 КБ, крупнейший блок ~7 КБ). Плата только
// принимает, проверяет (spectrum_import_decode) и пишет файл tmp+rename под воротами flash (spectrum_import_to_flash).
static esp_err_t import_reply(httpd_req_t *req, const char *status, const char *err)
{
    char r[64];
    snprintf(r, sizeof(r), "{\"ok\":false,\"err\":\"%s\"}", err);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, r);
    return ESP_OK;
}
// Итог записи: idx >= 0 — номер новой записи; -2 мало места; -4 слоты исчерпаны; -5 flash занят; прочее — ошибка ФС.
static esp_err_t import_reply_idx(httpd_req_t *req, int idx)
{
    if (idx >= 0) {
        char r[48];
        snprintf(r, sizeof(r), "{\"ok\":true,\"index\":%d}", idx);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, r);
        return ESP_OK;
    }
    if (idx == -2) return import_reply(req, "507 Insufficient Storage", "nospace");
    if (idx == -4) return import_reply(req, "409 Conflict", "exhausted");
    if (idx == -5) { httpd_resp_set_hdr(req, "Retry-After", "2"); return import_reply(req, "503 Service Unavailable", "busy"); }
    return import_reply(req, "500 Internal Server Error", "fserr");
}
// Приём тела: куски по 4096, бюджет подряд идущих таймаутов — как у OTA (httpd recv_wait_timeout = 3 с). Без ворот flash.
static bool import_recv_body(httpd_req_t *req, uint8_t *b)
{
    size_t got = 0; uint32_t streak = 0;
    int64_t t0 = esp_timer_get_time();   // #AWF-F3: общий срок приёма — побайтовый клиент не держит слот s_dl_active
    while (got < SPEC_IMPORT_SIZE) {
        if (import_recv_deadline_passed(t0, esp_timer_get_time())) return false;
        size_t want = SPEC_IMPORT_SIZE - got; if (want > 4096) want = 4096;
        int r = httpd_req_recv(req, (char *)b + got, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && !ota_timeout_budget_exceeded(++streak, OTA_MAX_CONSECUTIVE_TIMEOUTS)) continue;
        if (r <= 0) return false;
        streak = 0; got += (size_t)r;
    }
    return true;
}
// Тело задачи web_async_run (стек 6144 Б, на нём только скаляры): буферы — в PSRAM, внутреннюю RAM не трогаем.
static esp_err_t handle_import_job(httpd_req_t *req)
{
    uint8_t *b = heap_caps_malloc(SPEC_IMPORT_SIZE, MALLOC_CAP_SPIRAM);
    spectrum_data_t *sp = heap_caps_malloc(sizeof(*sp), MALLOC_CAP_SPIRAM);
    if (!b || !sp) { free(b); free(sp); import_reply(req, "500 Internal Server Error", "oom"); return ESP_FAIL; }
    bool rx = import_recv_body(req, b);
    imp_err_t e = rx ? spectrum_import_decode(b, SPEC_IMPORT_SIZE, sp) : IMP_BAD_SIZE;
    free(b);
    if (!rx) { free(sp); import_reply(req, "408 Request Timeout", "recv_timeout"); return ESP_FAIL; }   // тело не дочитано — закрыть сессию
    if (e != IMP_OK) { free(sp); return import_reply(req, "400 Bad Request", imp_err_str(e)); }
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) { free(sp); return ESP_OK; }
    int idx = spectrum_import_to_flash(sp);
    http_io_gate_leave();
    free(sp);
    return import_reply_idx(req, idx);
}
// Срок дочитывания чужого тела (идёт в потоке httpd, до web_async_run), мкс. Отдельная константа, не в import_body_plan.h.
// 2 с (было 5). Срок проверяется МЕЖДУ recv, один recv ждёт до recv_wait_timeout 3 с: молчащий клиент держит поток
// до 2+3 = 5 с (слив) и, если после слива тело дочитано не полностью, обработчик возвращает ESP_FAIL (сокет закрывается
// без purge в httpd_req_delete, IDF httpd_sess.c/httpd_uri.c); при ESP_OK purge дочитывал бы остаток БЕЗ срока (F1 прохода 3A).
// 64 КиБ линга по LAN/Wi-Fi уходят за доли секунды. Дошло ли 400/403 при 2 с — на плате НЕ проверено.
#define IMPORT_DRAIN_RECV_DEADLINE_US 2000000LL
// Потолок lingering-дочитывания при CLOSE_400 (тело > IMPORT_DRAIN_MAX), байт.
#define IMPORT_LINGER_MAX_BYTES 65536u
// Читает и отбрасывает до min(content_len, max_bytes) байт входа буфером 512 Б на стеке;
// стоп: лимит, срок IMPORT_DRAIN_RECV_DEADLINE_US, ошибка/0, бюджет таймаутов.
// Возвращает true, только если ВСЁ тело (content_len) прочитано; иначе вызывающий обязан закрыть сокет (ESP_FAIL).
static bool import_drain_input(httpd_req_t *req, size_t max_bytes)
{
    char sink[512];
    bool whole = (req->content_len <= max_bytes);
    size_t remaining = whole ? req->content_len : max_bytes;
    int64_t t0 = esp_timer_get_time();
    uint32_t streak = 0;
    while (remaining > 0) {
        if (esp_timer_get_time() - t0 >= IMPORT_DRAIN_RECV_DEADLINE_US) break;
        size_t want = import_drain_chunk(remaining, sizeof(sink));
        int r = httpd_req_recv(req, sink, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && !ota_timeout_budget_exceeded(++streak, OTA_MAX_CONSECUTIVE_TIMEOUTS)) continue;
        if (r <= 0) break;
        streak = 0;
        remaining -= (size_t)r;
    }
    return whole && remaining == 0;
}
// D-1131-1: DRAIN_400 дочитывает тело малым буфером (иначе RST, клиент теряет 400); CLOSE_400 — ESP_FAIL.
// Правило ответа без полного слива: Connection: close + ESP_FAIL (ESP_OK заставил бы httpd дочитывать остаток без срока).
static esp_err_t handle_import(httpd_req_t *req)
{
    if (!csrf_ok(req)) {   // F2: сначала ограниченный слив, чтобы RST не съел 403 (saved.html повторит запрос по 403)
        bool whole = import_drain_input(req, IMPORT_LINGER_MAX_BYTES);
        if (!whole) httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Bad or missing CSRF token");
        return whole ? ESP_OK : ESP_FAIL;
    }

    switch (import_len_action(req->content_len)) {
        case IMP_LEN_OK: {
            return web_async_run(req, handle_import_job);
        }
        case IMP_LEN_DRAIN_400: {
            bool whole = import_drain_input(req, req->content_len);
            if (!whole) httpd_resp_set_hdr(req, "Connection", "close");
            import_reply(req, "400 Bad Request", "bad_size");
            return whole ? ESP_OK : ESP_FAIL;
        }
        case IMP_LEN_CLOSE_400:
        default: {
            // Lingering close: полный 400 + Connection: close, затем ограниченное дочитывание входа (иначе RST рвёт ответ).
            httpd_resp_set_hdr(req, "Connection", "close");
            import_reply(req, "400 Bad Request", "bad_size");
            import_drain_input(req, IMPORT_LINGER_MAX_BYTES);
            return ESP_FAIL;
        }
    }
}

// #3/Codeaudit P1: буфер листинга — см. комментарий у handle_list.
#define SAVED_LIST_BUF_CAP (64 * 1024)

// issue #52. Хвост ответа /api/list после обоих циклов:
//   "],\"backups\":["                      13 Б
//   "],\"backup_count\":%d,\"session\":%u" 18 + 11 + 10 = 39 Б при предельных значениях
//   ",\"count\":%d}"                       10 + 11      = 21 Б
// Итого 73 Б; берём 96 с запасом. Раньше в цикле стояла константа 32, подобранная под
// прежний короткий хвост "],\"count\":N}" — при удлинении хвоста она перестала быть
// консервативной, и это не «меньше данных в ответе», а выход за буфер (см. ниже).
#define LIST_TAIL_RESERVE 96

// Дописывает в конец буфера с ЗАЖИМОМ длины. snprintf возвращает длину, которая
// ПОНАДОБИЛАСЬ БЫ, а не записанную: при усечении `len += snprintf(...)` уводит len за
// cap, дальше `cap - len` (size_t) становится огромным числом, а httpd_resp_send()
// отдаёт клиенту чужую память за концом буфера. Возвращаем всегда len <= cap-1.
static size_t list_append(char *buf, size_t cap, size_t len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static size_t list_append(char *buf, size_t cap, size_t len, const char *fmt, ...)
{
    if (len >= cap) return cap - 1;          // уже впритык — не пишем ничего
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;                   // ошибка кодирования — длину не двигаем
    size_t add = (size_t)n;
    if (add > cap - len - 1) add = cap - len - 1;   // усечено: считаем фактически записанное
    return len + add;
}

static esp_err_t handle_list(httpd_req_t *req)
{
    // Раньше без гейта, chunk'и слались клиенту живьём во время readdir/fopen —
    // держать гейт так было бы нельзя (слот на скорости клиента). Теперь под
    // гейтом только сбор в буфер PSRAM (Flash I/O); отдача клиенту — без гейта.
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    size_t cap = SAVED_LIST_BUF_CAP;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { cap = 4096; buf = malloc(cap); }
    if (!buf) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    size_t len = list_append(buf, cap, 0, "{\"spectra\":[");
    // issue #52: элемент списка снимков длиннее записи spec_ (имя + session + seq),
    // при предельных u32 и отрицательном saved_at — до ~150 Б; 224 с запасом.
    char path[80], item[224];
    int count = 0;
    // Перечисляем реальные файлы каталога, а не пробуем индексы подряд:
    // save() занимает первую дырку, поэтому после удалений индексы не
    // обязательно идут без разрывов — старый "стоп после 20 пропусков" терял
    // спектры с большими индексами. readdir видит все файлы.
    // #FW-24: спектры лежат в подкаталоге SPEC_DIR (/storage/spec) — отделены
    // от calib.bin/current.bin/wf_state.bin в корне mount. readdir здесь исправен
    // (корень тоже листится; пустой список был из-за %04d-бага в rebuild пути ниже).
    DIR *dir = opendir(SPEC_DIR);
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL) {
        int i = -1;
        if (sscanf(de->d_name, "spec_%d.bin", &i) != 1 || i < 0) continue;
        // Путь восстанавливаем из разобранного индекса (%d ограничен), а не из
        // d_name (%s до 255 байт) — иначе -Werror=format-truncation.
        // #FW-24 КОРЕНЬ БАГА: save пишет spec_%04d.bin (нули), а тут было
        // spec_%d.bin → fopen "spec_0.bin" ≠ "spec_0000.bin" → NULL → continue →
        // КАЖДЫЙ файл пропускался → список всегда пуст. readdir был исправен.
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", SPEC_DIR, i);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t counts = 0, time_sec = 0;
        time_t saved_at = 0;
        fseek(f, offsetof(spectrum_data_t, total_counts), SEEK_SET);
        fread(&counts, 4, 1, f);
        fread(&time_sec, 4, 1, f);
        fseek(f, offsetof(spectrum_data_t, saved_at), SEEK_SET);
        fread(&saved_at, sizeof(time_t), 1, f);
        // 1.2.31: признак импорта — префикс "IMP:" в serial_number (формат файла записи не менялся: чтение отвергает иной размер)
        char tag[4] = {0};
        fseek(f, offsetof(spectrum_data_t, serial_number), SEEK_SET);
        if (fread(tag, 1, 4, f) != 4) memset(tag, 0, sizeof(tag));
        fclose(f);
        int n = snprintf(item, sizeof(item), "%s{\"index\":%d,\"counts\":%" PRIu32 ",\"time\":%" PRIu32 ",\"saved_at\":%ld%s}",
            count > 0 ? "," : "", i, counts, time_sec, (long)saved_at, memcmp(tag, "IMP:", 4) ? "" : ",\"imp\":true");
        // #3: буфер вместо chunk-отправки — см. комментарий над handle_list.
        // issue #52: резерв считается от ФАКТИЧЕСКОГО хвоста (LIST_TAIL_RESERVE),
        // а не от прежней константы 32, рассчитанной на короткий хвост.
        if (n < 0 || len + (size_t)n + LIST_TAIL_RESERVE > cap) break;
        memcpy(buf + len, item, (size_t)n);
        len += (size_t)n;
        count++;
    }
    if (dir) closedir(dir);

    // issue #52: автоматические снимки — вторым массивом, чтобы старый клиент,
    // читающий только "spectra", продолжал работать без изменений.
    len = list_append(buf, cap, len, "],\"backups\":[");
    int bcount = 0;
    DIR *bdir = opendir(BACKUP_DIR);
    while (bdir && (de = readdir(bdir)) != NULL) {
        backup_id_t id;
        if (!backup_parse_name(de->d_name, &id)) continue;
        // Путь собираем из разобранных чисел, а не из d_name (до 255 Б) — иначе
        // -Werror=format-truncation, и это тот же корень, что у #FW-24 выше.
        snprintf(path, sizeof(path), "%s/" BACKUP_NAME_FMT, BACKUP_DIR, id.sess, id.seq);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t counts = 0, time_sec = 0;
        time_t saved_at = 0;
        fseek(f, offsetof(spectrum_data_t, total_counts), SEEK_SET);
        fread(&counts, 4, 1, f);
        fread(&time_sec, 4, 1, f);
        fseek(f, offsetof(spectrum_data_t, saved_at), SEEK_SET);
        fread(&saved_at, sizeof(time_t), 1, f);
        fclose(f);
        int n = snprintf(item, sizeof(item),
            "%s{\"name\":\"bk_%" PRIu32 "_%" PRIu32 "\",\"session\":%" PRIu32 ",\"seq\":%" PRIu32
            ",\"counts\":%" PRIu32 ",\"time\":%" PRIu32 ",\"saved_at\":%ld}",
            bcount > 0 ? "," : "", id.sess, id.seq, id.sess, id.seq,
            counts, time_sec, (long)saved_at);
        if (n < 0 || len + (size_t)n + LIST_TAIL_RESERVE > cap) break;
        memcpy(buf + len, item, (size_t)n);
        len += (size_t)n;
        bcount++;
    }
    if (bdir) closedir(bdir);
    len = list_append(buf, cap, len, "],\"backup_count\":%d,\"session\":%" PRIu32,
                      bcount, boot_config_get_session());
    len = list_append(buf, cap, len, ",\"count\":%d}", count);
    http_io_gate_leave();   // #3: гейт отпущен ДО сетевой отдачи клиенту
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// #AWF-6/bug: страницы встроены в прошивку и отдаются по фиксированному URI без
// версии в пути; без Cache-Control браузер применяет эвристическое кеширование
// и после OTA (тот же /system, новый бинарник) показывает старую вкладку, пока
// её не обновят через F5 -- новые блоки в разметке (напр. AWF-5 "Установить с
// GitHub") тогда не видны, хотя уже зашиты. no-cache = обязательная ревалидация
// (If-Modified-Since/ETag) на КАЖДЫЙ показ, не "не кешировать вовсе".
#define EMBED_HTML_HANDLER(fn,sym) static esp_err_t fn(httpd_req_t *req){ \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end"); \
    httpd_resp_set_type(req,"text/html"); \
    httpd_resp_set_hdr(req,"Cache-Control","no-cache"); \
    httpd_resp_send(req,(const char *)sym##_start, sym##_end - sym##_start); \
    return ESP_OK; }
EMBED_HTML_HANDLER(handle_index,        index_html)
EMBED_HTML_HANDLER(handle_saved_page,   saved_html)
EMBED_HTML_HANDLER(handle_system_page,  system_html)
EMBED_HTML_HANDLER(handle_service_page, service_html)
EMBED_HTML_HANDLER(handle_monitor_page, monitor_html)
EMBED_HTML_HANDLER(handle_captive_page, captive_html)   // #FIELD-10/#FIELD-11: лёгкая captive-landing
// F1 (итоговое ревью 25.09): обёртка handle_captive_page_route (откат метки
// активности через prev-таблицу) убрана вместе со всей prev/cancel-моделью —
// активность теперь поднимает ТОЛЬКО общий трамплин над uris[] (ниже,
// web_server_init), который классифицирует URI ДО вызова любого обработчика;
// /captive регистрируется как раньше, напрямую на handle_captive_page.

// #FIELD-5: общий JS авто-синхронизации времени (application/javascript, не text/html).
static esp_err_t handle_common_time_js(httpd_req_t *req)
{
    extern const uint8_t common_time_js_start[] asm("_binary_common_time_js_start");
    extern const uint8_t common_time_js_end[]   asm("_binary_common_time_js_end");
    httpd_resp_set_type(req, "application/javascript");
    // 1.2.36 (риск аудита S1-1): скрипт вырос до ~17 КБ и уходил целиком на каждый показ страницы. ETag = первые 8 байт SHA-256 ELF
    // (app_elf_sha256: меняется с любой пересборкой, а не только с version.txt -> после OTA старый кеш не подходит) + no-cache:
    // браузер всегда ревалидирует, при совпадении получает 304 без тела. Версия в URL не нужна (страницы не меняются).
    char etag[20];
    const esp_app_desc_t *ad = esp_app_get_description();
    if (ad) {
        static const char hx[] = "0123456789abcdef";
        etag[0] = '"';
        for (int i = 0; i < 8; i++) {
            etag[1 + 2 * i] = hx[ad->app_elf_sha256[i] >> 4];
            etag[2 + 2 * i] = hx[ad->app_elf_sha256[i] & 15];
        }
        etag[17] = '"';
        etag[18] = 0;
        httpd_resp_set_hdr(req, "ETag", etag);
        char inm[24];
        if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, etag) == 0) {
            httpd_resp_set_status(req, "304 Not Modified");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            return httpd_resp_send(req, NULL, 0);
        }
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)common_time_js_start, common_time_js_end - common_time_js_start);
    return ESP_OK;
}

static esp_err_t render_spectrum_xml(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/xml");
    char named[80];   // issue #52: префикс (до 23) + "S<u32>_<u32>.csv" (26) не влезали в 48
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[128];  // issue #52: "attachment; filename=\"\"" (22) + named[80]
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    httpd_resp_sendstr_chunk(req,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<ResultDataFile xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        "  <FormatVersion>120920</FormatVersion>\r\n"
        "  <ResultDataList>\r\n"
        "    <ResultData>\r\n");

    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts, te;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    localtime_r(&end_time, &te);

    // #FIELD-9 (A11): near-epoch → XML-комментарий (валиден, парсеры игнорируют); только при рассинхроне
    if (!time_is_synced(end_time))
        httpd_resp_sendstr_chunk(req, "      <!-- TIME NOT SYNCHRONIZED: board clock near-epoch, timestamps unreliable -->\r\n");

    char serial_esc[160];
    web_xml_escape(sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
               serial_esc, sizeof(serial_esc));
    int n = snprintf(buf, 4096,
        "      <SampleInfo>\r\n"
        "        <Name>%s</Name>\r\n"
        "        <Location />\r\n"
        "        <Time>%04d-%02d-%02dT%02d:%02d:%02d</Time>\r\n"
        "        <Weight>1</Weight>\r\n"
        "        <Volume>1</Volume>\r\n"
        "        <Note />\r\n"
        "      </SampleInfo>\r\n"
        "      <DeviceConfigReference>\r\n"
        "        <Name>Atom Spectra</Name>\r\n"
        "        <Guid>00000000-0000-0000-0000-000000000000</Guid>\r\n"
        "      </DeviceConfigReference>\r\n"
        "      <StartTime>%04d-%02d-%02dT%02d:%02d:%02d</StartTime>\r\n"
        "      <EndTime>%04d-%02d-%02dT%02d:%02d:%02d</EndTime>\r\n"
        "      <PresetTime>0</PresetTime>\r\n",
        serial_esc,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        te.tm_year+1900, te.tm_mon+1, te.tm_mday, te.tm_hour, te.tm_min, te.tm_sec);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 4096,
        "      <EnergySpectrum>\r\n"
        "        <NumberOfChannels>%d</NumberOfChannels>\r\n"
        "        <ChannelPitch>1</ChannelPitch>\r\n",
        SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, n);

    if (calib_export_present(sp)) {          // R7 (sweep-A): нули/NaN = «не задана»
        n = snprintf(buf, 4096,
            "        <EnergyCalibration>\r\n"
            "          <PolynomialOrder>%d</PolynomialOrder>\r\n"
            "          <Coefficients>\r\n",
            sp->calib_order);
        httpd_resp_send_chunk(req, buf, n);
        for (int i = 0; i <= sp->calib_order; i++) {
            n = snprintf(buf, 4096,
                "            <Coefficient>%.15G</Coefficient>\r\n",
                sp->calibration[i]);
            httpd_resp_send_chunk(req, buf, n);
        }
        httpd_resp_sendstr_chunk(req,
            "          </Coefficients>\r\n"
            "        </EnergyCalibration>\r\n");
    }

    float live_time = spectrum_compute_live_time(sp);

    n = snprintf(buf, 4096,
        "        <ValidPulseCount>%" PRIu32 "</ValidPulseCount>\r\n"
        "        <TotalPulseCount>%" PRIu32 "</TotalPulseCount>\r\n"
        "        <MeasurementTime>%" PRIu32 "</MeasurementTime>\r\n"
        "        <LiveTime>%.1f</LiveTime>\r\n"
        "        <NumberOfSamples>1</NumberOfSamples>\r\n"
        "        <Spectrum>\r\n",
        sp->total_counts,
        sp->total_counts + sp->lost_impulses,
        sp->total_time_sec, live_time);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos,
                "          <DataPoint>%" PRIu32 "</DataPoint>\r\n", sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_sendstr_chunk(req,
        "        </Spectrum>\r\n"
        "      </EnergySpectrum>\r\n"
        "      <PulseCollection>\r\n"
        "        <Format>Base64 encoded binary</Format>\r\n"
        "        <Pulses />\r\n"
        "      </PulseCollection>\r\n"
        "    </ResultData>\r\n"
        "  </ResultDataList>\r\n"
        "</ResultDataFile>\r\n");

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t render_spectrum_csv(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/csv");
    char named[80];   // issue #52: префикс (до 23) + "S<u32>_<u32>.csv" (26) не влезали в 48
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[128];  // issue #52: "attachment; filename=\"\"" (22) + named[80]
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    // #CSV-1 (2026-06-28): нативный формат Atom Spectra. Эталон оператора —
    // "spectrum (10).csv": одна строка-заголовок "Channel,Counts (TotalTime=<realtime>.0s)",
    // далее "канал,счёт" с канала 0 (не 1), без пробела после запятой, окончания CRLF,
    // 8192 канала. TotalTime = реальное (измерительное) время total_time_sec, формат %.1f.
    // Перекрывает прежний InterSpec/SpecUtils-заголовок #PR-2 по явному указанию оператора.
    // #FIELD-9 (A11): near-epoch → пометка в скобке TotalTime (одна строка-заголовок цела); только при рассинхроне
    time_t csv_ref = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    int n = time_is_synced(csv_ref)
        ? snprintf(buf, 4096, "Channel,Counts (TotalTime=%.1fs)\r\n", (double)sp->total_time_sec)
        : snprintf(buf, 4096, "Channel,Counts (TotalTime=%.1fs; TIME NOT SYNCHRONIZED)\r\n", (double)sp->total_time_sec);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 200 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos, "%d,%" PRIu32 "\r\n", i, sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// #EXP-1 (2026-06-28): нативный N42-2011 экспорт. Эталон оператора — "spectrum (10).N42"
// (родная выгрузка Atom Spectra через BecqMoni). Структура по эталону, КРОМЕ StartDateTime (ISO UTC с Z, #AWF-F2,
// а не dd.mm.yyyy местного, как у эталона и прошивок <=1.2.31): UTF-8 BOM, CRLF, БЕЗ финального
// перевода строки, отступы 2 пробела, блок RadInstrumentInformation дословно, CoefficientValues
// и ChannelData с замыкающим пробелом. Значения — живые из снимка спектра.
static esp_err_t render_spectrum_n42(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char named[80];   // issue #52: префикс (до 23) + "S<u32>_<u32>.csv" (26) не влезали в 48
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[128];  // issue #52: "attachment; filename=\"\"" (22) + named[80]
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint32_t r0 = esp_random(), r1 = esp_random(), r2 = esp_random(), r3 = esp_random();
    char uuid[40];
    snprintf(uuid, sizeof(uuid), "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)r0,
             (unsigned long)(r1 >> 16), (unsigned long)(r1 & 0xffff),
             (unsigned long)(r2 >> 16), (unsigned long)(r2 & 0xffff),
             (unsigned long)r3);
    int n = snprintf(buf, 4096,
        "\xEF\xBB\xBF"
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<RadInstrumentData xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
        " n42DocUUID=\"%s\" xmlns=\"http://physics.nist.gov/N42/2011/N42\">\r\n",
        uuid);
    httpd_resp_send_chunk(req, buf, n);
    // #FIELD-9 (A11): near-epoch → Remark в начале RadInstrumentData (схема N42-2011 разрешает); только при рассинхроне
    {
        time_t et = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
        if (!time_is_synced(et))
            httpd_resp_sendstr_chunk(req, "  <Remark>TIME NOT SYNCHRONIZED: board clock near-epoch, StartDateTime unreliable</Remark>\r\n");
    }
    httpd_resp_sendstr_chunk(req,
        "  <RadInstrumentInformation id=\"RadInstrument\">\r\n"
        "    <RadInstrumentManufacturerName>KB Radar</RadInstrumentManufacturerName>\r\n"
        "    <RadInstrumentModelName>ATOM Spectra</RadInstrumentModelName>\r\n"
        "    <RadInstrumentClassCode>Radionuclide Identifier</RadInstrumentClassCode>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Hardware</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion />\r\n"
        "    </RadInstrumentVersion>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>SoftwareName</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>BecqMoni</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Software</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>2026.6.15.1</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "  </RadInstrumentInformation>\r\n");
    const bool have_cal = calib_export_present(sp);   // R7 (sweep-A): один признак на файл
    if (have_cal) {
        httpd_resp_sendstr_chunk(req,
            "  <EnergyCalibration id=\"SpectrumCalibration-0\">\r\n"
            "    <CoefficientValues>");
        int cpos = 0;
        for (int i = 0; i <= sp->calib_order; i++)
            cpos += snprintf(buf + cpos, 4096 - cpos, "%.15G ", sp->calibration[i]);
        httpd_resp_send_chunk(req, buf, cpos);
        httpd_resp_sendstr_chunk(req,
            "</CoefficientValues>\r\n"
            "  </EnergyCalibration>\r\n");
    }
    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    time_t t_start = end_time - sp->total_time_sec;
    char start_iso[24];   // #AWF-F2: UTC с Z; импорт платы читает StartDateTime как UTC
    if (!n42_start_iso_utc((int64_t)t_start, start_iso, sizeof(start_iso))) strcpy(start_iso, "1970-01-01T00:00:00Z");
    n = snprintf(buf, 4096,
        "  <RadMeasurement id=\"SpectrumMeasurement-0\">\r\n"
        "    <MeasurementClassCode>Foreground</MeasurementClassCode>\r\n"
        "    <StartDateTime>%s</StartDateTime>\r\n"
        "    <RealTimeDuration>PT%" PRIu32 "S</RealTimeDuration>\r\n",
        start_iso, sp->total_time_sec);
    httpd_resp_send_chunk(req, buf, n);
    if (have_cal) {
        httpd_resp_sendstr_chunk(req,
            "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\""
            " energyCalibrationReference=\"SpectrumCalibration-0\">\r\n");
    } else {
        httpd_resp_sendstr_chunk(req,
            "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\">\r\n");
    }
    n = snprintf(buf, 4096,
        "      <LiveTimeDuration>PT%.1fS</LiveTimeDuration>\r\n"
        "      <ChannelData compressionCode=\"None\">",
        spectrum_compute_live_time(sp));
    httpd_resp_send_chunk(req, buf, n);
    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++)
            pos += snprintf(buf + pos, 4096 - pos, "%" PRIu32 " ", sp->bins[i]);
        httpd_resp_send_chunk(req, buf, pos);
    }
    n = snprintf(buf, 4096,
        "</ChannelData>\r\n"
        "    </Spectrum>\r\n"
        "    <GrossCounts id=\"GrossForeground\" radDetectorInformationReference=\"Detector\">\r\n"
        "      <TotalCounts>%" PRIu32 "</TotalCounts>\r\n"
        "    </GrossCounts>\r\n"
        "  </RadMeasurement>\r\n"
        "</RadInstrumentData>",
        sp->total_counts + sp->lost_impulses);
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// #EXP-2 (2026-06-28): экспорт ЛСРМ SpectraLine *.spe (бинарный вариант), как у SpectraVibe
// (gamma.io.lsrm_spe.write_lsrm_spe): CP-1251 заголовок KEY=VALUE\r\n, маркер "SPECTR=", затем
// бинарный блок счётов uint32 little-endian (по каналу, без разделителей). Значения ASCII.
static esp_err_t render_spectrum_spe(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char named[80];   // issue #52: префикс (до 23) + "S<u32>_<u32>.csv" (26) не влезали в 48
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[128];  // issue #52: "attachment; filename=\"\"" (22) + named[80]
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    int pos = snprintf(buf, 4096,
        "SHIFR=%s\r\n"
        "CONFIGNAME=Atom Spectra\r\n"
        "MEASBEGIN=%02d-%02d-%02d %02d:%02d:%02d.00\r\n"
        "TLIVE=%.2f\r\n"
        "TREAL=%.2f\r\n"
        "DETECTOR=Atom Spectra\r\n",
        sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
        ts.tm_mday, ts.tm_mon + 1, (ts.tm_year + 1900) % 100,
        ts.tm_hour, ts.tm_min, ts.tm_sec,
        spectrum_compute_live_time(sp), (double)sp->total_time_sec);
    // #FIELD-9 (A11): near-epoch → строка COMMENT (SPE игнорирует неизвестные ключи); только при рассинхроне
    if (!time_is_synced(end_time))
        pos += snprintf(buf + pos, 4096 - pos, "COMMENT=TIME NOT SYNCHRONIZED (board clock near-epoch)\r\n");
    if (calib_export_present(sp)) {          // R7 (sweep-A): нули/NaN = «не задана»
        double emax = 0.0;
        for (int i = sp->calib_order; i >= 0; i--)
            emax = emax * (SPECTRUM_CHANNELS - 1) + sp->calibration[i];
        if (emax < 0.0) emax = 0.0;
        pos += snprintf(buf + pos, 4096 - pos, "ENBOUNDS=0,%d\r\n", (int)(emax + 0.5));
        pos += snprintf(buf + pos, 4096 - pos, "ENERGY=%d", sp->calib_order);
        for (int i = 0; i < 7; i++) {
            double c = (i <= sp->calib_order) ? sp->calibration[i] : 0.0;
            pos += snprintf(buf + pos, 4096 - pos, ",%.10g", c);
        }
        pos += snprintf(buf + pos, 4096 - pos, "\r\n");
    }
    pos += snprintf(buf + pos, 4096 - pos, "SPECTRSIZE=%d\r\nSPECTR=", SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, pos);
    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int cnt = SPECTRUM_CHANNELS - i;
        if (cnt > 1024) cnt = 1024;
        httpd_resp_send_chunk(req, (const char *)&sp->bins[i], cnt * 4);
        i += cnt;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_export_xml(httpd_req_t *req)
{
    if (!http_io_gate_enter_or_503(req)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_xml(req, sp, "spectrum.xml");
    free(sp);
    http_io_gate_leave();
    return ret;
}

static esp_err_t handle_export_spe(httpd_req_t *req)
{
    if (!http_io_gate_enter_or_503(req)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_spe(req, sp, "spectrum.spe");
    free(sp);
    http_io_gate_leave();
    return ret;
}

static esp_err_t handle_export_n42(httpd_req_t *req)
{
    if (!http_io_gate_enter_or_503(req)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_n42(req, sp, "spectrum.n42");
    free(sp);
    http_io_gate_leave();
    return ret;
}

static esp_err_t handle_export_csv(httpd_req_t *req)
{
    if (!http_io_gate_enter_or_503(req)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_csv(req, sp, "spectrum.csv");
    free(sp);
    http_io_gate_leave();
    return ret;
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req);
static esp_err_t handle_saved_export_csv(httpd_req_t *req);
static esp_err_t handle_saved_json(httpd_req_t *req);
// AWF-2a финал: обработчик ошибки 404 esp_http_server (регистрация ниже, у
// httpd_register_err_handler — определение рядом с open_fn/activity slots).
// Срабатывает ТОЛЬКО когда uri_match_fn не нашёл НИ ОДНОГО зарегистрированного
// обработчика; httpd_resp_send_err(...,HTTPD_404_NOT_FOUND,...) ВНУТРИ уже
// СОВПАВШЕГО обработчика (web_server.c/web_waterfall.c/spectrum_http_cache.c) —
// другой путь esp_http_server, этим handle_404 не перехватывается, не трогать.
static esp_err_t handle_404(httpd_req_t *req, httpd_err_code_t err);

static esp_err_t handle_saved_get(httpd_req_t *req)
{
    const char *p = req->uri + 11;
    while (*p >= '0' && *p <= '9') p++;
    if (strcmp(p, "/export.xml") == 0) return handle_saved_export_xml(req);
    if (strcmp(p, "/export.csv") == 0) return handle_saved_export_csv(req);
    if (strcmp(p, "/spectrum.json") == 0) return handle_saved_json(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown saved action");
    return ESP_FAIL;
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    // #3/Codeaudit P1: read из Flash (spectrum_load_from_flash) шёл без гейта.
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    http_io_gate_leave();   // #3: файл уже прочитан в sp — рендер идёт БЕЗ гейта
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.xml", idx);
    esp_err_t ret = render_spectrum_xml(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_export_csv(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    http_io_gate_leave();
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.csv", idx);
    esp_err_t ret = render_spectrum_csv(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_json(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    http_io_gate_leave();
    esp_err_t ret = render_spectrum_json(req, sp);
    free(sp);
    return ret;
}

// ─── issue #52: снимки /api/backup/<name>/… ──────────────────────────────────
// Имя приходит из URI, поэтому единственная его проверка — backup_parse_name()
// внутри spectrum_backup_*: всё, что не "bk_<u32>_<u32>.bin", отвергается, и ни
// "..", ни "/" в путь попасть не могут.
#define BACKUP_URI_PREFIX     "/api/backup/"
#define BACKUP_URI_PREFIX_LEN (sizeof(BACKUP_URI_PREFIX) - 1)

// "/api/backup/bk_3_2/export.csv" → name="bk_3_2.bin", tail="/export.csv".
// false, если имя не влезло в буфер или префикса нет.
static bool parse_backup_uri(const char *uri, char *name, size_t cap, const char **tail)
{
    if (strncmp(uri, BACKUP_URI_PREFIX, BACKUP_URI_PREFIX_LEN) != 0) return false;
    const char *p = uri + BACKUP_URI_PREFIX_LEN;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0 || len + 5 > cap) return false;   // +5: ".bin" и '\0'
    memcpy(name, p, len);
    // В URI имя без расширения (короче и читаемо в адресной строке); на ФС —
    // с ".bin", как его пишет spectrum_backup_save().
    memcpy(name + len, ".bin", 5);
    if (tail) *tail = slash ? slash : "";
    return true;
}

// Загружает снимок под гейтом. Сам отвечает клиенту при отказе.
// Возвращает NULL, если ответ уже отправлен.
static spectrum_data_t *backup_load_or_fail(httpd_req_t *req, backup_id_t *id_out)
{
    char name[40];
    if (!parse_backup_uri(req->uri, name, sizeof(name), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad backup name");
        return NULL;
    }
    if (id_out) backup_parse_name(name, id_out);
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return NULL;
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return NULL;
    }
    if (spectrum_backup_load(name, sp) != 0) {
        free(sp);
        http_io_gate_leave();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Backup not found");
        return NULL;
    }
    http_io_gate_leave();   // файл прочитан — рендер без гейта, как у saved
    return sp;
}

static esp_err_t handle_backup_get(httpd_req_t *req)
{
    char name[40];
    const char *tail = "";
    if (!parse_backup_uri(req->uri, name, sizeof(name), &tail)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad backup name");
        return ESP_FAIL;
    }
    bool xml = (strcmp(tail, "/export.xml") == 0);
    bool csv = (strcmp(tail, "/export.csv") == 0);
    bool jsn = (strcmp(tail, "/spectrum.json") == 0);
    if (!xml && !csv && !jsn) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown backup action");
        return ESP_FAIL;
    }
    backup_id_t id = {0, 0};
    spectrum_data_t *sp = backup_load_or_fail(req, &id);
    if (!sp) return ESP_OK;      // ответ уже отправлен
    esp_err_t ret;
    if (jsn) {
        ret = render_spectrum_json(req, sp);
    } else {
        // Имя выгрузки говорит человеку, из какой сессии снимок: "S3_2.csv".
        char fn[32];
        snprintf(fn, sizeof(fn), "S%" PRIu32 "_%" PRIu32 ".%s", id.sess, id.seq,
                 xml ? "xml" : "csv");
        ret = xml ? render_spectrum_xml(req, sp, fn) : render_spectrum_csv(req, sp, fn);
    }
    free(sp);
    return ret;
}

static esp_err_t handle_backup_delete(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char name[40];
    if (!parse_backup_uri(req->uri, name, sizeof(name), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad backup name");
        return ESP_FAIL;
    }
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    int rc = spectrum_backup_delete(name);
    http_io_gate_leave();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}
// ─── конец issue #52 ─────────────────────────────────────────────────────────

static esp_err_t handle_saved_delete(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    // 1.2.31: удаление — ТОЛЬКО точный путь /api/saved/<i>/delete. Раньше любой POST /api/saved/* (в т.ч. /api/saved/abc)
    // шёл через atoi и стирал spec_0000.bin. Остальное — 404, ничего не удаляется (saved_delete_index — host-тест).
    int idx = saved_delete_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown saved action");
        return ESP_FAIL;
    }
    if (!http_io_gate_enter_wait_or_503(req, SAVED_FLASH_GATE_WAIT_MS)) return ESP_OK;
    int rc = spectrum_delete_from_flash(idx);
    http_io_gate_leave();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_device(httpd_req_t *req)
{
    const device_info_t *di = spectrum_get_device_info();
    // Снимок под локом: serial/calibration пишутся CDC-задачей конкурентно.
    // #PERF-4: только метаданные — bins[] здесь не нужны (используются
    // serial_number/calibration/calib_order/calib_valid). Полный снимок держал
    // SPEC_LOCK на 32 КБ и отнимал 8,5 % свипов у приёма USB.
    // 1.2.36 H12: и буфер на ~33 КБ под этот снимок не выделяется — пять нужных полей
    // (~110 Б) лежат в локальной структуре на стеке httpd.
    spectrum_calib_meta_t m;
    bool have_sp = spectrum_get_calib_meta(&m);
    cJSON *root = cJSON_CreateObject();
    if (!root) {  // P2-7: при OOM cJSON_Add* разыменует NULL
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "valid", di->valid);
    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "rise", di->rise);
        cJSON_AddNumberToObject(root, "fall", di->fall);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "max_integral", di->max_integral);
        cJSON_AddNumberToObject(root, "hyst", di->hyst);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "step", di->step);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "pot", di->pot);
        json_add_temp(root, "t1", di->t1);
        json_add_temp(root, "t2", di->t2);
        json_add_temp(root, "t3", di->t3);
        cJSON_AddBoolToObject(root, "tc_on", di->tc_on);
        cJSON_AddNumberToObject(root, "tp", di->tp);
    }
    if (have_sp && m.serial_number[0])
        cJSON_AddStringToObject(root, "serial", m.serial_number);
    if (have_sp && m.calib_valid) {
        cJSON *cal = cJSON_CreateArray();
        for (int i = 0; i <= m.calib_order; i++)
            cJSON_AddItemToArray(cal, cJSON_CreateNumber(m.calibration[i]));
        cJSON_AddItemToObject(root, "calibration", cal);
        cJSON_AddNumberToObject(root, "calib_order", m.calib_order);
    }
    // #AWF-12b F2 (release-gate-1.2.28-code.md): ОДИН признак "калибровка
    // задана" для прошивки и всех страниц — тот же предикат, что гейт
    // авто-считывания (usb_host_cdc.c). Раньше страницы решали каждая по-
    // своему (наличие поля "calibration" ИЛИ хотя бы один коэффициент != 0);
    // прошивка отдаёт "calibration" уже при calib_valid, ДАЖЕ если все
    // коэффициенты нулевые (старый calib.bin с v1.2.27, POST /api/calibration
    // с нулями) — index.html/waterfall.html читали "Задана", system.html
    // "не задана" одновременно. Поле — ВСЕГДА (не только при calib_valid),
    // чтобы страницы могли отличить "прошивка новая, калибровки нет" от
    // "прошивка старая, поля нет вовсе" без домысливания.
    // #AWF-12b R4 (release-gate-1.2.28-code-rc2.md): признак — ТЕМ ЖЕ снимком
    // sp, что и массив "calibration" выше (было отдельным вызовом
    // spectrum_calibration_is_missing() под своим SPEC_LOCK — окно между двумя
    // локами, куда мог встать разбор дампа usb_rxw, давало "calib_set":true
    // без "calibration"). have_sp==false — тоже "не задана".
    cJSON_AddBoolToObject(root, "calib_set",
        have_sp && !calib_is_missing(m.calibration, CALIB_COEFFS, m.calib_valid));
    // F12/RO1 (release-gate-1.2.28-code-fixes.md:128): счётчик отвергнутых
    // -cal (CRC ok, нули/NaN) — web/service.html "Считать" сверяет "до"/"после".
    cJSON_AddNumberToObject(root, "calib_reject_seq", spectrum_get_calib_reject_seq());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (!json) httpd_resp_set_status(req, "503 Service Unavailable");   // F-09: нет памяти на JSON
    httpd_resp_sendstr(req, json ? json : "{\"ok\":false,\"err\":\"oom\"}");
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_reboot_device(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_REBOOT);
    shproto_packet_complete(&pkt);
    usb_host_cdc_acq_intent_device_reboot();  // сторож набора вернёт набор после перезагрузки
    uint32_t cl_seq = cmdlog_glue_record(4, "[CMD_REBOOT]", 12, 0);   // t1: src 4 web_ui; кадр CMD_REBOOT без текста
    int ret = usb_host_cdc_send(pkt.data, pkt.len);
    cmdlog_glue_result(cl_seq, ret == 0, ret);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_system(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {  // P2-7: при OOM cJSON_Add* разыменует NULL
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    // #FW-28: версия прошивки ESP32 из esp_app_get_description()->version
    // (= PROJECT_VER = содержимое version.txt в корне проекта). Клиент сверяет
    // с latest release VibeEngineering-LLC/atomspectra-waterfall-esp32 через GitHub API.
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app_desc ? app_desc->version : "?");
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    // #MON-3: PSRAM отдельной строкой. Общая куча складывает internal и SPIRAM, по
    // ней нельзя судить, влезет ли следующее крупное кольцо: решение «расширить
    // историю мониторинга» до сих пор принималось по косвенной цифре. *_free и
    // *_min — счётчики аллокатора; *_largest — ОБХОД блоков кучи в критической
    // секции (У-4): его цена публикуется в heap_walk_us.
    // 1.2.36 H5 (C): обходы кучи (*_largest, heap_walk_us) и опрос стеков задач — МЕДЛЕННАЯ часть: значение живёт
    // SYS_SLOW_PERIOD_US (10 с), возраст — heap_walk_age_ms. Счётчики аллокатора (*_free, *_min) читаются на каждый запрос.
    static const char *const stk_names[] = { "usb_conn", "tcp_rx", "esp_timer", "sys_evt",
                                             "httpd", "wf_fs", "wf", "usb_rxw", "tcp_tx" };
    enum { STK_N = sizeof(stk_names) / sizeof(stk_names[0]) };
    static uint32_t s_heap_walk_n;   // число ОБХОДОВ кучи с загрузки (наблюдаемость, Commit 0)
    static int64_t  s_slow_at = 0;   // 0 = ещё не читали
    static size_t   s_psram_largest, s_int_largest, s_dflt_largest;
    static int64_t  s_walk_us;
    static int32_t  s_stk_min[STK_N];   // -1 = такой задачи нет
    int64_t slow_now = esp_timer_get_time();
    if (ttl_expired(s_slow_at, slow_now, SYS_SLOW_PERIOD_US)) {
        s_heap_walk_n++;
        s_psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        s_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        s_dflt_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
        s_walk_us = esp_timer_get_time() - slow_now;
        for (size_t i = 0; i < STK_N; i++) {
            TaskHandle_t th = xTaskGetHandle(stk_names[i]);
            s_stk_min[i] = th ? (int32_t)uxTaskGetStackHighWaterMark(th) : -1;
        }
        s_slow_at = slow_now;
    }
    size_t psram_largest = s_psram_largest, int_largest = s_int_largest, dflt_largest = s_dflt_largest;
    cJSON_AddNumberToObject(root, "heap_walk_us", (double)s_walk_us);
    cJSON_AddNumberToObject(root, "heap_walk_age_ms", (double)((slow_now - s_slow_at) / 1000));
    cJSON_AddNumberToObject(root, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "psram_largest", psram_largest);
    // Wi-Fi берёт буферы кадров из internal (#FW-50). int_* включают DMA-резерв
    // SPIRAM_MALLOC_RESERVE_INTERNAL (32 КБ), недоступный malloc()/cJSON/httpd (У-5);
    // int_dflt_* — только то, что доступно обычному malloc().
    cJSON_AddNumberToObject(root, "int_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "int_largest", int_largest);
    cJSON_AddNumberToObject(root, "int_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "int_dflt_free",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(root, "int_dflt_largest", dflt_largest);
    uint32_t af_n, af_size, af_caps, cj_spill;
    mem_diag_get(&af_n, &af_size, &af_caps, &cj_spill);
    cJSON_AddNumberToObject(root, "cjson_spill", cj_spill);      // cJSON ушёл из PSRAM во внутреннюю
    cJSON_AddNumberToObject(root, "alloc_fail", af_n);            // отказы аллокации с загрузки
    cJSON_AddNumberToObject(root, "alloc_fail_last_size", af_size);
    cJSON_AddNumberToObject(root, "alloc_fail_last_caps", af_caps);
    // #AUD-DIAG-1 R5 (часть; F-12/C-38/#RB-STK-1): минимум свободного стека за время работы, байт
    // (ESP-IDF: uxTaskGetStackHighWaterMark — в байтах). Нет задачи — поле не пишется.
    cJSON *stk = cJSON_AddObjectToObject(root, "stack_min_free");
    for (size_t i = 0; stk && i < STK_N; i++) {
        if (s_stk_min[i] >= 0) cJSON_AddNumberToObject(stk, stk_names[i], (double)s_stk_min[i]);
    }
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "usb_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());
    // #PERF-3 (P-014): esp_littlefs_info() не читает готовый счётчик, а ОБХОДИТ
    // раздел — ~290 мс на 112 МБ. Страницы дёргают /api/system каждые 2-5 с, всё
    // это время флеш занята, кэш обоих ядер заморожен (до XIP из PSRAM, П-3), USB не вычитывается ->
    // FIFO переходника переполняется -> битый CRC -> свип бракуется. Замер:
    // 19,1 % потерянных свипов против 0 % в покое. Значение чисто справочное для
    // UI и меняется медленно (ролловер сегмента раз в сотни секунд), поэтому
    // кэшируем. В make_room()/spectrum_save кэш НЕ применять: там решается,
    // хватает ли места, устаревшее значение переполнит флеш.
    static size_t s_fs_total = 0, s_fs_used = 0;
    static int64_t s_fs_at = 0;                 // 0 = ещё не читали
    int64_t now_us = esp_timer_get_time();
    if (ttl_expired(s_fs_at, now_us, 30LL * 1000000LL)) {
        size_t t = 0, u = 0;
        if (esp_littlefs_info("storage", &t, &u) == ESP_OK) {
            s_fs_total = t; s_fs_used = u; s_fs_at = now_us;
        }
    }
    cJSON_AddNumberToObject(root, "flash_total", (double)s_fs_total);
    cJSON_AddNumberToObject(root, "flash_used", (double)s_fs_used);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddStringToObject(root, "ssid", (char *)ap.ssid);
    }
    // #FIELD-6: сетевой режим + статус полевого AP + источник времени (для system.html).
    switch (wifi_manager_mode()) {
        case NET_MODE_FIELD_AP: cJSON_AddStringToObject(root, "net_mode", "field_ap"); break;
        case NET_MODE_SETUP:    cJSON_AddStringToObject(root, "net_mode", "setup");    break;
        default:                cJSON_AddStringToObject(root, "net_mode", "sta");      break;
    }
    cJSON_AddStringToObject(root, "ap_ssid",        wifi_manager_ap_ssid());
    cJSON_AddNumberToObject(root, "ap_clients",     wifi_manager_ap_clients());
    cJSON_AddBoolToObject(root,   "ap_pass_default", wifi_manager_ap_pass_is_default());
    cJSON_AddBoolToObject(root,   "ap_forced",      wifi_manager_ap_forced());
    // AWF-2a доработка: диагностика застревания в Field AP (живой тест 25.09,
    // 11 минут без следа) — причина последнего блока возврата + давность HTTP.
    cJSON_AddStringToObject(root, "return_block_reason", wifi_manager_return_block_reason());
    // F1: справочные поля — 0/false, пока открыт хоть один user-сокет
    // (гейт возврата смотрит НЕ на них, а на web_server_http_activity_quiet()).
    cJSON_AddNumberToObject(root, "http_idle_s", (double)web_server_http_idle_s());
    cJSON_AddStringToObject(root, "time_source",    net_time_source_str());
    cJSON_AddBoolToObject(root,   "sntp_synced",    net_time_sntp_synced());
    // #PERF-1/#PERF-2: observability for multi-tab spectrum cache + HEAVY gate
    cJSON_AddNumberToObject(root, "spectrum_render_count", (double)spectrum_http_cache_render_count());
    // 1.2.36 Commit 0: счётчики для замера «до/после» (цена пересборки кэша, обходы кучи, записи автосейва)
    cJSON_AddNumberToObject(root, "spectrum_build_us", (double)spectrum_http_cache_build_us_last());
    cJSON_AddNumberToObject(root, "spectrum_build_us_max", (double)spectrum_http_cache_build_us_max());
    cJSON_AddNumberToObject(root, "spectrum_json_builds", (double)spectrum_http_cache_json_builds());
    spectrum_gz_add_metrics(root);   // #GZ-T (1.2.37-rc90): gz_level, gz_comp_bytes, gz_ok/miss/nomem/fail, gz_async_ok/fallback/async_timeout/queue_full/send_err, gz_ring_us, время, размеры, gz_stack_hwm
    cJSON_AddNumberToObject(root, "heap_walk_n", (double)s_heap_walk_n);
    cJSON_AddNumberToObject(root, "autosave_writes", (double)spectrum_autosave_writes());
    cJSON_AddNumberToObject(root, "autosave_skips", (double)spectrum_autosave_skips());
    cJSON_AddNumberToObject(root, "spec_seq_retry", (double)spectrum_spec_seq_retry());       // 1.2.36 H1: повторы seqlock-копии
    cJSON_AddNumberToObject(root, "spec_seq_fallback", (double)spectrum_spec_seq_fallback()); // снимков под SPEC_LOCK (запасной путь)
    cJSON_AddBoolToObject(root,   "http_heavy_busy", http_io_gate_busy());
    cJSON_AddNumberToObject(root, "http_heavy_rejects", (double)http_io_gate_reject_count());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (!json) httpd_resp_set_status(req, "503 Service Unavailable");   // F-09: нет памяти на JSON
    httpd_resp_sendstr(req, json ? json : "{\"ok\":false,\"err\":\"oom\"}");
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// #FW-43: POST /api/usb/recover — UI «Повторить связь». Teardown CDC; connect task
// re-opens within ~2s with RX reset + FTDI re-init + -inf retries.
static esp_err_t handle_usb_recover(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    usb_host_cdc_request_recover();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"reopening\":true}");
}

// #FW-22: глубокая USB-Host диагностика (read-only JSON снапшота usb_diag_snapshot_t).
// Инструментация для #FW-43 (hot-plug re-init): enum_cb_count/open_attempts/cdc_open/
// last_open_errno/rx_cb_count/pkt_hist позволяют увидеть, где рвётся реконнект прибора.
static esp_err_t handle_usb_diag(httpd_req_t *req)
{
    usb_diag_snapshot_t d;
    usb_host_cdc_diag_snapshot(&d);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root,   "host_installed",        d.host_installed);
    cJSON_AddBoolToObject(root,   "cdc_driver_installed",  d.cdc_driver_installed);
    cJSON_AddNumberToObject(root, "last_host_event_flags", d.last_host_event_flags);
    cJSON_AddNumberToObject(root, "host_event_ts_ms",      d.host_event_ts_ms);
    cJSON_AddNumberToObject(root, "enum_cb_count",         d.enum_cb_count);
    cJSON_AddNumberToObject(root, "last_seen_vid",         d.last_seen_vid);
    cJSON_AddNumberToObject(root, "last_seen_pid",         d.last_seen_pid);
    cJSON_AddNumberToObject(root, "last_seen_class",       d.last_seen_class);
    cJSON_AddNumberToObject(root, "last_seen_subclass",    d.last_seen_subclass);
    cJSON_AddNumberToObject(root, "last_seen_ts_ms",       d.last_seen_ts_ms);
    cJSON_AddNumberToObject(root, "bus_devs_now",          d.bus_devs_now);
    cJSON_AddNumberToObject(root, "open_attempts",         d.open_attempts);
    cJSON_AddNumberToObject(root, "last_open_errno",       d.last_open_errno);
    cJSON_AddStringToObject(root, "last_open_errstr",      esp_err_to_name(d.last_open_errno));
    cJSON_AddNumberToObject(root, "last_open_ts_ms",       d.last_open_ts_ms);
    cJSON_AddBoolToObject(root,   "cdc_open",              d.cdc_open);
    cJSON_AddNumberToObject(root, "cdc_error_count",       d.cdc_error_count);
    cJSON_AddNumberToObject(root, "rx_watchdog_trips",     d.rx_watchdog_trips);
    cJSON_AddNumberToObject(root, "bus_empty_trips",       d.bus_empty_trips);
    cJSON_AddNumberToObject(root, "reconnect_ok",          d.reconnect_ok);
    cJSON_AddNumberToObject(root, "last_fault_reason",     d.last_fault_reason);
    cJSON_AddNumberToObject(root, "last_fault_ts_ms",      d.last_fault_ts_ms);
    cJSON_AddNumberToObject(root, "ftdi_step_ok_mask",     d.ftdi_step_ok_mask);
    cJSON_AddNumberToObject(root, "ftdi_last_errno",       d.ftdi_last_errno);
    cJSON_AddNumberToObject(root, "tx_packets",            d.tx_packets);
    cJSON_AddNumberToObject(root, "tx_bytes",              d.tx_bytes);
    cJSON_AddNumberToObject(root, "last_tx_ts_ms",         d.last_tx_ts_ms);
    cJSON_AddNumberToObject(root, "last_tx_errno",         d.last_tx_errno);
    cJSON_AddStringToObject(root, "last_tx_cmd",           d.last_tx_cmd);
    cJSON_AddNumberToObject(root, "rx_cb_count",           d.rx_cb_count);
    cJSON_AddNumberToObject(root, "rx_bytes",              d.rx_bytes);
    cJSON_AddNumberToObject(root, "rx_last_ts_ms",         d.rx_last_ts_ms);
    cJSON_AddNumberToObject(root, "rx_last_len",           d.rx_last_len);
    cJSON_AddStringToObject(root, "rx_last_first16_hex",   d.rx_last_first16_hex);
    cJSON_AddNumberToObject(root, "line_status_errors",    d.line_status_errors);
    cJSON_AddNumberToObject(root, "pkt_hist",              d.pkt_hist);
    cJSON_AddNumberToObject(root, "pkt_text",              d.pkt_text);
    cJSON_AddNumberToObject(root, "pkt_stat",              d.pkt_stat);
    cJSON_AddNumberToObject(root, "pkt_osc",               d.pkt_osc);
    cJSON_AddNumberToObject(root, "pkt_unknown",           d.pkt_unknown);
    cJSON_AddNumberToObject(root, "pkt_bad",               d.pkt_bad);  // #FW-53
    // #FW-53: свипы, а НЕ чанки. pkt_hist выше считает фрагменты гистограммы
    // (~128/с), из которых собирается один спектр — величины несопоставимы.
    {
        uint32_t sw_ok = 0, sw_drop = 0;
        spectrum_get_sweep_stats(&sw_ok, &sw_drop);
        cJSON_AddNumberToObject(root, "sweep_commits", sw_ok);
        cJSON_AddNumberToObject(root, "sweep_drops",   sw_drop);
    }
    cJSON_AddNumberToObject(root, "last_shproto_ts_ms",    d.last_shproto_ts_ms);
    cJSON_AddNumberToObject(root, "last_hist_ts_ms",       d.last_hist_ts_ms);
    cJSON_AddNumberToObject(root, "acq_intent",            d.acq_intent);
    cJSON_AddNumberToObject(root, "acq_resend_count",      d.acq_resend_count);
    cJSON_AddNumberToObject(root, "last_acq_resend_ts_ms", d.last_acq_resend_ts_ms);
    cJSON_AddNumberToObject(root, "drv_task_alive_ts_ms",  d.drv_task_alive_ts_ms);
    cJSON_AddNumberToObject(root, "conn_task_alive_ts_ms", d.conn_task_alive_ts_ms);
    cJSON_AddNumberToObject(root, "dma_free_largest",      d.dma_free_largest);
    cJSON_AddNumberToObject(root, "dma_free_total",        d.dma_free_total);
    cJSON_AddNumberToObject(root, "uptime_ms",             d.uptime_ms);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (!json) httpd_resp_set_status(req, "503 Service Unavailable");   // F-09: нет памяти на JSON
    httpd_resp_sendstr(req, json ? json : "{\"ok\":false,\"err\":\"oom\"}");
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_set_calibration(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[512] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "coeffs");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing coeffs");
        return ESP_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 1) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty coeffs");
        return ESP_FAIL;
    }
    if (n > CALIB_COEFFS) n = CALIB_COEFFS;
    double coeffs[CALIB_COEFFS] = {0};
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        // P2-5: cJSON парсит "NaN"/"Infinity" в valuedouble как есть. NaN/Inf
        // в коэффициентах калибровки потом дают мусорные энергии и могут
        // распространиться в N42/CSV экспорт — отбраковываем на входе.
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad coeff");
            return ESP_FAIL;
        }
        coeffs[i] = item->valuedouble;
    }
    spectrum_set_calibration(coeffs, n - 1);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---- #DEV-6: бэкап/восстановление настроек прибора (вкладка «Сервис») ----
//
// Формат — сырой текст ответов -inf + -tc_pot?, байт-в-байт совместимый с
// автосейвом MCA.exe (references/atomspectra_settings_backup_2026-07-01.md).
// Ниже — общий key/value-парсер этого текста для восстановления.

static inline bool is_kv_boundary(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Ищет "KEY" на границе слова (не внутри другого токена — напр. не путает
// "POT" с "POT2", "TC" с "TCpot") и возвращает указатель на начало значения
// (сразу после разделяющего пробела), либо NULL.
// Граница слова — пробел/таб/CR/LF (не только пробел): бэкап — две строки
// через "\r\n" ("Tcpot [...]" начинается сразу после перевода строки), якорь
// только на ' ' пропускал бы этот ключ (#DEV-7, 2026-07-01).
static const char *find_kv(const char *text, const char *key)
{
    size_t klen = strlen(key);
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        bool left_ok  = (p == text) || is_kv_boundary(p[-1]);
        bool right_ok = (p[klen] == '\0') || is_kv_boundary(p[klen]);
        if (left_ok && right_ok)
            return p + klen + (is_kv_boundary(p[klen]) && p[klen] != '\0' ? 1 : 0);
        p += 1;
    }
    return NULL;
}

static bool kv_get_int(const char *text, const char *key, long *out)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    char *end;
    long val = strtol(v, &end, 10);
    if (end == v) return false;
    *out = val;
    return true;
}

static bool kv_get_double(const char *text, const char *key, double *out)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    char *end;
    double val = strtod(v, &end);
    if (end == v) return false;
    *out = val;
    return true;
}

// Копирует значение-токен до пробела (для "ON"/"OFF" и т.п.)
static bool kv_get_token(const char *text, const char *key, char *buf, size_t bufsz)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    size_t n = 0;
    while (v[n] && v[n] != ' ' && n < bufsz - 1) { buf[n] = v[n]; n++; }
    buf[n] = '\0';
    return n > 0;
}

// Парсит "KEY [n1 n2 ...]" -> массив long (до max элементов). Возвращает
// число элементов (0 для "[]"), либо -1 если ключ/скобка не найдены.
static int kv_get_array(const char *text, const char *key, long *out, int max)
{
    const char *v = find_kv(text, key);
    if (!v) return -1;
    while (*v == ' ') v++;
    if (*v != '[') return -1;
    v++;
    int n = 0;
    while (*v && *v != ']') {
        char *end;
        long val = strtol(v, &end, 10);
        if (end == v) break;
        if (n < max) out[n++] = val;
        v = end;
        while (*v == ' ') v++;
    }
    return n;
}

// LK-05 (1.2.30): httpd ждёт ответ прибора на -inf и -tc_pot? не дольше SETTINGS_RAW_WAIT_MS на КАЖДУЮ команду (было 2000 мс; ответ здорового
// прибора — 0,1–0,5 с). Занятый/молчащий прибор — 400 через 1 с вместо 2 с: веб-сервер стоит вдвое меньше.
#define SETTINGS_RAW_WAIT_MS 1000
// Общая для /api/settings/backup И /api/settings/snapshot (BUG-AS-08,
// KNOWN_ISSUES.md:77): read-only -inf/-tc_pot? прибору + ожидание ответа
// (spectrum_get_info_raw/spectrum_get_tcpot_raw, atomspectra.h). Сама шлёт
// httpd-ошибку и ESP_FAIL при неудаче.
static esp_err_t settings_read_raw_or_err(httpd_req_t *req,
                                           char *info_out, size_t info_cap,
                                           char *tcpot_out, size_t tcpot_cap)
{
    if (!usb_host_cdc_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device not connected");
        return ESP_FAIL;
    }

    char line[700];
    uint32_t seq_before = 0, seq_after = 0;

    spectrum_get_info_raw(line, sizeof(line), &seq_before);
    usb_host_send_text_command("-inf");
    for (int waited = 0; waited < SETTINGS_RAW_WAIT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        spectrum_get_info_raw(line, sizeof(line), &seq_after);
        if (seq_after != seq_before) break;
    }
    if (seq_after == seq_before) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response to -inf (device busy/offline?)");
        return ESP_FAIL;
    }
    spectrum_get_info_raw(info_out, info_cap, NULL);

    spectrum_get_tcpot_raw(line, sizeof(line), &seq_before);
    usb_host_send_text_command("-tc_pot?");
    seq_after = seq_before;
    for (int waited = 0; waited < SETTINGS_RAW_WAIT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        spectrum_get_tcpot_raw(line, sizeof(line), &seq_after);
        if (seq_after != seq_before) break;
    }
    if (seq_after == seq_before) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response to -tc_pot? (older firmware?)");
        return ESP_FAIL;
    }
    spectrum_get_tcpot_raw(tcpot_out, tcpot_cap, NULL);
    return ESP_OK;
}

// GET /api/settings/backup — не изменилось для клиента: читалка выше + стрим
// на скачивание. #FW-17: static 2048Б — не на стеке httpd-воркера.
static esp_err_t handle_settings_backup(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;   // S-01 (1.2.30): тяжёлый GET — токен обязателен
    static char info_line[2048];
    char tcpot_line[700];
    if (settings_read_raw_or_err(req, info_line, sizeof(info_line),
                                  tcpot_line, sizeof(tcpot_line)) != ESP_OK)
        return ESP_FAIL;
    char bkp_name[48], bkp_disp[80];
    web_build_export_name(bkp_name, sizeof(bkp_name), "atomspectra_backup.txt");
    snprintf(bkp_disp, sizeof(bkp_disp), "attachment; filename=\"%s\"", bkp_name);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", bkp_disp);
    httpd_resp_sendstr_chunk(req, info_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_sendstr_chunk(req, tcpot_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// #FW-42-style: одна крайняя копия слепка на LittleFS, перезаписывается.
#define DSP_SNAPSHOT_PATH STORAGE_PATH "/dsp_snapshot.txt"
#define DSP_SNAPSHOT_TMP_PATH STORAGE_PATH "/dsp_snapshot.tmp"   // У4: запись до rename

// BUG-AS-08 (KNOWN_ISSUES.md:77): пишет снимок на flash. Вынесена из
// handle_settings_snapshot ниже — держит его < 25 строк (delegation guard).
// М7 (release-gate-firmware-v1.2.28-code.md): flash_quiet_writer_lock — тот же
// лок, что берут autosave (spectrum.c) и /api/save перед своей flash-записью;
// раньше запись шла в обход и могла совпасть с секундным пакетом гистограммы
// (класс потерь #FW-8). Успех — по КАЖДОМУ шагу (fopen/fputs/fclose), не
// только fopen: caller больше не отвечает "сохранён" на обрезанную/неполную
// запись (не проверялось совсем).
static bool settings_snapshot_write_file(const char *info_line,
                                          const char *tcpot_line, const char *stamp)
{
    // Н7 (раунд 2) + У3 (раунд 3): тихое окно после пакета гистограммы ждём ДО
    // лока и перепроверяем ПОСЛЕ (лок мог освободиться уже за окном), не дольше
    // 25 опросов по 50 мс; не дождались — пишем, как 1.2.27.
    if (!flash_quiet_writer_lock_in_window(25)) return false;
    // У4 (раунд 3): временный файл + rename — сбой записи не уничтожает прежний
    // слепок, GET не может отдать обрезанный файл как успех.
    bool ok = snapshot_file_write_atomic(DSP_SNAPSHOT_TMP_PATH, DSP_SNAPSHOT_PATH,
                                         stamp, info_line, tcpot_line);
    flash_quiet_writer_unlock();
    return ok;
}

// BUG-AS-08: стримит тот же снимок на скачивание с меткой-времени-именем.
static esp_err_t settings_snapshot_send(httpd_req_t *req, const char *info_line,
    const char *tcpot_line, struct tm *tmv, const char *stamp)
{
    // #AWF-12c-style: буферы с запасом под -Werror=format-truncation —
    // gcc консервативно считает %d по ширине int (до 11 симв.), не по
    // факту (tm_* реально 2-4 цифры), иначе сборка падает предупреждением.
    char dl_base[96], dl_name[160], dl_disp[192];
    snprintf(dl_base, sizeof(dl_base), "dsp_snapshot_%04d%02d%02d_%02d%02d%02d.txt",
             tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
             tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    web_build_export_name(dl_name, sizeof(dl_name), dl_base);
    snprintf(dl_disp, sizeof(dl_disp), "attachment; filename=\"%s\"", dl_name);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", dl_disp);
    httpd_resp_sendstr_chunk(req, "# AtomSpectra DSP snapshot ");
    httpd_resp_sendstr_chunk(req, stamp);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_sendstr_chunk(req, info_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_sendstr_chunk(req, tcpot_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// POST /api/settings/snapshot (BUG-AS-08, KNOWN_ISSUES.md:77): "Сохранить
// слепок настройки прибора" — читает -inf/-tc_pot? (settings_read_raw_or_err,
// read-only), пишет на LittleFS (settings_snapshot_write_file) и отдаёт на
// скачивание (settings_snapshot_send). CSRF обязателен — пишет flash.
static esp_err_t handle_settings_snapshot(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    static char info_line[2048];
    char tcpot_line[700];
    if (settings_read_raw_or_err(req, info_line, sizeof(info_line),
                                  tcpot_line, sizeof(tcpot_line)) != ESP_OK)
        return ESP_FAIL;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    bool saved = settings_snapshot_write_file(info_line, tcpot_line, stamp);
    // М7: заголовок читает frontend (web/service.html doDspSnapshot) — текст
    // "сохранён на плате" не показывается на сбое записи.
    httpd_resp_set_hdr(req, "X-Dsp-Snapshot-Saved", saved ? "1" : "0");
    return settings_snapshot_send(req, info_line, tcpot_line, &tmv, stamp);
}

// GET /api/settings/snapshot — отдаёт ПОСЛЕДНИЙ слепок БЕЗ обращения к
// прибору: POST выше перезапишет файл свежим чтением, а если DSP-настройка
// сломалась ПРЯМО СЕЙЧАС, свежий POST затрёт хороший слепок битым.
static esp_err_t handle_settings_snapshot_get(httpd_req_t *req)
{
    FILE *f = fopen(DSP_SNAPSHOT_PATH, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No snapshot saved yet");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"dsp_snapshot.txt\"");
    char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    // У4 (раунд 3): ошибка чтения посреди файла — ответ без завершающего чанка
    // (соединение рвётся), браузер не получит обрезанный слепок как успех.
    bool read_err = ferror(f) != 0;
    fclose(f);
    if (read_err) return ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void send_cmd_delayed(const char *cmd)
{
    usb_host_send_text_command(cmd);
    // Пауза между командами настройки — не заваливать вход прибора пачкой
    // текстовых команд подряд (см. -t/-t_pot до 20 точек каждая).
    vTaskDelay(pdMS_TO_TICKS(30));
}

// POST /api/settings/restore — body = текст бэкапа (тот же формат, что
// отдаёт handle_settings_backup). Разбирает поля и шлёт прибору
// последовательность команд настройки по PROTOCOL.md.
// ⚠ МЕНЯЕТ физическую конфигурацию прибора (RISE/FALL/NOISE/MAX/HYST/F/POT/
// POT2/термокомпенсацию/pile-up). CSRF обязателен. Первый реальный тест на
// железе — только по явному отдельному «да» оператора (см. CLAUDE.md #DEV-6,
// прецедент #DEV-5 — оператор ранее сказал «не сейчас» именно для -frq).
static esp_err_t handle_settings_restore(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    if (!usb_host_cdc_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device not connected");
        return ESP_FAIL;
    }

    char *body = malloc(1600);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int total = 0, r;
    while (total < 1599 && (r = httpd_req_recv(req, body + total, 1599 - total)) > 0)
        total += r;
    if (total <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[total] = '\0';

    if (!strstr(body, "VERSION ") || !strstr(body, "RISE ")) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not an AtomSpectra backup (VERSION/RISE missing)");
        return ESP_FAIL;
    }

    char cmd[160];
    long v;
    double d;
    char tok[16];
    long arr[40];
    int n;

    if (kv_get_int(body, "RISE", &v))  { snprintf(cmd, sizeof(cmd), "-ris %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "FALL", &v))  { snprintf(cmd, sizeof(cmd), "-fall %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "NOISE", &v)) { snprintf(cmd, sizeof(cmd), "-nos %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "HYST", &v))  { snprintf(cmd, sizeof(cmd), "-hyst %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "MAX", &v))   { snprintf(cmd, sizeof(cmd), "-max %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "STEP", &v))  { snprintf(cmd, sizeof(cmd), "-step %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_double(body, "F", &d) && d > 0) {
        snprintf(cmd, sizeof(cmd), "-frq %ld", (long)(d + 0.5));
        send_cmd_delayed(cmd);
    }
    // POT/POT2 = значения U (HV) и V (baseline) соответственно (см.
    // atomspectra_protocol_official_spec.txt:141). Синтаксис без пробела.
    if (kv_get_int(body, "POT", &v))  { snprintf(cmd, sizeof(cmd), "-U%ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "POT2", &v)) { snprintf(cmd, sizeof(cmd), "-V%ld", v); send_cmd_delayed(cmd); }

    if (kv_get_int(body, "Prise", &v)) { snprintf(cmd, sizeof(cmd), "-prise %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Srise", &v)) { snprintf(cmd, sizeof(cmd), "-srise %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Pfall", &v)) { snprintf(cmd, sizeof(cmd), "-pfall %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Sfall", &v)) { snprintf(cmd, sizeof(cmd), "-sfall %ld", v); send_cmd_delayed(cmd); }

    if (kv_get_int(body, "PileUpThr", &v)) { snprintf(cmd, sizeof(cmd), "-pthr %ld", v); send_cmd_delayed(cmd); }
    n = kv_get_array(body, "PileUp", arr, (int)(sizeof(arr) / sizeof(arr[0])));
    if (n > 0) {
        int off = snprintf(cmd, sizeof(cmd), "-pileup");
        for (int i = 0; i < n && off < (int)sizeof(cmd) - 8; i++)
            off += snprintf(cmd + off, sizeof(cmd) - off, " %ld", arr[i]);
        send_cmd_delayed(cmd);
    }

    // Tco[] — до 20 точек (temp, max_integral) термокомпенсации макс. интеграла.
    n = kv_get_array(body, "Tco", arr, (int)(sizeof(arr) / sizeof(arr[0])));
    if (n >= 2) {
        send_cmd_delayed("-tclear");
        int npoints = n / 2;
        if (npoints > 20) npoints = 20;
        for (int i = 0; i < npoints; i++) {
            snprintf(cmd, sizeof(cmd), "-t %d %ld %ld", i + 1, arr[2 * i], arr[2 * i + 1]);
            send_cmd_delayed(cmd);
        }
    }
    if (kv_get_int(body, "TP", &v)) { snprintf(cmd, sizeof(cmd), "-tp %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_token(body, "TC", tok, sizeof(tok))) {
        snprintf(cmd, sizeof(cmd), "-tc %s", strcasecmp(tok, "ON") == 0 ? "on" : "off");
        send_cmd_delayed(cmd);
    }

    // Вторая строка бэкапа: "Tcpot [...]" (регистр отличается от "TCpot" выше,
    // strstr — регистрозависим, поэтому find_kv не путает две строки) —
    // до 20 точек компенсации baseline (#DOC-3/BUG-AS-08).
    n = kv_get_array(body, "Tcpot", arr, (int)(sizeof(arr) / sizeof(arr[0])));
    if (n >= 2) {
        int npoints = n / 2;
        if (npoints > 20) npoints = 20;
        for (int i = 0; i < npoints; i++) {
            snprintf(cmd, sizeof(cmd), "-t_pot %d %ld %ld", i + 1, arr[2 * i], arr[2 * i + 1]);
            send_cmd_delayed(cmd);
        }
    }
    if (kv_get_token(body, "TCpot", tok, sizeof(tok))) {
        snprintf(cmd, sizeof(cmd), "-tc_pot %s", strcasecmp(tok, "ON") == 0 ? "on" : "off");
        send_cmd_delayed(cmd);
    }

    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_reboot_esp(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_healthcheck(httpd_req_t *req)
{
    bool usb = usb_host_cdc_is_connected();
    bool wifi = wifi_is_connected();
    int64_t uptime_us = esp_timer_get_time();
    const spectrum_data_t *sp = spectrum_get_current();

    httpd_resp_set_type(req, "application/json");
    if (usb && wifi) {
        httpd_resp_set_status(req, "200 OK");
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"analyzer_connected\":%s,\"wifi_connected\":%s,"
        "\"uptime_sec\":%.1f,\"spectrum_valid\":%s}",
        (usb && wifi) ? "ok" : "degraded",
        usb ? "true" : "false",
        wifi ? "true" : "false",
        (double)uptime_us / 1000000.0,
        sp->valid ? "true" : "false");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ===================== #FIELD-4/5/7: полевой AP — время и пароль =====================

// Нижняя граница вменяемого epoch для /api/time (2023-11-14). WF_SANE_EPOCH из
// spectrogram.c не экспортирован — дублируем как локальную санитарную границу.
#define FIELD_MIN_EPOCH_S  1700000000LL
#define FIELD_MAX_EPOCH_S  4102444800LL   // 2100-01-01
#define FIELD_AP_URL       "http://192.168.4.1/"

// #FIELD-5 (A2): POST /api/time — установка времени платы от браузера телефона.
// Тело JSON: {"epoch_ms": <ms since 1970 UTC>, "manual": <bool, опц.>}.
// В Outdoor (полевой AP) SNTP не поднят → sntp_synced=false → время идёт от телефона.
// Guard-логика (net_time_should_accept) — в net_time.c (host-тестируема):
//   sntp_synced → отклонить (война источников; T1); manual → принять; auto → |Δ|>5 с.
// При приёме — settimeofday + пометка источника + spectrogram_time_synced() (пересчёт
// started_at ongoing-записи из монотонного uptime, тот же путь что SNTP-cb #FW-23).
static esp_err_t handle_time_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;

    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *jms  = cJSON_GetObjectItem(root, "epoch_ms");
    cJSON *jman = cJSON_GetObjectItem(root, "manual");
    if (!cJSON_IsNumber(jms)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "epoch_ms required");
        return ESP_FAIL;
    }
    int64_t epoch_ms = (int64_t)jms->valuedouble;
    bool manual = cJSON_IsTrue(jman);
    cJSON_Delete(root);

    if (epoch_ms < FIELD_MIN_EPOCH_S * 1000 || epoch_ms > FIELD_MAX_EPOCH_S * 1000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "epoch out of range");
        return ESP_FAIL;
    }

    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    int64_t dt_abs_sec = llabs(epoch_ms - now_ms) / 1000;

    bool sntp   = net_time_sntp_synced();
    bool accept = net_time_should_accept(sntp, manual, dt_abs_sec);

    if (accept) {
        struct timeval tv = { .tv_sec  = (time_t)(epoch_ms / 1000),
                              .tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000) };
        settimeofday(&tv, NULL);
        net_time_set_source(manual ? TIME_SRC_MANUAL : TIME_SRC_BROWSER);
        spectrogram_time_synced();   // пересчёт started_at ongoing-записи (#FW-23)
        ESP_LOGI(TAG, "FIELD-5: time set from %s (was off %lld s)",
                 manual ? "manual" : "browser", (long long)dt_abs_sec);
    }

    char out[96];
    int n = snprintf(out, sizeof(out),
        "{\"ok\":true,\"accepted\":%s,\"source\":\"%s\",\"sntp\":%s}",
        accept ? "true" : "false", net_time_source_str(), sntp ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

// #FIELD-4: captive-portal probe endpoints. В полевом AP DNS-hijack (wifi_manager)
// заворачивает все имена на 192.168.4.1; OS-пробы связности (Android generate_204/
// gen_204, iOS/macOS hotspot-detect/library-test, Windows ncsi/connecttest) получают
// здесь 302 → всплывает «Вход в сеть», тап открывает Web UI. В STA (Indoor) плата —
// клиент, проба до неё не доходит; для чистоты отвечаем 204 (как настоящий generate_204).
static esp_err_t handle_captive_probe(httpd_req_t *req)
{
    // F1 (итоговое ревью 25.09): классификация теперь идёт ДО дispatch, в
    // общем трамплине (web_server_init) — здесь ничего откатывать не нужно.
    if (wifi_manager_is_ap_mode()) {
        // #FIELD-10/#FIELD-11: вместо 302 на тяжёлый index отдаём лёгкую captive-landing
        // прямо в мини-браузере ОС — крупный адрес 192.168.4.1 (виден сразу) + авто-редирект
        // на страницу прибора + кнопка-fallback. 200+HTML вместо 204 → OS считает «captive
        // portal», всплывает «Вход в сеть» и открывает нашу страницу.
        return handle_captive_page(req);
    }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// #FIELD-7 (#SEC-2): POST /api/ap-pass — смена пароля полевого AP. Тело {"password":"..."}.
// Пустой → сброс к дефолту (следующий boot возьмёт AP_PASS_DEFAULT). Непустой обязан быть
// ≥8 символов (мин WPA2-PSK). Хранится в NVS wifi/ap_pass (тот же ключ, что читает
// load_ap_config); применяется после ребута (способ A4 — пересоздавать netif/httpd на
// лету рискованно). Пароль общий для всех (#SEC-2): оператор раздаёт SSID+пароль голосом.
static esp_err_t handle_ap_pass(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;

    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *jp = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(jp)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password required");
        return ESP_FAIL;
    }
    const char *pass = jp->valuestring;
    size_t plen = strlen(pass);
    if (plen > 0 && plen < 8) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password must be >=8 chars (WPA2)");
        return ESP_FAIL;
    }
    if (plen >= WIFI_PASS_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too long");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        if (plen == 0) nvs_erase_key(nvs, "ap_pass");   // сброс к дефолту (not-found ок)
        else           nvs_set_str(nvs, "ap_pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// #FIELD-6 (T4/T5): переключатель Indoor/Outdoor со стартовой страницы. Тело
// {"mode":"indoor"|"outdoor"}. outdoor → NVS ap_mode=1 (липкий forced AP); indoor →
// снять ap_mode (ребут в STA). Применяется ребутом (способ A4). Тот же ключ ap_mode,
// что читает wifi_manager на старте (FIELD-2b) и ставит setup-кнопка (FIELD-2c).
static esp_err_t handle_net_mode(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *jm = cJSON_GetObjectItem(root, "mode");
    const char *mode = cJSON_IsString(jm) ? jm->valuestring : NULL;
    bool outdoor;
    if (mode && strcmp(mode, "outdoor") == 0)      outdoor = true;
    else if (mode && strcmp(mode, "indoor") == 0)  outdoor = false;
    else { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be indoor|outdoor"); return ESP_FAIL; }
    cJSON_Delete(root);

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        if (outdoor) nvs_set_u8(nvs, "ap_mode", 1);
        else         nvs_erase_key(nvs, "ap_mode");   // → Indoor (not-found ок)
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// F1 (итоговое ревью 25.09): активность — на уровне СОКЕТА (прежняя модель
// метила ОТКРЫТИЕ соединения; keep-alive-опрос и WS водопада держат одно
// соединение часами, метка не обновлялась — через 10 мин работающий
// пользователь считался бездействующим). Модель и host-тесты —
// http_activity_plan.h. open_fn/close_fn метят открытие/закрытие;
// web_server_note_request_activity() — ЕДИНСТВЕННАЯ точка, где запрос
// СЧИТАЕТСЯ (общий трамплин над uris[] ниже + choke points web_waterfall.c).
// N1 (ревью-2): open_fn/close_fn для активности БОЛЬШЕ НЕ НУЖНЫ — модель
// упрощена до метки последнего запроса (http_activity_plan.h), сокет ей не
// нужен вовсе. close_fn остаётся ТОЛЬКО ради чистки WS-реестра водопада
// (web_waterfall_on_close) — config.close_fn ниже указывает на него напрямую.
static http_activity_state_t s_activity;

void web_server_note_request_activity(const char *uri)
{
    http_activity_note_request(&s_activity, http_uri_is_activity(uri),
                                (uint32_t)(esp_timer_get_time() / 1000));
}

// Незарегистрированный URI структурно НЕ проходит через трамплин (тот висит
// только на СОВПАВШИХ обработчиках) — не поднимает активность без единой
// лишней строчки здесь. http_404_is_activity() остаётся тестируемым
// инвариантом-документацией (всегда false).
static esp_err_t handle_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

// F1: общий трамплин над uris[] (регистрация ниже, web_server_init) — индекс
// в user_ctx выбирает исходный обработчик из простого массива функций (без
// malloc/struct). DOC-URI-1: единственный источник истины по числу и запасу
// max_uri_handlers — комментарий у config.max_uri_handlers=90 ниже (пересчитан
// 2026-09-27: 79 факт, запас +11); эта строка раньше повторяла устаревшие числа
// (80/~53) и разошлась с ними при последнем поднятии лимита.
#define WEB_SERVER_URI_MAX 90
static esp_err_t (*s_wrap_handlers[WEB_SERVER_URI_MAX])(httpd_req_t *);

static esp_err_t activity_trampoline(httpd_req_t *req)
{
    size_t idx = (size_t)(intptr_t)req->user_ctx;
    web_server_note_request_activity(req->uri);
    return s_wrap_handlers[idx](req);
}

bool web_server_http_activity_quiet(uint32_t threshold_ms)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return http_activity_quiet(&s_activity, now_ms, threshold_ms);
}

uint32_t web_server_http_idle_s(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return (now_ms - s_activity.last_user_event_ms) / 1000;
}

void web_server_init(void)
{
    csrf_generate();
    http_io_gate_init();           // #PERF-2
    spectrum_http_cache_init();    // #PERF-1
    spectrum_gz_init();            // #GZ-T (1.2.37-rc90): крючки сжатия спектра; задача и память — только при gzip_level 1/6

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // #WF-1: httpd на core 1 к остальной прикладной сети (#TCP-2). Дефолт
    // tskNO_AFFINITY позволял httpd (prio 5) исполняться на core 0 рядом с
    // USB-приёмом — уводим целиком.
    config.core_id = 1;
    // #WF-2/#MON-1/#FIELD-4/#FW-50/issue #52/AWF-5: О9 (release-gate-firmware-
    // v1.2.28-code.md) — пересчитано 2026-09-28 теми же командами (см. ниже):
    // 62 в uris[] (1.2.31: +/api/import) + 19 reg + 1 /ws/waterfall = 82, не 79 (комментарий отстал
    // от кода при добавлении эндпоинтов после 2026-09-27). С лимитом 80 запас был бы всего +1 —
    // ПОСЛЕДНИЙ обработчик молча не регистрировался бы → тихий 404 (та самая
    // авария, которой лимит 45 стоил цикла reconnect). 90 даёт запас +8 (82 из 90).
    // Добавляешь эндпоинт — пересчитай:
    //   awk '/httpd_uri_t uris\[\]/,/^    };/' main/web_server.c | grep -cE '^\s*\{"/'
    //   grep -cE '^\s*reg\(server' main/web_waterfall.c   (+1 на /ws/waterfall)
    config.max_uri_handlers = 90;
    config.stack_size = 8192;
    config.max_open_sockets = HTTPD_MAX_OPEN_SOCKETS;   // socket_budget.h: 19 + 3 внутр. + 2 tcp_bridge = 24 (проверка при сборке)
    config.lru_purge_enable = true;      // при исчерпании пула закрыть LRU-соединение, не отказывать (errno 23)
    config.uri_match_fn = httpd_uri_match_wildcard;
    // #UI-15 P0: чистим WS-реестр при ЛЮБОМ закрытии сокета (RST/FIN/LRU/F5);
    // без callback зомбирующиеся fd ломают broadcast после нескольких F5.
    config.close_fn = web_waterfall_on_close;   // N1: активности тут больше не нужно
    // #UI-15 P2: сжимаем default recv/send (~5 c) — освобождаем сокеты быстрее
    // под давлением F5+poll, иначе пул держит «полудохлые» соединения долго.
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    // N1 (ревью-2): TCP keepalive — зомби-сокет (телефон ушёл из зоны, FIN не
    // пришёл, ACK последнего ответа получен, lwIP не ретранслирует) теперь
    // корректно обнуляется гейтом возврата (активность по метке запроса, не
    // по факту, что сокет ещё открыт), но keepalive всё равно освобождает
    // занятый слот httpd раньше, чем это сделал бы LRU (только при 11/11).
    // Значения IDF-дефолтов (esp_http_server.h): idle/interval 5с, count 3.
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        {"/api/csrf-token",              HTTP_GET,  handle_csrf_token,       NULL},
        {"/api/status",                  HTTP_GET,  handle_status,           NULL},
        {"/api/spectrum",                HTTP_GET,  handle_spectrum,         NULL},
        {"/api/spectrum.json",           HTTP_GET,  handle_spectrum_json,    NULL},
        {"/api/spectrum/meta.json",      HTTP_GET,  handle_spectrum_meta,    NULL},  // #PERF-1
        {"/api/command",                 HTTP_POST, handle_command,          NULL},
        {"/api/devlog",                  HTTP_GET,  handle_devlog,           NULL},
        {"/api/debug/log/meta",          HTTP_GET,  handle_debug_log_meta,   NULL},  // #FW-50
        {"/api/debug/cmdlog",            HTTP_GET,  handle_debug_cmdlog,     NULL},  // 1.2.39-t1 CSRF, 404 без AWF_CMDLOG
        {"/api/debug/log",               HTTP_GET,  handle_debug_log_get,    NULL},  // #FW-50 CSRF
        {"/api/debug/log/flush",         HTTP_POST, handle_debug_log_flush,  NULL},  // #FW-50 CSRF
        {"/api/debug/log/config",        HTTP_GET,  handle_debug_log_meta,   NULL},  // тот же meta_json
        {"/api/debug/log/config",        HTTP_POST, handle_debug_log_config_set, NULL},  // CSRF
        {"/api/monitor/series",          HTTP_GET,  handle_monitor_series,   NULL},  // #MON-1
        {"/api/reset",                   HTTP_POST, handle_reset,            NULL},
        {"/api/ota",                     HTTP_POST, handle_ota,              NULL},  // AWF-4
        {"/api/ota/github/check",        HTTP_GET,  handle_ota_gh_check,     NULL},  // AWF-5
        {"/api/ota/github/install",      HTTP_POST, handle_ota_gh_install,   NULL},  // AWF-5
        {"/api/ota/github/progress",     HTTP_GET,  handle_ota_gh_progress,  NULL},  // AWF-5
        {"/api/ota/github/channel",      HTTP_GET,  handle_ota_gh_channel_get, NULL},// AWF-5
        {"/api/ota/github/channel",      HTTP_POST, handle_ota_gh_channel_set, NULL},// AWF-5
        {"/api/boot-config",             HTTP_GET,  handle_boot_config_get,  NULL},
        {"/api/boot-config",             HTTP_POST, handle_boot_config_set,  NULL},
        {"/api/save",                    HTTP_POST, handle_save,             NULL},
        {"/api/list",                    HTTP_GET,  handle_list,             NULL},
        {"/api/export.xml",              HTTP_GET,  handle_export_xml,       NULL},
        {"/api/export.csv",              HTTP_GET,  handle_export_csv,       NULL},
        {"/api/export.n42",              HTTP_GET,  handle_export_n42,       NULL},
        {"/api/export.spe",              HTTP_GET,  handle_export_spe,       NULL},
        {"/api/saved/*",                 HTTP_GET,  handle_saved_get,        NULL},
        {"/api/saved/*",                 HTTP_POST, handle_saved_delete,     NULL},
        {"/api/import",                  HTTP_POST, handle_import,           NULL},  // 1.2.31: НЕ под /api/saved/* (там POST = удаление)
        {"/api/backup/*",                HTTP_GET,  handle_backup_get,       NULL},  // issue #52
        {"/api/backup/*",                HTTP_POST, handle_backup_delete,    NULL},  // issue #52
        {"/api/device",                  HTTP_GET,  handle_device,           NULL},
        {"/api/system",                  HTTP_GET,  handle_system,           NULL},
        {"/api/usb-diag",                HTTP_GET,  handle_usb_diag,         NULL},  // #FW-22
        {"/api/usb/recover",             HTTP_POST, handle_usb_recover,      NULL},  // #FW-43
        {"/api/reboot-device",           HTTP_POST, handle_reboot_device,    NULL},
        {"/api/wifi/reset",              HTTP_POST, handle_wifi_reset,       NULL},
        {"/api/reboot-esp",              HTTP_POST, handle_reboot_esp,       NULL},
        {"/api/calibration",             HTTP_POST, handle_set_calibration,  NULL},
        {"/api/settings/backup",         HTTP_GET,  handle_settings_backup,  NULL},
        {"/api/settings/snapshot",       HTTP_POST, handle_settings_snapshot, NULL},  // BUG-AS-08
        {"/api/settings/snapshot",       HTTP_GET,  handle_settings_snapshot_get, NULL},  // BUG-AS-08: без чтения прибора
        {"/api/settings/restore",        HTTP_POST, handle_settings_restore, NULL},
        // #FIELD-5/7: установка времени от браузера + смена пароля полевого AP
        {"/api/time",                    HTTP_POST, handle_time_set,         NULL},
        {"/api/ap-pass",                 HTTP_POST, handle_ap_pass,          NULL},
        {"/api/net-mode",                HTTP_POST, handle_net_mode,         NULL},  // #FIELD-6 T4/T5
        // #FIELD-4: captive-portal пробы связности ОС → 302 на Web UI (в AP-режиме)
        {"/generate_204",                HTTP_GET,  handle_captive_probe,    NULL},  // Android
        {"/gen_204",                     HTTP_GET,  handle_captive_probe,    NULL},  // Android alt
        {"/hotspot-detect.html",         HTTP_GET,  handle_captive_probe,    NULL},  // iOS/macOS
        {"/library/test/success.html",   HTTP_GET,  handle_captive_probe,    NULL},  // iOS/macOS
        {"/ncsi.txt",                    HTTP_GET,  handle_captive_probe,    NULL},  // Windows
        {"/connecttest.txt",             HTTP_GET,  handle_captive_probe,    NULL},  // Windows
        {"/captive",                     HTTP_GET,  handle_captive_page,     NULL},  // #FIELD-10/11 landing
        {"/healthcheck",                 HTTP_GET,  handle_healthcheck,      NULL},
        {"/saved",                       HTTP_GET,  handle_saved_page,       NULL},
        {"/system",                      HTTP_GET,  handle_system_page,      NULL},
        {"/service",                     HTTP_GET,  handle_service_page,     NULL},
        {"/monitor",                     HTTP_GET,  handle_monitor_page,     NULL},
        {"/common-time.js",              HTTP_GET,  handle_common_time_js,   NULL},  // #FIELD-5
        {"/",                            HTTP_GET,  handle_index,            NULL},
    };

    // N4 (ревью-2): uris[] — статический литерал, его размер ИЗВЕСТЕН на
    // сборке (в отличие от web_waterfall.c's reg(), тут нужен ровно
    // _Static_assert, не рантайм-guard) — лишняя запись без пересмотра
    // WEB_SERVER_URI_MAX раньше молча переполнила бы s_wrap_handlers[].
    _Static_assert(sizeof(uris) / sizeof(uris[0]) <= WEB_SERVER_URI_MAX,
                   "WEB_SERVER_URI_MAX меньше числа записей uris[]");

    // F1: трамплин маркирует активность (по http_uri_is_activity(req->uri))
    // ДО вызова настоящего обработчика — правка ТОЛЬКО регистрации (эта
    // петля), не тел ~53 обработчиков. user_ctx — индекс в s_wrap_handlers
    // (иначе он свободен у всех записей uris[], проверено до внедрения).
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        s_wrap_handlers[i] = uris[i].handler;
        httpd_uri_t entry = uris[i];
        entry.handler = activity_trampoline;
        entry.user_ctx = (void *)(intptr_t)i;
        // issue #52b (sweep-B задача 7): переполнение таблицы обработчиков
        // раньше молча роняло ПОСЛЕДНИЙ маршрут (класс уже стоил инцидента
        // с лимитом 45) -- теперь хотя бы громкий ESP_LOGE с именем URI.
        esp_err_t rerr = httpd_register_uri_handler(server, &entry);
        if (rerr != ESP_OK)
            ESP_LOGE(TAG, "issue#52b: register '%s' failed: %s", uris[i].uri, esp_err_to_name(rerr));
    }

    web_waterfall_register(server);      // /waterfall, /api/waterfall/*, /ws/waterfall
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_404);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}