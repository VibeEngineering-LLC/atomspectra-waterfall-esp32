#include "atomspectra.h"
#include "esp_log.h"
#include <inttypes.h>
#include "esp_littlefs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "spectrum";
#define STORAGE_PATH "/sto" "rage"
#define AUTOSAVE_FILE STORAGE_PATH "/current.bin"
#define CALIB_FILE    STORAGE_PATH "/calib.bin"
#define AUTOSAVE_RESERVE (1024 * 1024)

typedef struct {
    char serial[64];
    double calibration[CALIB_COEFFS];
    int calib_order;
    uint8_t valid;
} calib_store_t;

static spectrum_data_t s_spectrum;
static device_info_t   s_device_info;

// Защищает s_spectrum от гонки между CDC-таском (писатель) и httpd-таском (читатель).
static SemaphoreHandle_t s_spec_lock;
#define SPEC_LOCK()   do { if (s_spec_lock) xSemaphoreTake(s_spec_lock, portMAX_DELAY); } while (0)
#define SPEC_UNLOCK() do { if (s_spec_lock) xSemaphoreGive(s_spec_lock); } while (0)

void spectrum_init(void)
{
    s_spec_lock = xSemaphoreCreateMutex();
    memset(&s_spectrum, 0, sizeof(s_spectrum));
    memset(&s_device_info, 0, sizeof(s_device_info));
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        ESP_LOGI(TAG, "LittleFS: total=%zu used=%zu free=%zu", total, used, total - used);
    }
}

void spectrum_process_histogram_chunk(const uint8_t *data, size_t len)
{
    if (len < 6) return;
    SPEC_LOCK();
    uint16_t offset = data[0] | (data[1] << 8);
    size_t bin_bytes = len - 2;
    size_t bin_count = bin_bytes / 4;
    // Инкрементально правим total_counts на дельту изменённых бинов вместо
    // полного пересчёта суммы по 8192 каналам под локом на каждый chunk
    // (приходит десятками в секунду) — арифметика точная, дрейфа нет.
    int64_t delta = 0;
    for (size_t i = 0; i < bin_count && (offset + i) < SPECTRUM_CHANNELS; i++) {
        size_t idx = 2 + i * 4;
        uint32_t v = data[idx] | (data[idx+1] << 8) | (data[idx+2] << 16) | (data[idx+3] << 24);
        delta += (int64_t)v - (int64_t)s_spectrum.bins[offset + i];
        s_spectrum.bins[offset + i] = v;
    }
    s_spectrum.total_counts = (uint32_t)((int64_t)s_spectrum.total_counts + delta);
    s_spectrum.valid = true;
    SPEC_UNLOCK();
}

void spectrum_process_stat_packet(const uint8_t *data, size_t len)
{
    if (len < 10) return;
    SPEC_LOCK();
    s_spectrum.total_time_sec = data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24);
    s_spectrum.cpu_load = data[4] | (data[5] << 8);
    s_spectrum.cps = data[6] | (data[7]<<8) | (data[8]<<16) | (data[9]<<24);
    if (len >= 14)
        s_spectrum.lost_impulses = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
    SPEC_UNLOCK();
}
void spectrum_process_info_response(const char *text)
{
    SPEC_LOCK();
    device_info_t *d = &s_device_info;
    memset(d, 0, sizeof(*d));
    // static: 3 КБ парс-буфера не на стеке CDC-таска (P1-1). Функция целиком
    // выполняется под SPEC_LOCK, единственный путь вызова — безопасно.
    static char lbuf[48][64];
    int lcount = 0;
    const char *lp = text;
    while (*lp && lcount < 48) {
        int li = 0;
        while (*lp && *lp != '\n' && *lp != '\r' && li < 63)
            lbuf[lcount][li++] = *lp++;
        lbuf[lcount][li] = '\0';
        while (*lp == '\n' || *lp == '\r') lp++;
        lcount++;
    }
    if (lcount >= 40) {
        strncpy(s_spectrum.serial_number, lbuf[39], sizeof(s_spectrum.serial_number) - 1);
        ESP_LOGI(TAG, "Serial: %s", s_spectrum.serial_number);
    }
    ESP_LOGI(TAG, "Info response: %d lines", lcount);
    for (int i = 0; i < lcount && i < 12; i++)
        ESP_LOGI(TAG, "  L[%d]: \"%s\"", i, lbuf[i]);
    if (lcount >= 11) {
        char hcat[256] = {0};
        for (int i = 0; i < 10; i++)
            strncat(hcat, lbuf[i], sizeof(hcat) - strlen(hcat) - 1);
        uint32_t cc = 0;
        for (int i = 0; hcat[i]; i++) {
            cc ^= (uint8_t)hcat[i];
            for (int j = 0; j < 8; j++) {
                if (cc & 1) cc = (cc >> 1) ^ 0xEDB88320;
                else cc >>= 1;
            }
        }
        uint32_t ce = (uint32_t)strtoul(lbuf[10], NULL, 16);
        if (cc == ce) {
            for (int c = 0; c < CALIB_COEFFS && (c*2+1) < 10; c++) {
                char pair[128];
                snprintf(pair, sizeof(pair), "%s%s", lbuf[c*2], lbuf[c*2+1]);
                uint64_t raw = strtoull(pair, NULL, 16);
                double val;
                memcpy(&val, &raw, sizeof(val));
                s_spectrum.calibration[c] = val;
            }
            int order = CALIB_COEFFS - 1;
            while (order > 0 && s_spectrum.calibration[order] == 0.0) order--;
            s_spectrum.calib_order = order;
            s_spectrum.calib_valid = true;
            spectrum_save_calibration();
            ESP_LOGI(TAG, "Calibration OK: order=%d", s_spectrum.calib_order);
        } else {
            ESP_LOGW(TAG, "Calibration CRC mismatch: computed=%08x expected=%08x", (unsigned)cc, (unsigned)ce);
        }
    }
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char key[32] = {0};
        int ki = 0;
        while (*p && *p != ' ' && *p != '\n' && ki < 30) key[ki++] = *p++;
        key[ki] = 0;
        while (*p == ' ') p++;
        if (strcmp(key, "DEV") == 0) d->dev = atoi(p);
        else if (strcmp(key, "VERSION") == 0) d->version = atoi(p);
        else if (strcmp(key, "RISE") == 0) d->rise = atoi(p);
        else if (strcmp(key, "FALL") == 0) d->fall = atoi(p);
        else if (strcmp(key, "NOISE") == 0) d->noise = atoi(p);
        else if (strcmp(key, "F") == 0) d->freq = atof(p);
        else if (strcmp(key, "MAX") == 0) d->max_integral = atoi(p);
        else if (strcmp(key, "HYST") == 0) d->hyst = atoi(p);
        else if (strcmp(key, "MODE") == 0) d->mode = atoi(p);
        else if (strcmp(key, "STEP") == 0) d->step = atoi(p);
        else if (strcmp(key, "t") == 0) d->time_sec = atoi(p);
        else if (strcmp(key, "POT") == 0) d->pot = atoi(p);
        else if (strcmp(key, "T1") == 0) d->t1 = atof(p);
        else if (strcmp(key, "T2") == 0) d->t2 = atof(p);
        else if (strcmp(key, "T3") == 0) d->t3 = atof(p);
        else if (strcmp(key, "TC") == 0) d->tc_on = (strncmp(p, "ON", 2) == 0);
        else if (strcmp(key, "TP") == 0) d->tp = atoi(p);
        if (*p == '[') { while (*p && *p != ']') p++; if (*p==']') p++; }
        else { while (*p && *p != ' ' && *p != '\n') p++; }
    }
    d->valid = true;
    s_spectrum.temperature[0] = d->t1;
    s_spectrum.temperature[1] = d->t2;
    s_spectrum.temperature[2] = d->t3;
    SPEC_UNLOCK();
}
void spectrum_reset(void)
{
    SPEC_LOCK();
    memset(s_spectrum.bins, 0, sizeof(s_spectrum.bins));
    s_spectrum.total_counts = 0;
    s_spectrum.total_time_sec = 0;
    s_spectrum.cps = 0;
    s_spectrum.lost_impulses = 0;
    s_spectrum.valid = false;
    SPEC_UNLOCK();
}

// ВНИМАНИЕ: возвращают сырой указатель на разделяемое состояние БЕЗ лока.
// Допустимо ТОЛЬКО для логов/статуса (отдельные скалярные поля, чтение которых
// атомарно на ESP32). Для согласованного снимка многополевых данных
// (bins[]+total+calib) использовать spectrum_get_snapshot().
const spectrum_data_t *spectrum_get_current(void) { return &s_spectrum; }
const device_info_t   *spectrum_get_device_info(void) { return &s_device_info; }

// Атомарный снимок спектра под локом — для сетевых читателей (export/spectrum).
// Лок держится только на время memcpy; httpd_resp_send идёт уже по копии.
bool spectrum_get_snapshot(spectrum_data_t *out)
{
    SPEC_LOCK();
    memcpy(out, &s_spectrum, sizeof(*out));
    SPEC_UNLOCK();
    return out->valid;
}

int spectrum_save_to_flash(void)
{
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return -1;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return -1; }
    s_spectrum.saved_at = time(NULL);
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    if (total - used < AUTOSAVE_RESERVE + sizeof(spectrum_data_t)) {
        ESP_LOGW(TAG, "Save rejected: free=%zu < reserve=%d", total - used, AUTOSAVE_RESERVE);
        free(snap);
        return -2;
    }
    char path[64];
    int idx = 0;
    FILE *f;
    while (idx < 9999) {
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, idx);
        f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        idx++;
    }
    f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "Cannot create %s", path); free(snap); return -1; }
    size_t wr = fwrite(snap, sizeof(*snap), 1, f);
    int fc = fclose(f);
    if (wr != 1 || fc != 0) {
        ESP_LOGE(TAG, "Write to %s failed (wr=%zu fc=%d), removing", path, wr, fc);
        remove(path);
        free(snap);
        return -1;
    }
    ESP_LOGI(TAG, "Saved spectrum to %s (%" PRIu32 " counts)", path, snap->total_counts);
    free(snap);
    return idx;
}

int spectrum_load_from_flash(int index, spectrum_data_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, index);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(out, 1, sizeof(*out), f);
    fclose(f);
    return (rd == sizeof(*out)) ? 0 : -1;
}

// ВНИМАНИЕ: вызывается из info_response/set_calibration, которые уже держат SPEC_LOCK.
// Сам лок НЕ берёт (иначе deadlock на нерекурсивном мьютексе).
void spectrum_save_calibration(void)
{
    if (!s_spectrum.calib_valid) return;
    calib_store_t st = {0};
    strncpy(st.serial, s_spectrum.serial_number, sizeof(st.serial) - 1);
    memcpy(st.calibration, s_spectrum.calibration, sizeof(st.calibration));
    st.calib_order = s_spectrum.calib_order;
    st.valid = 1;
    FILE *f = fopen(CALIB_FILE, "wb");
    if (!f) return;
    size_t wr = fwrite(&st, sizeof(st), 1, f);
    if (fclose(f) != 0 || wr != 1) {
        ESP_LOGE(TAG, "Calibration write failed (wr=%zu)", wr);
        return;
    }
    ESP_LOGI(TAG, "Calibration saved for '%s'", st.serial);
}

void spectrum_set_calibration(const double *coeffs, int order)
{
    SPEC_LOCK();
    for (int i = 0; i <= order && i < CALIB_COEFFS; i++)
        s_spectrum.calibration[i] = coeffs[i];
    for (int i = order + 1; i < CALIB_COEFFS; i++)
        s_spectrum.calibration[i] = 0.0;
    s_spectrum.calib_order = order;
    s_spectrum.calib_valid = true;
    ESP_LOGI(TAG, "Manual calibration set: order=%d c0=%.6g c1=%.6g", order,
             s_spectrum.calibration[0], s_spectrum.calibration[1]);
    spectrum_save_calibration();
    SPEC_UNLOCK();
}

void spectrum_load_calibration(void)
{
    FILE *f = fopen(CALIB_FILE, "rb");
    if (!f) return;
    calib_store_t st = {0};
    size_t rd = fread(&st, 1, sizeof(st), f);
    fclose(f);
    if (rd != sizeof(st) || !st.valid) return;
    SPEC_LOCK();
    memcpy(s_spectrum.calibration, st.calibration, sizeof(st.calibration));
    s_spectrum.calib_order = st.calib_order;
    s_spectrum.calib_valid = true;
    if (st.serial[0] && !s_spectrum.serial_number[0])
        strncpy(s_spectrum.serial_number, st.serial, sizeof(s_spectrum.serial_number) - 1);
    SPEC_UNLOCK();
    ESP_LOGI(TAG, "Calibration loaded: order=%d serial='%s'", st.calib_order, st.serial);
}

void spectrum_autosave(void)
{
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return; }
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();
    FILE *f = fopen(AUTOSAVE_FILE, "wb");
    if (!f) { ESP_LOGE(TAG, "Autosave open failed"); free(snap); return; }
    size_t wr = fwrite(snap, sizeof(*snap), 1, f);
    if (fclose(f) != 0 || wr != 1)
        ESP_LOGE(TAG, "Autosave write failed (wr=%zu)", wr);
    free(snap);
}

void spectrum_restore_autosave(void)
{
    FILE *f = fopen(AUTOSAVE_FILE, "rb");
    if (!f) return;
    SPEC_LOCK();
    size_t rd = fread(&s_spectrum, 1, sizeof(s_spectrum), f);
    fclose(f);
    if (rd == sizeof(s_spectrum) && s_spectrum.valid)
        ESP_LOGI(TAG, "Restored autosave: %" PRIu32 " counts, %" PRIu32 "s", s_spectrum.total_counts, s_spectrum.total_time_sec);
    else
        memset(&s_spectrum, 0, sizeof(s_spectrum));
    SPEC_UNLOCK();
}

int spectrum_delete_from_flash(int index)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, index);
    if (remove(path) != 0) return -1;
    ESP_LOGI(TAG, "Deleted %s", path);
    return 0;
}