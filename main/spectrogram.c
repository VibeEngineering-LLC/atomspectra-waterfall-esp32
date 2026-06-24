#include "atomspectra.h"
#include "spectrogram.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>

static const char *TAG = "wf";

#define STORAGE_PATH      "/sto" "rage"
#define WF_DATA           STORAGE_PATH "/wf_data.bin"
#define WF_META           STORAGE_PATH "/wf_meta.json"
#define WF_FLASH_RESERVE  (1024 * 1024)

static uint16_t        *s_ring;
static uint32_t         s_capacity;
static uint32_t         s_head;
static uint32_t         s_count;
static uint16_t        *s_row;
static uint32_t        *s_prev;
static uint32_t         s_prev_total;
static spectrum_data_t *s_snap;     // только start()/write_meta() (serial/calib для META)
static spectrum_data_t *s_wf_snap;  // приватный буфер периодического wf_task (P3-4)

static wf_status_t       s_status;
static wf_row_cb_t       s_row_cb;
static FILE             *s_fp;
static SemaphoreHandle_t s_lock;

#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static void wf_task(void *arg);

static uint32_t flash_free_bytes(void)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK) return 0;
    return (uint32_t)(total - used);
}

static void write_meta(uint32_t rows)
{
    FILE *f = fopen(WF_META, "wb");
    if (!f) return;
    fprintf(f,
        "{\"format\":\"atomspectra-waterfall\",\"version\":1,\"channels\":%d,"
        "\"dtype\":\"uint16\",\"byte_order\":\"little\",\"interval_sec\":%" PRIu32 ","
        "\"started_at\":%ld,\"rows\":%" PRIu32,
        WF_CHANNELS, s_status.interval_sec, (long)s_status.started_at, rows);
    if (s_snap && s_snap->serial_number[0])
        fprintf(f, ",\"serial\":\"%s\"", s_snap->serial_number);
    if (s_snap && s_snap->calib_valid) {
        fprintf(f, ",\"calibration\":[");
        for (int i = 0; i <= s_snap->calib_order; i++)
            fprintf(f, "%s%.15g", i ? "," : "", s_snap->calibration[i]);
        fprintf(f, "]");
    }
    fprintf(f, "}");
    fclose(f);
}

static void ring_push(const uint16_t *row)
{
    memcpy(s_ring + (size_t)s_head * WF_CHANNELS, row, WF_ROW_BYTES);
    s_head = (s_head + 1) % s_capacity;
    if (s_count < s_capacity) s_count++;
}

void spectrogram_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));
    s_status.interval_sec = WF_INTERVAL_DEFAULT;
    s_status.persist      = true;

    s_capacity = WF_RING_ROWS_DEFAULT;
    size_t ring_bytes = (size_t)s_capacity * WF_ROW_BYTES;
    s_ring = heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM);
    while (!s_ring && s_capacity > 16) {
        s_capacity /= 2;
        ring_bytes = (size_t)s_capacity * WF_ROW_BYTES;
        s_ring = heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM);
    }
    s_prev = heap_caps_malloc(WF_CHANNELS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_row  = heap_caps_malloc(WF_ROW_BYTES, MALLOC_CAP_SPIRAM);
    s_snap = heap_caps_malloc(sizeof(spectrum_data_t), MALLOC_CAP_SPIRAM);
    s_wf_snap = heap_caps_malloc(sizeof(spectrum_data_t), MALLOC_CAP_SPIRAM);

    if (!s_ring || !s_prev || !s_row || !s_snap || !s_wf_snap) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        s_status.ready = false;
        return;
    }
    s_status.ready         = true;
    s_status.ring_capacity = s_capacity;
    ESP_LOGI(TAG, "waterfall ready: ring=%" PRIu32 " rows (%u KB PSRAM)",
             s_capacity, (unsigned)(ring_bytes / 1024));

    xTaskCreatePinnedToCore(wf_task, "wf", 4096, NULL, 3, NULL, 1);
}

static void wf_task(void *arg)
{
    for (;;) {
        uint32_t iv = s_status.interval_sec;
        if (iv < WF_INTERVAL_MIN) iv = WF_INTERVAL_MIN;
        vTaskDelay(pdMS_TO_TICKS(iv * 1000));

        if (!s_status.recording) continue;
        spectrum_get_snapshot(s_wf_snap);

        bool reset = (s_wf_snap->total_counts < s_prev_total);
        for (int i = 0; i < WF_CHANNELS; i++) {
            int64_t d = (int64_t)s_wf_snap->bins[i] - (reset ? 0 : (int64_t)s_prev[i]);
            if (d < 0) d = 0; else if (d > 65535) d = 65535;
            s_row[i]  = (uint16_t)d;
            s_prev[i] = s_wf_snap->bins[i];
        }
        s_prev_total = s_wf_snap->total_counts;

        LOCK();
        ring_push(s_row);
        s_status.total_rows++;
        s_status.ring_count = s_count;
        uint32_t idx     = s_status.total_rows - 1;
        bool     persist = s_status.persist && !s_status.flash_full;
        UNLOCK();

        if (persist) {
            if (flash_free_bytes() < (uint32_t)WF_ROW_BYTES + WF_FLASH_RESERVE) {
                LOCK(); s_status.flash_full = true; UNLOCK();
                ESP_LOGW(TAG, "flash full at %" PRIu32 " rows", s_status.flash_rows);
            } else if (s_fp) {
                size_t wr = fwrite(s_row, 1, WF_ROW_BYTES, s_fp);
                if (wr != WF_ROW_BYTES || fflush(s_fp) != 0) {
                    ESP_LOGE(TAG, "flash row write failed (wr=%zu), stopping persist", wr);
                    LOCK(); s_status.flash_full = true; UNLOCK();
                } else {
                    LOCK(); s_status.flash_rows++; UNLOCK();
                }
            }
        }
        if (s_row_cb) s_row_cb(s_row, WF_ROW_BYTES, idx);
    }
}

void spectrogram_get_status(wf_status_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_status;
    UNLOCK();
}

int spectrogram_start(void)
{
    if (!s_status.ready) return -1;
    spectrum_get_snapshot(s_snap);
    LOCK();
    memcpy(s_prev, s_snap->bins, WF_CHANNELS * sizeof(uint32_t));
    s_prev_total        = s_snap->total_counts;
    s_head              = 0;
    s_count             = 0;
    s_status.ring_count = 0;
    s_status.total_rows = 0;
    s_status.flash_full = false;
    s_status.flash_rows = 0;
    s_status.started_at = time(NULL);
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    if (s_status.persist) {
        s_fp = fopen(WF_DATA, "wb");
        if (!s_fp) ESP_LOGE(TAG, "cannot open data file");
        write_meta(0);
    }
    s_status.recording = true;
    UNLOCK();
    ESP_LOGI(TAG, "recording started, interval=%" PRIu32 "s persist=%d",
             s_status.interval_sec, s_status.persist);
    return 0;
}

int spectrogram_stop(void)
{
    LOCK();
    s_status.recording = false;
    if (s_fp) { fflush(s_fp); fclose(s_fp); s_fp = NULL; }
    uint32_t rows    = s_status.flash_rows;
    bool     persist = s_status.persist;
    UNLOCK();
    if (persist) write_meta(rows);
    ESP_LOGI(TAG, "recording stopped, flash_rows=%" PRIu32, rows);
    return 0;
}

int spectrogram_clear(void)
{
    LOCK();
    if (s_status.recording) { UNLOCK(); return -1; }
    s_head              = 0;
    s_count             = 0;
    s_status.ring_count = 0;
    s_status.total_rows = 0;
    s_status.flash_rows = 0;
    s_status.flash_full = false;
    UNLOCK();
    unlink(WF_DATA);
    unlink(WF_META);
    ESP_LOGI(TAG, "waterfall cleared");
    return 0;
}

void spectrogram_set_interval(uint32_t sec)
{
    if (sec < WF_INTERVAL_MIN) sec = WF_INTERVAL_MIN;
    if (sec > WF_INTERVAL_MAX) sec = WF_INTERVAL_MAX;
    LOCK(); s_status.interval_sec = sec; UNLOCK();
}

void spectrogram_set_persist(bool on)
{
    LOCK(); s_status.persist = on; UNLOCK();
}

void spectrogram_set_row_cb(wf_row_cb_t cb)
{
    s_row_cb = cb;
}

size_t spectrogram_copy_window(uint16_t *dst, size_t max_rows, uint32_t *first_total_index)
{
    LOCK();
    size_t n = s_count;
    if (n > max_rows) n = max_rows;
    size_t start = ((size_t)s_head + s_capacity - n) % s_capacity;
    for (size_t i = 0; i < n; i++) {
        size_t ri = (start + i) % s_capacity;
        memcpy(dst + i * WF_CHANNELS, s_ring + ri * WF_CHANNELS, WF_ROW_BYTES);
    }
    if (first_total_index) *first_total_index = s_status.total_rows - (uint32_t)n;
    UNLOCK();
    return n;
}

size_t spectrogram_stream_window(uint16_t *bounce, size_t max_rows,
                                 uint32_t *first_total_index,
                                 wf_emit_cb_t emit, void *ctx)
{
    // Снимок параметров окна под локом.
    LOCK();
    size_t n = s_count;
    if (max_rows && n > max_rows) n = max_rows;
    size_t start = ((size_t)s_head + s_capacity - n) % s_capacity;
    if (first_total_index) *first_total_index = s_status.total_rows - (uint32_t)n;
    UNLOCK();

    // Построчно: копия под локом в bounce, emit() — вне лока.
    // P3-5: окно — снимок best-effort. Если во время длинной выгрузки идёт
    // запись (wf_task пушит новые строки), кольцо может прокрутиться, и строки
    // у хвоста окна будут перезаписаны более свежими до того, как мы их скопируем.
    // Для мониторингового экспорта это приемлемо (а не строгий консистентный срез).
    for (size_t i = 0; i < n; i++) {
        LOCK();
        size_t ri = (start + i) % s_capacity;
        memcpy(bounce, s_ring + ri * WF_CHANNELS, WF_ROW_BYTES);
        UNLOCK();
        if (emit && !emit(ctx, bounce, WF_ROW_BYTES)) return i;
    }
    return n;
}

const char *spectrogram_data_path(void)
{
    return WF_DATA;
}
