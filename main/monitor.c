// #MON-1 (#FW-40): сбор серии CPS на плате вместо браузера.
//
// Проблема: web/monitor.html копил дельты по live-poll /api/spectrum.json в JS.
// Вкладка в фоне → браузер троттлит setInterval до ~60 с (интервалы скачут,
// диагонали в графике), вкладка закрыта → дыра в данных.
//
// Решение: monitor_task подписан на коммиты свипов (spectrum_add_commit_listener,
// как wf_task/autosave) и на каждом коммите снимает атомарную пару
// (total_counts, total_time_sec) через spectrum_get_totals() — дёшево, без
// 32 КБ snapshot. Дельта против прошлого замера кладётся базовым сэмплом в
// кольцо PSRAM (6 ч при 1 Гц); httpd отдаёт хвост серии по seq
// (GET /api/monitor/series?since=<seq>, web_server.c). Точность не зависит от
// фазы задачи: dur считается по НАБОРНОМУ времени прибора — проспали два
// коммита → один честный сэмпл dur=2, а не искажение.

#include "monitor.h"
#include "atomspectra.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "monitor";

#define MON_RING_PSRAM     21600   // 6 ч при 1 Гц: 21600 × 10 Б ≈ 211 КБ PSRAM
#define MON_RING_INTERNAL   3600   // fallback без PSRAM: 1 ч ≈ 35 КБ internal

static monitor_sample_t *s_ring;
static uint32_t s_cap;       // фактическая ёмкость кольца (см. monitor_init)
static uint32_t s_head;      // индекс, куда ляжет следующий сэмпл
static uint32_t s_count;     // занято сэмплов (<= s_cap)
static uint32_t s_last_seq;  // seq последнего записанного (0 = ещё ничего → первый сэмпл seq=1)
static uint32_t s_epoch;     // эпоха серии; растёт при откате счётчиков прибора

// Писатель — monitor_task (1 сэмпл/с), читатель — httpd-воркер. Лок держится
// микросекунды (запись 10 Б) / доли мс (копирование чанка <= 20 КБ).
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_commit_sig;   // отдаёт spectrum.c на каждом коммите свипа

static void ring_push(uint32_t end_sec, uint32_t dcounts, uint16_t dur)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ring[s_head].end_sec = end_sec;
    s_ring[s_head].counts  = dcounts;
    s_ring[s_head].dur     = dur;
    s_head = (s_head + 1) % s_cap;
    if (s_count < s_cap) s_count++;   // при переполнении старые вытесняются, seq растёт дальше
    s_last_seq++;
    xSemaphoreGive(s_lock);
}

// Откат счётчиков (сброс спектра -rst / рестарт прибора): новая эпоха, кольцо
// очищается. seq НЕ обнуляется — монотонен через эпохи, клиенту так проще.
static void ring_new_epoch(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_epoch++;
    s_count = 0;
    s_head  = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "counters rollback -> epoch %" PRIu32 ", ring cleared", s_epoch);
}

size_t monitor_copy_since(uint32_t since, monitor_sample_t *out, size_t max,
                          uint32_t *out_first_seq, uint32_t *out_next_seq,
                          uint32_t *out_epoch)
{
    if (!s_lock || !s_ring) {   // монитор отключён (alloc failed) — пустая серия
        if (out_first_seq) *out_first_seq = 1;
        if (out_next_seq)  *out_next_seq  = 1;
        if (out_epoch)     *out_epoch     = 0;
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t next  = s_last_seq + 1;
    uint32_t first = next;                        // «отдать нечего» по умолчанию
    uint32_t epoch = s_epoch;                     // строго под локом — когерентно с сэмплами
    if (s_count > 0) {
        uint32_t oldest = s_last_seq - s_count + 1;
        uint64_t from   = (uint64_t)since + 1;    // uint64 — нет wrap на since=UINT32_MAX
        if (from < oldest) from = oldest;
        if (from <= s_last_seq) {
            size_t avail = (size_t)(s_last_seq - (uint32_t)from) + 1;
            n = (avail > max) ? max : avail;
            first = (uint32_t)from;
            for (size_t i = 0; i < n; i++) {
                uint32_t seq = first + (uint32_t)i;
                uint32_t idx = (s_head + s_cap - (s_last_seq - seq + 1)) % s_cap;
                out[i] = s_ring[idx];
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (out_first_seq) *out_first_seq = first;
    if (out_next_seq)  *out_next_seq  = next;
    if (out_epoch)     *out_epoch     = epoch;
    return n;
}

static void monitor_task(void *arg)
{
    (void)arg;
    uint32_t prev_counts = 0, prev_time = 0;
    bool prev_valid = false;
    for (;;) {
        // Будимся коммитом свипа (паттерн wf_task); 1500 мс — fallback-тик при
        // молчащем приборе (тогда totals не растут и сэмпл не эмитится).
        if (s_commit_sig) xSemaphoreTake(s_commit_sig, pdMS_TO_TICKS(1500));
        else vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t counts = 0, tsec = 0;
        spectrum_get_totals(&counts, &tsec);

        if (!prev_valid) {
            // Первый замер (в т.ч. restored-autosave) — только опорная точка:
            // иначе первый сэмпл получил бы весь накопленный столбец разом.
            prev_counts = counts; prev_time = tsec; prev_valid = true;
            continue;
        }
        if (counts < prev_counts || tsec < prev_time) {
            // Откат счётчиков = сброс спектра или рестарт прибора → новая эпоха.
            ring_new_epoch();
            prev_counts = counts; prev_time = tsec;
            continue;
        }
        uint32_t dur = tsec - prev_time;
        if (dur == 0) continue;              // свип без прироста времени — ждём дальше
        if (dur > UINT16_MAX) {
            // Скачок крупнее формата сэмпла (>18 ч) — не данные, тихий ресинк.
            ESP_LOGW(TAG, "time jump %" PRIu32 "s > sample fmt, resync", dur);
            prev_counts = counts; prev_time = tsec;
            continue;
        }
        ring_push(tsec, counts - prev_counts, (uint16_t)dur);
        prev_counts = counts; prev_time = tsec;
    }
}

void monitor_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "lock create failed - monitor disabled");
        return;
    }

    s_cap  = MON_RING_PSRAM;
    s_ring = heap_caps_malloc(s_cap * sizeof(monitor_sample_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        // Fallback: internal RAM, час истории — лучше, чем ничего.
        s_cap  = MON_RING_INTERNAL;
        s_ring = malloc(s_cap * sizeof(monitor_sample_t));
        if (s_ring)
            ESP_LOGW(TAG, "PSRAM alloc failed - internal ring %" PRIu32 " samples (1h)", s_cap);
    }
    if (!s_ring) {
        ESP_LOGE(TAG, "ring alloc failed - monitor disabled");
        return;
    }

    // Эпоха рандомизируется на старте: перезапуск платы (кольцо потеряно)
    // выглядит для клиента как смена эпохи → чистый ресинк, а не тихая дыра.
    s_epoch = esp_random() & 0x3FFFFFFF;

    s_commit_sig = xSemaphoreCreateBinary();
    if (s_commit_sig) spectrum_add_commit_listener(s_commit_sig);

    // Core 1 (как wf_task) — подальше от USB-приёма на core 0. Prio 2 хватает:
    // dur считается по времени прибора, опоздание задачи данные не искажает.
    xTaskCreatePinnedToCore(monitor_task, "mon", 4096, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "ring %" PRIu32 " samples (%" PRIu32 " KB, %s), epoch %" PRIu32,
             s_cap, (uint32_t)(s_cap * sizeof(monitor_sample_t) / 1024),
             s_cap == MON_RING_PSRAM ? "PSRAM" : "internal", s_epoch);
}
