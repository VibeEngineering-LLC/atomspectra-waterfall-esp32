#include "atomspectra.h"
#include "spectrum_t1.h"
#include "hist_drop_diag.h"
#include "flash_quiet.h"
#include "spectrum_hist_stage.h"
#include "http_io_gate.h"
#include "backup_plan.h"   // issue #52: разбор имени снимка и план ротации
#include "spectrum_restore_plan.h"  // AWF-1: выбор источника восстановления
#include "spectrum_base_plan.h"     // AWF-3: сброс прибора и слияние база+прибор
#include "esp_log.h"
#include <stddef.h>
#include <inttypes.h>
#include "esp_littlefs.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>      // #FW-58: check unlink() result (issue #24)
#include <sys/stat.h>   // #FW-24: mkdir SPEC_DIR
#include <dirent.h>     // issue #52: обход BACKUP_DIR
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

static const char *TAG = "spectrum";
#define AUTOSAVE_FILE     STORAGE_PATH "/current.bin"
#define AUTOSAVE_TMP_FILE STORAGE_PATH "/current.bin.tmp"
#define CALIB_FILE        STORAGE_PATH "/calib.bin"
// AWF-3: спектр, накопленный до последнего обнаруженного сброса анализатора.
#define BASE_FILE         STORAGE_PATH "/base.bin"
#define BASE_TMP_FILE     STORAGE_PATH "/base.bin.tmp"
#define AUTOSAVE_RESERVE  (1024 * 1024)

typedef struct {
    char serial[64];
    double calibration[CALIB_COEFFS];
    int calib_order;
    uint8_t valid;
} calib_store_t;

static spectrum_data_t s_spectrum;
static device_info_t   s_device_info;
// AWF-1 (#3): LittleFS была отформатирована при этой загрузке (mount без
// format_if_mount_failed не удался) — /api/status обязан показать это явно.
static bool s_fs_formatted;
// P2: определение — у backup_scan() ниже (тот же раздел, тот же DIR/readdir).
static void cleanup_orphan_backup_tmp(void);
// AWF-3: определение — у spectrum_restore_autosave() ниже (тот же раздел,
// нужен atomic_write_snapshot).
static void spectrum_base_save(void);
static uint32_t s_t1_session_inf;
static bool     s_t1_refresh_sent;
static uint32_t s_t1_open_ms;

// #DEV-6: сырые тексты последних -inf / -tc_pot? ответов — источник для бэкапа
// настроек (см. spectrum_process_info_response/_tcpot_response).
// #FW-17: -inf с ПУСТЫМ PileUp[] ~404Б, но при реальном наборе таблица PileUp[]
// растёт до 99 элементов (значения-счётчики) → строка -inf ~1.2-1.5 КБ. Прежние
// 700Б молча резали её (store_raw_trimmed) на ~53-м элементе, теряя хвост PileUp[]
// и "PileUpThr N". 2048Б — с запасом на 99 крупных элементов. Tcpot-строка короче,
// ей 700Б хватает (20 пар термокомпенсации ~130Б).
static char     s_info_raw[2048];
static int      s_info_raw_len = 0;
static uint32_t s_info_raw_seq = 0;
static char     s_tcpot_raw[700];
static int      s_tcpot_raw_len = 0;
static uint32_t s_tcpot_raw_seq = 0;

// Защищает s_spectrum от гонки между CDC-таском (писатель) и httpd-таском (читатель).
static SemaphoreHandle_t s_spec_lock;
#define SPEC_LOCK()   do { if (s_spec_lock) xSemaphoreTake(s_spec_lock, portMAX_DELAY); } while (0)
#define SPEC_UNLOCK() do { if (s_spec_lock) xSemaphoreGive(s_spec_lock); } while (0)

// #WF-1: калибровка изменилась, требуется persist. Взводится под SPEC_LOCK
// (парсер -inf/-cal, set_calibration), гасится в spectrum_save_calibration
// (main loop) — flash-запись больше не выполняется под SPEC_LOCK в CDC/httpd.
static volatile bool s_calib_dirty;

// #FW-8: staging-сборка секундного свипа гистограммы. Прибор на 600000 бод шлёт
// ВЕСЬ спектр раз в секунду цепочкой chunk-ов: offset==0 — старт свипа, каждый
// следующий строго продолжает предыдущий, покрытие до 8192 каналов — свип полный
// (официальная спека AtomSpectra, пример приёма histogram). Во время flash erase
// (finalize+create сегмента водопада) кэш замораживает CDC-таск, и часть chunk-ов
// теряется: раньше они писались прямо в s_spectrum.bins, и живой спектр становился
// смесью старых и новых диапазонов каналов → дельта-строка водопада с рваными
// counts/dur («полосы на границах сегментов»). Теперь chunk-и собираются в staging
// (PSRAM) и публикуются в s_spectrum АТОМАРНО только полным свипом; рваный свип
// отбрасывается — живой спектр держит прошлый когерентный снимок. STAT-поля
// (total_time_sec и пр.) публикуются ВМЕСТЕ со свипом: время и counts замерзают/
// движутся синхронно, поэтому dur строк водопада остаётся честным.
// Staging трогает ТОЛЬКО CDC-таск (histogram и STAT приходят из одного feed_shproto)
// — лок на staging не нужен, SPEC_LOCK берётся только на публикацию.
static uint32_t *s_hist_staging;                  // [SPECTRUM_CHANNELS], PSRAM
// AWF-3: база — спектр, накопленный до последнего обнаруженного сброса
// анализатора. Показываемый s_spectrum.bins[i] = s_base_bins[i] + dev_bins[i]
// (dev — текущая накопительная гистограмма прибора, s_hist_staging на коммите).
// Тот же PSRAM-приём, что s_hist_staging (см. spectrum_init).
static uint32_t *s_base_bins;                     // [SPECTRUM_CHANNELS], PSRAM
static uint32_t  s_base_time_sec;
static uint32_t  s_base_total_counts;
static uint32_t  s_dev_resets;                    // #7: счётчик сворачиваний с боота шлюза
static spectrum_hist_stage_t s_hist_stage;        // непрерывность свипа (idle = UINT32_MAX)
static uint32_t  s_reset_gen;                     // AUD-ASW126 #1/#12: httpd Reset
static uint32_t  s_stage_reset_gen;               // снимок gen на offset==0 (CDC)
static uint32_t  s_hist_commits = 0;              // опубликованных полных свипов
static uint32_t  s_hist_drops = 0;                // отброшенных рваных свипов
typedef struct {
    uint32_t total_time_sec;
    uint16_t cpu_load;
    uint32_t cps;
    uint32_t lost_impulses;
    uint32_t pulse_width;
    bool     fresh;                               // пришёл ли STAT после последнего commit
} stat_stage_t;
static stat_stage_t s_stat_stage;

// #FW-13 фикс №2: слушатели коммита свипа. Полный свип = конец USB-burst и начало
// тихого окна (~0.5 с до следующего свипа) — единственная фаза, где flash-запись
// (freeze кэша обоих ядер) не рвёт приём FTDI (FIFO 256 Б = 4.3 мс @600000 бод).
// Потребители (wf_task — строка водопада, main loop — autosave, monitor_task —
// серия CPS #MON-1) привязывают свои записи к этому окну через binary-семафоры.
// Occupied: main autosave, monitor, wf s_commit_sig, wf s_fs_quiet_sig.
// 6 = 4 used + 2 spare (overflow is silent LOGW; consumer falls back to tick).
#define COMMIT_LISTENERS_MAX 6
static SemaphoreHandle_t s_commit_listeners[COMMIT_LISTENERS_MAX];

void spectrum_add_commit_listener(void *sem)
{
    for (int i = 0; i < COMMIT_LISTENERS_MAX; i++)
        if (!s_commit_listeners[i]) { s_commit_listeners[i] = (SemaphoreHandle_t)sem; return; }
    ESP_LOGW(TAG, "commit listeners full");
}

void spectrum_init(void)
{
    s_spec_lock = xSemaphoreCreateMutex();
    memset(&s_spectrum, 0, sizeof(s_spectrum));
    memset(&s_device_info, 0, sizeof(s_device_info));
    // #FW-8: 32 КБ staging в PSRAM (пишется из CDC-таска ~130 chunk-ов/с — трафик
    // копеечный). Нет PSRAM → internal heap; нет и его → legacy-путь прямой записи.
    s_hist_staging = heap_caps_malloc(SPECTRUM_CHANNELS * sizeof(uint32_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hist_staging)
        s_hist_staging = malloc(SPECTRUM_CHANNELS * sizeof(uint32_t));
    if (!s_hist_staging)
        ESP_LOGE(TAG, "hist staging alloc failed — fallback to direct bin writes");
    // AWF-3: та же PSRAM-схема, что у s_hist_staging (32 КБ, тот же трафик —
    // пишется раз в коммит, не каждый chunk). База нулевая, пока
    // spectrum_restore_base() (main.c, после mount) её не заполнит с flash.
    s_base_bins = heap_caps_malloc(SPECTRUM_CHANNELS * sizeof(uint32_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_base_bins)
        s_base_bins = malloc(SPECTRUM_CHANNELS * sizeof(uint32_t));
    if (s_base_bins)
        memset(s_base_bins, 0, SPECTRUM_CHANNELS * sizeof(uint32_t));
    else
        ESP_LOGE(TAG, "base bins alloc failed — AWF-3 base merge disabled");
    spectrum_hist_stage_reset(&s_hist_stage);
    // AWF-1 (#3): раньше format_if_mount_failed=true стирал раздел МОЛЧА —
    // событие видно только по факту пустого current.bin. Теперь пробуем БЕЗ
    // формата первыми, и если mount не удался, форматируем явно и громко
    // (ESP_LOGE с кодом ошибки), выставляя s_fs_formatted для /api/status.
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed (%s) — formatting partition 'storage'",
                 esp_err_to_name(ret));
        esp_err_t fmt = esp_littlefs_format("storage");
        if (fmt != ESP_OK) {
            ESP_LOGE(TAG, "LittleFS format failed: %s", esp_err_to_name(fmt));
        } else {
            s_fs_formatted = true;
            ret = esp_vfs_littlefs_register(&conf);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        ESP_LOGI(TAG, "LittleFS: total=%zu used=%zu free=%zu", total, used, total - used);
        if (s_fs_formatted)
            ESP_LOGE(TAG, "LittleFS was reformatted this boot — accumulated data on flash is LOST");
        mkdir(SPEC_DIR, 0777);   // #FW-24: подкаталог сохранённых спектров (отделение от calib/current/wf_state в корне)
        mkdir(BACKUP_DIR, 0777); // issue #52: автоснимки — отдельно от ручных, чтобы ротация их не касалась
        cleanup_orphan_backup_tmp();  // P2: осиротевшие bk_*.bin.tmp после обрыва питания
    }
}

// AWF-1 (#3): для /api/status — «ФС отформатирована при старте».
bool spectrum_fs_was_formatted(void)
{
    return s_fs_formatted;
}

// AWF-3 доработка (живой тест 25.09): свёртка базы и слияние — ОДНОЙ функцией
// spectrum_base_commit() (main/spectrum_base_plan.h). До фикса fold (по STAT)
// и merge шли раздельно: merge выполнялся ВСЕГДА, а fold — только при свежем
// STAT; на первом коммите после реального сброса без свежего STAT merge
// затирал восстановленный показанный спектр нулевой базой ДО того, как fold
// успевал его сохранить (issue 25.09, база получила 9 вместо 267049).
// spectrum_base_commit() проверяет сброс (по счёту — ВСЕГДА; по времени —
// если STAT свежий) СТРОГО до merge — тот же код, что и host-тест.
static bool commit_fold_and_merge_locked(bool stat_fresh, uint32_t t_new_raw, uint64_t dev_total)
{
    if (!s_base_bins) {
        // Деградация: alloc базы не удался (spectrum_init) — старое прямое
        // зеркалирование прибора без базы, как до AWF-3.
        memcpy(s_spectrum.bins, s_hist_staging, SPECTRUM_CHANNELS * sizeof(uint32_t));
        s_spectrum.total_counts = (uint32_t)dev_total;
        return false;
    }
    spectrum_base_state_t st = { s_base_bins, s_base_time_sec, s_base_total_counts,
                                  s_spectrum.bins, s_spectrum.total_time_sec, s_spectrum.total_counts };
    bool did_reset = spectrum_base_commit(&st, s_hist_staging, (uint32_t)dev_total,
                                           SPECTRUM_CHANNELS, stat_fresh, t_new_raw);
    s_base_time_sec = st.base_time;
    s_base_total_counts = st.base_counts;
    s_spectrum.total_counts = st.shown_counts;
    if (did_reset) s_dev_resets++;
    return did_reset;
}

static void commit_apply_time_stat_fresh_locked(uint32_t t_new_raw, uint32_t base_now,
                                                uint32_t dev_prev, uint32_t expected)
{
    uint32_t t_new = t_new_raw;
    if (s_spectrum.valid && t_new + 5 >= dev_prev && t_new < expected)
        t_new = expected;
    s_spectrum.total_time_sec = base_now + t_new;
    s_spectrum.cpu_load       = s_stat_stage.cpu_load;
    s_spectrum.cps            = s_stat_stage.cps;
    s_spectrum.lost_impulses  = s_stat_stage.lost_impulses;
    s_spectrum.pulse_width    = s_stat_stage.pulse_width;
    s_stat_stage.fresh = false;
}

static void commit_apply_time_locked(bool stat_fresh, uint32_t t_new_raw, uint32_t base_now,
                                     uint32_t dev_prev, uint32_t expected)
{
    if (stat_fresh)
        commit_apply_time_stat_fresh_locked(t_new_raw, base_now, dev_prev, expected);
    else if (s_spectrum.valid)
        s_spectrum.total_time_sec = base_now + expected;   // STAT потерян
    else
        s_spectrum.total_time_sec = base_now + 1;           // первый коммит без STAT
}

// #FW-12, обобщено на dev-относительное время (dev_prev = shown-база; база
// прибавляется в конце). Тождественно старому поведению при base=0, и
// корректно сразу после сворачивания: dev_prev==0 автоматически. Откат >=5с
// раньше трактовался как рестарт прибора — теперь база сворачивается
// (commit_fold_and_merge_locked), не молча принимается t_new абсолютом.
static void commit_time_locked(bool stat_fresh, uint32_t t_new_raw)
{
    static uint32_t s_drops_at_commit = 0;
    uint32_t drops_delta = s_hist_drops - s_drops_at_commit;
    s_drops_at_commit = s_hist_drops;
    uint32_t base_now = s_base_bins ? s_base_time_sec : 0;
    uint32_t dev_prev = s_spectrum.valid ? (s_spectrum.total_time_sec - base_now) : 0;
    uint32_t expected = dev_prev + 1 + drops_delta;
    commit_apply_time_locked(stat_fresh, t_new_raw, base_now, dev_prev, expected);
}

void spectrum_process_histogram_chunk(const uint8_t *data, size_t len)
{
    if (len < 6) return;
    uint16_t offset = data[0] | (data[1] << 8);
    size_t bin_count = (len - 2) / 4;

    if (!s_hist_staging) {
        // Legacy-путь (staging не выделился): прямая запись с инкрементальным total.
        SPEC_LOCK();
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
        return;
    }

    // #FW-8: сборка свипа в staging. offset==0 — старт нового свипа (официальная
    // спека); разрыв непрерывности = потерянный chunk (flash-freeze) → свип битый.
    // Непрерывность — spectrum_hist_stage_* (тот же код, что host-тест).
    if (offset == 0) {
        hist_drop_diag_note_burst_start();
        SPEC_LOCK();
        s_stage_reset_gen = s_reset_gen;
        SPEC_UNLOCK();
    }
    bool was_ok = s_hist_stage.ok;
    uint32_t exp_before = s_hist_stage.next_offset;
    spectrum_hist_stage_note_chunk(&s_hist_stage, offset, (uint32_t)bin_count);
#if HIST_DROP_DIAG
    if (was_ok && !s_hist_stage.ok) {
        ESP_LOGW(TAG,
                 "histogram gap exp=%" PRIu32 " got=%u filled=%" PRIu32
                 " as=%d wf=%d/%s",
                 exp_before, (unsigned)offset, exp_before,
                 hist_drop_diag_autosave_active() ? 1 : 0,
                 hist_drop_diag_wf_active() ? 1 : 0,
                 hist_drop_diag_wf_tag());
    }
#else
    (void)was_ok;
    (void)exp_before;
#endif
    for (size_t i = 0; i < bin_count && (offset + i) < SPECTRUM_CHANNELS; i++) {
        size_t idx = 2 + i * 4;
        s_hist_staging[offset + i] =
            data[idx] | (data[idx+1] << 8) | (data[idx+2] << 16) | (data[idx+3] << 24);
    }

    if (spectrum_hist_stage_complete(&s_hist_stage, SPECTRUM_CHANNELS)) {
        if (s_hist_stage.ok) {
            // Полный свип: сумма вне лока (staging приватен CDC-таску), публикация
            // атомарно — bins + STAT одним куском, время когерентно counts.
            uint64_t total = 0;
            for (size_t i = 0; i < SPECTRUM_CHANNELS; i++) total += s_hist_staging[i];
            SPEC_LOCK();
            if (s_reset_gen != s_stage_reset_gen) {
                SPEC_UNLOCK();
                s_hist_drops++;
                ESP_LOGW(TAG, "histogram sweep dropped (reset during sweep)");
                spectrum_hist_stage_reset(&s_hist_stage);
                return;
            }
            // AWF-3: свернуть базу (если прибор перезапустился), слить
            // база+свип, обновить время (#FW-12, обобщено на dev-время).
            bool stat_fresh = s_stat_stage.fresh;
            uint32_t t_new_raw = stat_fresh ? s_stat_stage.total_time_sec : 0;
            bool did_reset = commit_fold_and_merge_locked(stat_fresh, t_new_raw, total);
            commit_time_locked(stat_fresh, t_new_raw);
            s_spectrum.valid = true;
            SPEC_UNLOCK();
            if (did_reset) spectrum_base_save();   // flash-запись, ВНЕ лока
            s_hist_commits++;
            flash_quiet_note_commit();
            hist_drop_diag_note_commit();
            // #FW-13 фикс №2: сигнал «burst кончился, тихое окно открыто».
            for (int i = 0; i < COMMIT_LISTENERS_MAX; i++)
                if (s_commit_listeners[i]) xSemaphoreGive(s_commit_listeners[i]);
        } else {
            s_hist_drops++;
#if HIST_DROP_DIAG
            ESP_LOGW(TAG,
                     "histogram sweep dropped (gap in chunks), drops=%" PRIu32
                     " ok=%" PRIu32 " dt_commit_ms=%lld dt_as_end_ms=%lld"
                     " as=%d as_to=%d wf=%d/%s",
                     s_hist_drops, s_hist_commits,
                     (long long)hist_drop_diag_ms_since_commit(),
                     (long long)hist_drop_diag_ms_since_autosave_end(),
                     hist_drop_diag_autosave_active() ? 1 : 0,
                     hist_drop_diag_last_wait_timed_out() ? 1 : 0,
                     hist_drop_diag_wf_active() ? 1 : 0,
                     hist_drop_diag_wf_tag());
#else
            ESP_LOGW(TAG, "histogram sweep dropped (gap in chunks), drops=%" PRIu32, s_hist_drops);
#endif
        }
        spectrum_hist_stage_reset(&s_hist_stage);
    }
}

void spectrum_process_stat_packet(const uint8_t *data, size_t len)
{
    if (len < 10) return;
    // #FW-8: STAT — в staging, публикация вместе со свипом гистограммы (когерентность
    // время↔counts для dur строк водопада). Без staging — старый прямой путь.
    if (s_hist_staging) {
        s_stat_stage.total_time_sec = data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24);
        s_stat_stage.cpu_load = data[4] | (data[5] << 8);
        s_stat_stage.cps = data[6] | (data[7]<<8) | (data[8]<<16) | (data[9]<<24);
        if (len >= 14)
            s_stat_stage.lost_impulses = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
        // #DT-4: суммарная ширина импульсов (отсчёты АЦП), STAT offset 14. Диагностика;
        // мёртвое время считается методом BecqMoni (RISE+FALL+1)/F, в расчёт не идёт.
        if (len >= 18)
            s_stat_stage.pulse_width = data[14] | (data[15]<<8) | (data[16]<<16) | (data[17]<<24);
        s_stat_stage.fresh = true;
        return;
    }
    SPEC_LOCK();
    s_spectrum.total_time_sec = data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24);
    s_spectrum.cpu_load = data[4] | (data[5] << 8);
    s_spectrum.cps = data[6] | (data[7]<<8) | (data[8]<<16) | (data[9]<<24);
    if (len >= 14)
        s_spectrum.lost_impulses = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
    if (len >= 18)
        s_spectrum.pulse_width = data[14] | (data[15]<<8) | (data[16]<<16) | (data[17]<<24);
    SPEC_UNLOCK();
}

// #FW-8: диагностика сборки свипов для /api/spectrum JSON и верификации фикса.
void spectrum_get_hist_stats(uint32_t *commits, uint32_t *drops)
{
    if (commits) *commits = s_hist_commits;
    if (drops)   *drops   = s_hist_drops;
}
// Копирует text (без хвостовых \r\n\пробел) в raw-буфер фиксированного размера,
// бампает seq. Общий хелпер для -inf и -tc_pot? (вызывать ТОЛЬКО под SPEC_LOCK).
static void store_raw_trimmed(const char *text, char *buf, size_t bufsz, int *out_len, uint32_t *out_seq)
{
    size_t n = strlen(text);
    while (n > 0 && (text[n-1] == '\n' || text[n-1] == '\r' || text[n-1] == ' ')) n--;
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';
    *out_len = (int)n;
    (*out_seq)++;
}

void spectrum_process_info_response(const char *text)
{
    SPEC_LOCK();
    // #BRIDGE-3: этот вход обслуживает ДВА разных ответа прибора — роутер
    // (usb_host_cdc.c) шлёт сюда и -inf (параметры+температура), и -cal (дамп
    // 40 hex-регистров: калибровка+CRC+серийник). Различаем по содержимому:
    // -inf несёт ключ "VERSION ", -cal ключей не содержит. РАНЬШЕ -cal шёл в ту
    // же ветку memset(device_info)+key-парсер → ключей нет → температура и версия
    // обнулялись (симптом «t=0»); серийник брался из lbuf[39] безусловно и при
    // сдвиге строк (склейка под нагрузкой bridge) вставал в FFFFFFFF — в дампе
    // 28 строк FFFFFFFF («пустые» слоты калибровки), любой сдвиг индекса туда попадал.
    bool is_inf = (strstr(text, "VERSION ") != NULL);
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
    // #FW-13: LOGD, не LOGI — функция в CDC-таске, консоль UART0 115200 блокирующая
    // (FTDI FT232R 256 Б FIFO = 4.3 мс при 600000 бод → overflow, gap в chunk-ах).
    ESP_LOGD(TAG, "Info response: %d lines (%s)", lcount, is_inf ? "inf" : "cal");
    // Калибровка: только у -cal (CRC L[0..9]==L[10], ≥40 строк). Для -inf это
    // одна строка (lcount<11) → блок штатно пропускается, калибровку не трогает.
    if (lcount >= 11) {
        char hcat[256] = {0};
        for (int i = 0; i < 10; i++)
            strncat(hcat, lbuf[i], sizeof(hcat) - strlen(hcat) - 1);
        // #CMD-1: прибор считает СТАНДАРТНЫЙ CRC32 (init 0xFFFFFFFF, рефлексия,
        // финальный XOR 0xFFFFFFFF) по ASCII-конкатенации регистров L[0..9].
        // Подтверждено на реальном дампе -cal: CRC32("BFF9A132...00000000")=DF786A7E,
        // совпадает с регистром L[10]. Прежний init=0 без финального XOR давал mismatch.
        uint32_t cc = 0xFFFFFFFF;
        for (int i = 0; hcat[i]; i++) {
            cc ^= (uint8_t)hcat[i];
            for (int j = 0; j < 8; j++) {
                if (cc & 1) cc = (cc >> 1) ^ 0xEDB88320;
                else cc >>= 1;
            }
        }
        cc ^= 0xFFFFFFFF;
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
            s_calib_dirty = true;   // #WF-1: запись сделает main loop вне SPEC_LOCK
            ESP_LOGI(TAG, "Calibration OK: order=%d", s_spectrum.calib_order);
        } else {
            // #FW-13: LOGD — для -inf mismatch штатен (CRC-формат только у -cal),
            // WARN здесь печатался каждые 30 с в CDC-таске (см. комментарий выше).
            ESP_LOGD(TAG, "Calibration CRC mismatch: computed=%08x expected=%08x", (unsigned)cc, (unsigned)ce);
        }
    }

    if (!is_inf) {
        // #BRIDGE-3: -cal обновляет ТОЛЬКО серийник (L39), device_info/температуру
        // НЕ трогает и s_info_raw (сырой -inf для бэкапа) не перетирает. Серийник —
        // с проверкой: ровно 8 hex-символов и не «FFFFFFFF» (пустой слот). При
        // сдвиге строк lbuf[39] попадает в блок FFFFFFFF → отбрасываем, прежнее
        // валидное значение сохраняется (серийник больше не «портится» под bridge).
        if (lcount >= 40) {
            const char *sn = lbuf[39];
            size_t sl = strlen(sn);
            bool ok = (sl == 8);
            int fcnt = 0;
            for (size_t i = 0; ok && i < sl; i++) {
                char c = sn[i];
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) ok = false;
                if (c == 'F' || c == 'f') fcnt++;
            }
            if (ok && fcnt != 8) {
                strncpy(s_spectrum.serial_number, sn, sizeof(s_spectrum.serial_number) - 1);
                ESP_LOGI(TAG, "Serial: %s", s_spectrum.serial_number);
            } else {
                ESP_LOGD(TAG, "Serial rejected (misaligned -cal): \"%s\"", sn);
            }
        }
        SPEC_UNLOCK();
        return;
    }

    // ===== -inf: параметры прибора + температура + версия (серийника не несёт) =====
    // #DEV-6: сырой текст -inf для бэкапа настроек — структурный парсер ниже
    // хранит лишь подмножество полей (POT2/Tco[]/PileUp[]/PileUpThr/Prise/Pfall/
    // TCpot он молча пропускает), бэкап берёт полную строку.
    store_raw_trimmed(text, s_info_raw, sizeof(s_info_raw), &s_info_raw_len, &s_info_raw_seq);
    device_info_t *d = &s_device_info;
    memset(d, 0, sizeof(*d));
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
        else if (strcmp(key, "Srise") == 0) d->srise = atoi(p);
        else if (strcmp(key, "Sfall") == 0) d->sfall = atoi(p);
        else if (strcmp(key, "NOISE") == 0) d->noise = atoi(p);
        else if (strcmp(key, "F") == 0) d->freq = atof(p);
        else if (strcmp(key, "MAX") == 0) d->max_integral = atoi(p);
        else if (strcmp(key, "HYST") == 0) d->hyst = atoi(p);
        else if (strcmp(key, "MODE") == 0) d->mode = atoi(p);
        else if (strcmp(key, "STEP") == 0) d->step = atoi(p);
        else if (strcmp(key, "t") == 0) d->time_sec = atoi(p);
        else if (strcmp(key, "POT") == 0) d->pot = atoi(p);
        else if (strcmp(key, "T1") == 0) d->t1 = spectrum_temp_from_token(p);
        else if (strcmp(key, "T2") == 0) d->t2 = spectrum_temp_from_token(p);
        else if (strcmp(key, "T3") == 0) d->t3 = spectrum_temp_from_token(p);
        else if (strcmp(key, "TC") == 0) d->tc_on = (strncmp(p, "ON", 2) == 0);
        else if (strcmp(key, "TP") == 0) d->tp = atoi(p);
        if (*p == '[') { while (*p && *p != ']') p++; if (*p==']') p++; }
        else { while (*p && *p != ' ' && *p != '\n') p++; }
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (spectrum_t1_hold_active(s_t1_session_inf, now_ms, s_t1_open_ms))
        d->t1 = NAN;   /* до open+5s: T1 не публиковать (в т.ч. reconnect retry) */
    s_t1_session_inf++;
    d->valid = true;
    s_spectrum.temperature[0] = d->t1;
    s_spectrum.temperature[1] = d->t2;
    s_spectrum.temperature[2] = d->t3;
    SPEC_UNLOCK();
}

void spectrum_t1_on_cdc_open(uint32_t now_ms)
{
    SPEC_LOCK();
    s_t1_session_inf = 0;
    s_t1_refresh_sent = false;
    s_t1_open_ms = now_ms;
    s_device_info.t1 = NAN;
    s_spectrum.temperature[0] = NAN;
    SPEC_UNLOCK();
}

void spectrum_t1_on_cdc_teardown(void)
{
    SPEC_LOCK();
    s_t1_session_inf = 0;
    s_t1_refresh_sent = false;
    s_t1_open_ms = 0;
    s_device_info.t1 = NAN;
    s_spectrum.temperature[0] = NAN;
    SPEC_UNLOCK();
}

bool spectrum_t1_refresh_due(uint32_t now_ms)
{
    bool due;
    SPEC_LOCK();
    due = (s_t1_session_inf >= 1) && !s_t1_refresh_sent &&
          (now_ms - s_t1_open_ms) >= SPECTRUM_T1_REFRESH_MS;
    SPEC_UNLOCK();
    return due;
}

void spectrum_t1_mark_refresh_sent(void)
{
    SPEC_LOCK();
    s_t1_refresh_sent = true;
    SPEC_UNLOCK();
}
// #DEV-6: ответ на -tc_pot? ("Tcpot [-40 51 -16 45 ...]") — отдельная команда,
// таблица баз. темп. компенсации НЕ входит в -inf (см. #DOC-3/BUG-AS-08).
void spectrum_process_tcpot_response(const char *text)
{
    SPEC_LOCK();
    store_raw_trimmed(text, s_tcpot_raw, sizeof(s_tcpot_raw), &s_tcpot_raw_len, &s_tcpot_raw_seq);
    SPEC_UNLOCK();
}

int spectrum_get_info_raw(char *out, size_t outsz, uint32_t *out_seq)
{
    SPEC_LOCK();
    int n = s_info_raw_len < (int)outsz - 1 ? s_info_raw_len : (int)outsz - 1;
    if (n > 0) memcpy(out, s_info_raw, n);
    out[n] = '\0';
    if (out_seq) *out_seq = s_info_raw_seq;
    SPEC_UNLOCK();
    return n;
}

int spectrum_get_tcpot_raw(char *out, size_t outsz, uint32_t *out_seq)
{
    SPEC_LOCK();
    int n = s_tcpot_raw_len < (int)outsz - 1 ? s_tcpot_raw_len : (int)outsz - 1;
    if (n > 0) memcpy(out, s_tcpot_raw, n);
    out[n] = '\0';
    if (out_seq) *out_seq = s_tcpot_raw_seq;
    SPEC_UNLOCK();
    return n;
}

void spectrum_reset(void)
{
    // AUD-ASW126 #1/#12: httpd must not fclose autosave FILE* or write s_stage_*.
    // CDC drops an in-flight sweep via s_reset_gen; main consumes abort.
    spectrum_autosave_abort();
    // #FW-58 (issue #24): unlink the already-committed autosave file HERE, not only
    // via the deferred spectrum_autosave_consume_abort() on the next main-loop tick
    // (up to ~60 s, or longer if consume is starved). AUTOSAVE_FILE is the rename()
    // target, distinct from AUTOSAVE_TMP_FILE and from the writer-owned s_as_fp/
    // s_as_snap state — removing it here is metadata-only and does not race the
    // sliced writer. Without this, Reset (handle_reset httpd handler answers
    // "ok":true immediately) followed by a reboot/power cut inside the deferred
    // window left current.bin on flash; spectrum_restore_autosave() at boot then
    // silently resurrected the pre-reset spectrum — the Reset undid itself.
    if (unlink(AUTOSAVE_FILE) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "Reset: current.bin unlink failed (errno=%d)", errno);
    // F2 (итоговое ревью 25.09): restore_resolve() читает current.bin.tmp как
    // запасной источник, если current.bin негоден (spectrum.c restore_resolve:
    // load_valid_snapshot(AUTOSAVE_TMP_FILE,...)) — Reset обязан удалять ВСЕ
    // файлы, которые читает восстановление, иначе нарезанный tmp, дописанный
    // ДО Reset, переживает Reset и при следующей загрузке «воскрешает»
    // сброшенный спектр (тот же класс, что #FW-58 «Reset undid itself»,
    // другим путём — tmp, а не current.bin).
    if (unlink(AUTOSAVE_TMP_FILE) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "Reset: current.bin.tmp unlink failed (errno=%d)", errno);
    // AWF-3 (#4): очистка спектра обязана обнулить и базу — иначе первый же
    // коммит после Reset «восстановит» старый спектр через base+dev.
    if (unlink(BASE_FILE) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "Reset: base.bin unlink failed (errno=%d)", errno);
    // F2: base.bin.tmp — restore не читает его СЕЙЧАС (spectrum_restore_base()
    // проверяет только BASE_FILE, без tmp-фолбэка), но удаляем на случай
    // будущего добавления такого фолбэка и для чистоты (тот же принцип
    // «Reset обязан удалить всё, что могло бы быть прочитано»).
    if (unlink(BASE_TMP_FILE) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "Reset: base.bin.tmp unlink failed (errno=%d)", errno);
    SPEC_LOCK();
    s_reset_gen++;
    memset(s_spectrum.bins, 0, sizeof(s_spectrum.bins));
    s_spectrum.total_counts = 0;
    s_spectrum.total_time_sec = 0;
    s_spectrum.cps = 0;
    s_spectrum.lost_impulses = 0;
    s_spectrum.pulse_width = 0;
    s_spectrum.valid = false;
    if (s_base_bins) memset(s_base_bins, 0, SPECTRUM_CHANNELS * sizeof(uint32_t));
    s_base_time_sec = 0;
    s_base_total_counts = 0;
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

// #MON-1: атомарная пара (total_counts, total_time_sec) под тем же SPEC_LOCK,
// каким защищена публикация коммита свипа. Дешёвая альтернатива
// spectrum_get_snapshot() для потребителей раз-в-секунду (монитор CPS):
// не копирует 32 КБ bins на каждый тик.
void spectrum_get_totals(uint32_t *counts, uint32_t *time_sec)
{
    SPEC_LOCK();
    if (counts)   *counts   = s_spectrum.total_counts;
    if (time_sec) *time_sec = s_spectrum.total_time_sec;
    SPEC_UNLOCK();
}

int spectrum_save_to_flash(void)
{
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return -3;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return -1; }  // #FW-24: нет валидного спектра
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
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", SPEC_DIR, idx);
        f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        idx++;
    }
    // #FW-59 (задача #3/Codeaudit P1): idx==9999 значит слоты 0..9998 заняты —
    // цикл выше НЕ выполнил тело для idx=9999 (условие while сработало раньше),
    // поэтому `path` всё ещё держит имя ПОСЛЕДНЕГО проверенного файла (spec_9998.bin).
    // Без этой проверки следующий fopen(path,"wb") молча перезаписал бы
    // spec_9998.bin, а вызывающему вернулся бы idx=9999 — имя файла и
    // сообщённый индекс разошлись бы, спектр по факту потерян.
    if (idx >= 9999) {
        ESP_LOGE(TAG, "Save rejected: spec_XXXX.bin slots exhausted (0..9998 all taken)");
        free(snap);
        return -4;
    }
    f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "Cannot create %s", path); free(snap); return -3; }
    size_t wr = fwrite(snap, sizeof(*snap), 1, f);
    int fc = fclose(f);
    if (wr != 1 || fc != 0) {
        ESP_LOGE(TAG, "Write to %s failed (wr=%zu fc=%d), removing", path, wr, fc);
        remove(path);
        free(snap);
        return -3;
    }
    ESP_LOGI(TAG, "Saved spectrum to %s (%" PRIu32 " counts)", path, snap->total_counts);
    free(snap);
    return idx;
}

// Приводит поля, прочитанные С ФЛЕША, в заведомо безопасный вид. Потребители
// (экспорт xml/csv/spe/json) ходят по calibration[] до calib_order включительно,
// не проверяя границу: повреждённая ячейка LittleFS с calib_order=1000 дала бы
// чтение далеко за массивом из CALIB_COEFFS элементов. Совпадение размера файла
// со структурой (проверяется у вызывающих) целостности ПОЛЕЙ не гарантирует.
static void sanitize_loaded(spectrum_data_t *out)
{
    if (out->calib_order < 0 || out->calib_order >= CALIB_COEFFS) {
        if (out->calib_valid)
            ESP_LOGW(TAG, "loaded spectrum: calib_order=%d out of range, calibration dropped",
                     out->calib_order);
        out->calib_order = 0;
        out->calib_valid = false;
    }
    out->serial_number[sizeof(out->serial_number) - 1] = '\0';  // строка из файла
}

// ─── issue #52: автоматические резервные снимки ───────────────────────────────
// Решение «какие файлы удалить» вынесено в host-тестируемый backup_plan.c; здесь
// только ввод-вывод. Вся функция работает под http_io_gate (держит вызывающий).

// P2: осиротевшие bk_*.bin.tmp (обрыв питания между fopen(tmp) и rename в
// atomic_write_snapshot) — backup_parse_name их не видит (".bin.tmp" != ".bin"),
// поэтому ни ротация, ни restore их никогда не тронут; чистим раз на старте.
// Путь восстанавливаем из РАЗОБРАННЫХ чисел, не из d_name (%s до 255 Б) —
// иначе -Werror=format-truncation (тот же приём, что web_server.c:654).
static bool cleanup_one_backup_tmp(const char *name)
{
    size_t len = strlen(name);
    if (len < 5 || strcmp(name + len - 4, ".tmp") != 0) return false;
    char stripped[64];
    if (len - 4 >= sizeof(stripped)) return false;
    memcpy(stripped, name, len - 4);
    stripped[len - 4] = '\0';
    backup_id_t id;
    if (!backup_parse_name(stripped, &id)) return false;
    char path[88];
    snprintf(path, sizeof(path), "%s/" BACKUP_NAME_FMT ".tmp", BACKUP_DIR, id.sess, id.seq);
    return remove(path) == 0;
}

static void cleanup_orphan_backup_tmp(void)
{
    DIR *dir = opendir(BACKUP_DIR);
    if (!dir) return;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL)
        if (cleanup_one_backup_tmp(de->d_name)) n++;
    closedir(dir);
    if (n > 0) ESP_LOGW(TAG, "backup: removed %d orphaned tmp file(s) at boot", n);
}

// Собирает идентификаторы снимков каталога. Возвращает число найденных, либо -1
// при ошибке открытия каталога. Файлы, не подходящие под шаблон (в т.ч. чужие
// .bak), пропускаются и в ротацию не попадают.
static int backup_scan(backup_id_t *out, int cap)
{
    DIR *dir = opendir(BACKUP_DIR);
    if (!dir) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        backup_id_t id;
        if (!backup_parse_name(de->d_name, &id)) continue;
        if (n < cap) out[n] = id;
        n++;                       // считаем и сверх cap — вызывающий увидит переполнение
    }
    closedir(dir);
    return n;
}

// P2 (второе независимое ревью 9453113): атомарная запись spectrum_data_t —
// общий хелпер для autosave (current.bin) и резервных снимков (bk_*.bin).
// tmp_path обязан быть уникален для вызывающего. true при успехе; при любой
// неудаче tmp_path подчищен (unlink), final_path не тронут.
static bool atomic_write_snapshot(const char *tmp_path, const char *final_path,
                                  const spectrum_data_t *data)
{
    unlink(tmp_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    size_t wr = fwrite(data, sizeof(*data), 1, f);
    int cl = fclose(f);
    if (wr != 1 || cl != 0) { unlink(tmp_path); return false; }
    if (rename(tmp_path, final_path) != 0) { unlink(tmp_path); return false; }
    return true;
}

int spectrum_backup_save(uint32_t sess, uint32_t seq, int keep)
{
    if (keep <= 0) return -1;

    // Место проверяем ПЕРВЫМ делом: при заполненном разделе отказ должен стоить
    // одного вызова esp_littlefs_info, а не malloc(33 КБ) + SPEC_LOCK + обход
    // каталога на каждой безнадёжной попытке (планировщик повторяет раз в тик).
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    if (total - used < AUTOSAVE_RESERVE + sizeof(spectrum_data_t)) {
        ESP_LOGW(TAG, "backup rejected: free=%zu < reserve=%d", total - used, AUTOSAVE_RESERVE);
        return -2;
    }

    // Снимок берём ДО ротации: если спектра нет, удалять старые незачем.
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return -3;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return -1; }
    s_spectrum.saved_at = time(NULL);
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();

    // Сериализация писателей LittleFS: тот же лок, что берут autosave, калибровка
    // и запись сегментов водопада. Без него снимок может уйти на flash РЯДОМ с
    // записью сегмента — а лок ровно для этого и заведён (flash_quiet.h).
    // Держим только вокруг файлового ввода-вывода, без ожиданий коммита внутри.
    if (!flash_quiet_writer_lock(pdMS_TO_TICKS(200))) {
        free(snap);
        return -4;                      // занято другим писателем — повторим позже
    }

    // Ротация до записи — освобождаем место под новый файл.
    //
    // Массивы static, а не на стеке: 2 × 40 × 8 Б = 640 Б, а main-task, откуда
    // приходит планировщик, имеет самый маленький стек среди задач с флеш-вводом
    // (CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584 против 8192 у httpd), причём
    // CONFIG_COMPILER_STACK_CHECK_MODE_NONE — переполнение не поймалось бы.
    // Гонки нет: функция вызывается только под flash_quiet_writer_lock (взят выше),
    // который сериализует всех писателей LittleFS.
    static backup_id_t have[BACKUP_KEEP_MAX * 2];
    static backup_id_t del[BACKUP_KEEP_MAX * 2];
    int n = backup_scan(have, (int)(sizeof(have) / sizeof(have[0])));
    if (n < 0) {
        // Каталога нет (первый снимок после обновления прошивки) — создаём.
        if (mkdir(BACKUP_DIR, 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "backup: cannot create %s (errno=%d)", BACKUP_DIR, errno);
            flash_quiet_writer_unlock();
            free(snap);
            return -3;
        }
        n = 0;
    }
    if (n > (int)(sizeof(have) / sizeof(have[0]))) {
        ESP_LOGW(TAG, "backup: %d snapshots, scan capped at %d", n,
                 (int)(sizeof(have) / sizeof(have[0])));
        n = (int)(sizeof(have) / sizeof(have[0]));
    }
    int ndel = backup_rotate_plan(have, n, keep, del, (int)(sizeof(del) / sizeof(del[0])));
    if (ndel < 0) {
        ESP_LOGE(TAG, "backup: rotate plan refused (n=%d keep=%d)", n, keep);
        flash_quiet_writer_unlock();
        free(snap);
        return -3;
    }
    int nfail = 0;
    for (int i = 0; i < ndel; i++) {
        char p[80];
        snprintf(p, sizeof(p), "%s/" BACKUP_NAME_FMT, BACKUP_DIR, del[i].sess, del[i].seq);
        if (remove(p) != 0) {
            nfail++;
            ESP_LOGW(TAG, "backup: cannot remove %s (errno=%d)", p, errno);
        } else {
            ESP_LOGI(TAG, "backup: rotated out %s", p);
        }
    }
    if (nfail > 0 && ndel - nfail == 0) {
        // Ни один файл не удалён, хотя план требовал: место под новый снимок не
        // освободилось. Пишем — каталог вырастет сверх keep; молчать нельзя,
        // иначе рост выглядел бы как «ротация работает».
        ESP_LOGE(TAG, "backup: rotation removed nothing of %d planned — dir will exceed keep=%d",
                 ndel, keep);
    }

    // P2: tmp+rename (atomic_write_snapshot) — обрыв питания портит только
    // tmp, боевое имя bk_<sess>_<seq>.bin либо целое, либо отсутствует;
    // обрезанный файл под боевым именем раньше навсегда занимал слот ротации.
    char path[80], tmp_path[88];
    snprintf(path, sizeof(path), "%s/" BACKUP_NAME_FMT, BACKUP_DIR, sess, seq);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (!atomic_write_snapshot(tmp_path, path, snap)) {
        ESP_LOGE(TAG, "backup: write %s failed (errno=%d)", path, errno);
        flash_quiet_writer_unlock();
        free(snap);
        return -3;
    }
    flash_quiet_writer_unlock();
    ESP_LOGI(TAG, "backup: saved %s (%" PRIu32 " counts, %" PRIu32 "s, keep=%d, rotated=%d)",
             path, snap->total_counts, snap->total_time_sec, keep, ndel);
    free(snap);
    return 0;
}

int spectrum_backup_load(const char *name, spectrum_data_t *out)
{
    backup_id_t id;
    if (!out || !backup_parse_name(name, &id)) return -1;   // защита от "../" и мусора
    char path[80];
    snprintf(path, sizeof(path), "%s/" BACKUP_NAME_FMT, BACKUP_DIR, id.sess, id.seq);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(out, 1, sizeof(*out), f);
    fclose(f);
    if (rd != sizeof(*out)) return -1;
    sanitize_loaded(out);      // те же гарантии, что и для ручных spec_NNNN.bin
    return 0;
}

uint32_t spectrum_backup_max_session(void)
{
    DIR *dir = opendir(BACKUP_DIR);
    if (!dir) return 0;
    uint32_t max_sess = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        backup_id_t id;
        if (!backup_parse_name(de->d_name, &id)) continue;
        if (id.sess > max_sess) max_sess = id.sess;
    }
    closedir(dir);
    return max_sess;
}

int spectrum_backup_delete(const char *name)
{
    backup_id_t id;
    if (!backup_parse_name(name, &id)) return -1;
    char path[80];
    snprintf(path, sizeof(path), "%s/" BACKUP_NAME_FMT, BACKUP_DIR, id.sess, id.seq);
    if (remove(path) != 0) return -1;
    ESP_LOGI(TAG, "backup: deleted %s", path);
    return 0;
}
// ─── конец issue #52 ──────────────────────────────────────────────────────────

int spectrum_load_from_flash(int index, spectrum_data_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", SPEC_DIR, index);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(out, 1, sizeof(*out), f);
    fclose(f);
    if (rd != sizeof(*out)) return -1;
    sanitize_loaded(out);
    return 0;
}

// #WF-1: флеш-запись калибровки вынесена из-под SPEC_LOCK и из CDC/httpd-тасков.
// Парсер -inf/-cal (CDC-таск) и spectrum_set_calibration (httpd) лишь взводят
// s_calib_dirty под SPEC_LOCK; фактическую запись на LittleFS (freeze кэша
// обоих ядер) делает main loop (10-с тик) ВНЕ лока — SPEC_LOCK не держится
// на время flash-операции и приём USB не останавливается.
// Вызывать БЕЗ SPEC_LOCK (берёт его сам для снапшота).
void spectrum_save_calibration(void)
{
    if (!s_calib_dirty) return;
    /* Rare dirty flag — still freeze LittleFS; only write in quiet if USB live. */
    if (usb_host_cdc_is_connected() && !flash_quiet_can_start_slice())
        return;
    calib_store_t st = {0};
    SPEC_LOCK();
    if (!s_spectrum.calib_valid) { s_calib_dirty = false; SPEC_UNLOCK(); return; }
    strncpy(st.serial, s_spectrum.serial_number, sizeof(st.serial) - 1);
    memcpy(st.calibration, s_spectrum.calibration, sizeof(st.calibration));
    st.calib_order = s_spectrum.calib_order;
    st.valid = 1;
    s_calib_dirty = false;
    SPEC_UNLOCK();
    if (!flash_quiet_writer_lock(pdMS_TO_TICKS(200))) {
        SPEC_LOCK(); s_calib_dirty = true; SPEC_UNLOCK();
        return;
    }
    FILE *f = fopen(CALIB_FILE, "wb");
    if (!f) {
        flash_quiet_writer_unlock();
        SPEC_LOCK(); s_calib_dirty = true; SPEC_UNLOCK();
        return;
    }
    size_t wr = fwrite(&st, sizeof(st), 1, f);
    if (fclose(f) != 0 || wr != 1) {
        flash_quiet_writer_unlock();
        ESP_LOGE(TAG, "Calibration write failed (wr=%zu)", wr);
        SPEC_LOCK(); s_calib_dirty = true; SPEC_UNLOCK();
        return;
    }
    flash_quiet_writer_unlock();
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
    s_calib_dirty = true;   // #WF-1: запись сделает main loop вне SPEC_LOCK
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

/* #FW-8 residual F1a: sliced LittleFS autosave across post-commit quiet windows. */
static spectrum_data_t *s_as_snap;
static FILE *s_as_fp;
static size_t s_as_off;
static size_t s_as_total;
static int s_as_slices;
static int64_t s_as_t0;
static int s_as_fail_streak;
static int64_t s_as_last_ok_us;
static volatile bool s_as_abort;

static void autosave_cleanup_failed(void)
{
    if (s_as_fp) { fclose(s_as_fp); s_as_fp = NULL; }
    unlink(AUTOSAVE_TMP_FILE);
    if (s_as_snap) { free(s_as_snap); s_as_snap = NULL; }
    s_as_off = s_as_total = 0;
    s_as_slices = 0;
}

void spectrum_autosave_note_fail(void)
{
    if (s_as_fail_streak < 1000000) s_as_fail_streak++;
}

void spectrum_autosave_note_ok(void)
{
    s_as_fail_streak = 0;
    s_as_last_ok_us = esp_timer_get_time();
}

int spectrum_autosave_fail_streak(void)
{
    return s_as_fail_streak;
}

int spectrum_autosave_age_sec(void)
{
    if (s_as_last_ok_us <= 0) return -1;
    int64_t age = (esp_timer_get_time() - s_as_last_ok_us) / 1000000;
    if (age < 0) return -1;
    if (age > 86400 * 365) return (int)(86400 * 365);
    return (int)age;
}

bool spectrum_autosave_in_progress(void)
{
    return s_as_abort || s_as_snap != NULL || s_as_fp != NULL;
}

void spectrum_autosave_abort(void)
{
    s_as_abort = true;
    if (s_as_snap || s_as_fp)
        ESP_LOGW(TAG, "autosave abort requested (consume on main)");
}

void spectrum_autosave_consume_abort(void)
{
    if (!s_as_abort) return;
    s_as_abort = false;
    if (s_as_snap || s_as_fp)
        ESP_LOGW(TAG, "autosave aborted mid-write (tmp discarded)");
    autosave_cleanup_failed();
    // #FW-58 (issue #24): check the result instead of logging unconditional success.
    // ENOENT is the expected/common case now that spectrum_reset() already unlinks
    // synchronously — not an error. Any other errno means current.bin is still on
    // flash and this pass will NOT retry (s_as_abort is already cleared above), so
    // it must be visible, not silently claimed as done.
    if (unlink(AUTOSAVE_FILE) == 0)
        ESP_LOGI(TAG, "autosave consume: current.bin unlinked");
    else if (errno != ENOENT)
        ESP_LOGW(TAG, "autosave consume: current.bin unlink failed (errno=%d)", errno);
}

void spectrum_autosave_yield(void)
{
    if (!s_as_snap) return;
    if (s_as_fp) {
        fflush(s_as_fp);
        fclose(s_as_fp);
        s_as_fp = NULL;
    }
    ESP_LOGI(TAG, "autosave yielded (off=%zu/%zu, tmp kept)", s_as_off, s_as_total);
}

bool spectrum_autosave_begin(void)
{
#if HIST_DROP_E1_NO_AUTOSAVE
    ESP_LOGI(TAG, "autosave skipped (HIST_DROP_E1_NO_AUTOSAVE)");
    return false;
#endif
    if (s_as_abort) {
        spectrum_autosave_consume_abort();
        return false;
    }
    /* Resume after yield: reopen tmp for append, keep snap/offset. */
    if (s_as_snap && s_as_fp) return true;
    if (s_as_snap && !s_as_fp) {
        if (usb_host_cdc_is_connected() && !flash_quiet_can_start_slice())
            return false;
        if (!http_io_gate_try_enter())
            return false;
        if (!flash_quiet_writer_lock(flash_quiet_writer_lock_ticks())) {
            http_io_gate_leave();
            return false;
        }
        FILE *f = fopen(AUTOSAVE_TMP_FILE, "ab");
        if (!f) {
            flash_quiet_writer_unlock();
            http_io_gate_leave();
            ESP_LOGE(TAG, "Autosave tmp reopen failed");
            return false;
        }
        flash_quiet_writer_unlock();
        http_io_gate_leave();
        s_as_fp = f;
        return true;
    }
    /* fopen/unlink of tmp can take 100–160 ms — only in post-commit quiet. */
    if (usb_host_cdc_is_connected() && !flash_quiet_can_start_slice())
        return false;
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return false;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return false; }
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();

    if (!http_io_gate_try_enter()) {
        free(snap);
        return false;
    }
    if (!flash_quiet_writer_lock(flash_quiet_writer_lock_ticks())) {
        http_io_gate_leave();
        free(snap);
        return false;
    }
    unlink(AUTOSAVE_TMP_FILE);
    int64_t t_open0 = esp_timer_get_time();
    FILE *f = fopen(AUTOSAVE_TMP_FILE, "wb");
    int64_t open_us = esp_timer_get_time() - t_open0;
    flash_quiet_writer_unlock();
    http_io_gate_leave();
    if (!f) {
        ESP_LOGE(TAG, "Autosave tmp open failed");
        free(snap);
        return false;
    }
#if HIST_DROP_I2_SPLIT_TIMING || HIST_DROP_DIAG
    ESP_LOGI(TAG, "autosave begin open_us=%lld bytes=%zu",
             (long long)open_us, sizeof(*snap));
#else
    (void)open_us;
#endif
    s_as_snap = snap;
    s_as_fp = f;
    s_as_off = 0;
    s_as_total = sizeof(*snap);
    s_as_slices = 0;
    s_as_t0 = esp_timer_get_time();
    return true;
}

void spectrum_autosave_pump(void)
{
    if (s_as_abort) {
        spectrum_autosave_consume_abort();
        return;
    }
    if (!s_as_fp || !s_as_snap) return;

    /* Dedicated quiet window for close/rename once payload is fully written. */
    if (s_as_off >= s_as_total) {
        if (!flash_quiet_can_start_slice()) return;
        if (!http_io_gate_try_enter()) return;
        if (!flash_quiet_writer_lock(pdMS_TO_TICKS(50))) {
            http_io_gate_leave();
            return;
        }
        fflush(s_as_fp);
        if (fclose(s_as_fp) != 0) {
            s_as_fp = NULL;
            flash_quiet_writer_unlock();
            http_io_gate_leave();
            ESP_LOGE(TAG, "autosave fclose failed");
            autosave_cleanup_failed();
            return;
        }
        s_as_fp = NULL;
        if (rename(AUTOSAVE_TMP_FILE, AUTOSAVE_FILE) != 0) {
            flash_quiet_writer_unlock();
            http_io_gate_leave();
            ESP_LOGE(TAG, "autosave rename failed");
            autosave_cleanup_failed();
            return;
        }
        flash_quiet_writer_unlock();
        http_io_gate_leave();
        int64_t total_us = esp_timer_get_time() - s_as_t0;
        ESP_LOGI(TAG, "autosave complete slices=%d total_us=%lld",
                 s_as_slices, (long long)total_us);
        free(s_as_snap);
        s_as_snap = NULL;
        s_as_off = s_as_total = 0;
        s_as_slices = 0;
        spectrum_autosave_note_ok();
        return;
    }

    /* At most a few slices per quiet window; require erase-cliff headroom. */
    while (s_as_off < s_as_total) {
        if (s_as_abort) {
            spectrum_autosave_consume_abort();
            return;
        }
        if (!flash_quiet_can_start_slice()) break;
        if (!http_io_gate_try_enter()) break;
        if (!flash_quiet_writer_lock(pdMS_TO_TICKS(50))) {
            http_io_gate_leave();
            break;
        }

        size_t chunk = FLASH_QUIET_SLICE_BYTES;
        if (chunk > s_as_total - s_as_off) chunk = s_as_total - s_as_off;

        int64_t t0 = esp_timer_get_time();
        size_t n = fwrite((const uint8_t *)s_as_snap + s_as_off, 1, chunk, s_as_fp);
        int64_t dt = esp_timer_get_time() - t0;
        flash_quiet_writer_unlock();
        http_io_gate_leave();
        if (n != chunk) {
            ESP_LOGE(TAG, "autosave slice fwrite failed off=%zu n=%zu", s_as_off, n);
            autosave_cleanup_failed();
            return;
        }
        s_as_off += n;
        s_as_slices++;
#if HIST_DROP_DIAG
        ESP_LOGI(TAG, "autosave slice n=%zu off=%zu/%zu dt_us=%lld rem_us=%lld",
                 n, s_as_off, s_as_total, (long long)dt,
                 (long long)flash_quiet_remaining_us());
#else
        (void)dt;
#endif
        /* One slice that burned the guard → wait for next commit. */
        if (!flash_quiet_can_start_slice()) break;
    }
}

void spectrum_autosave(void)
{
#if HIST_DROP_E1_NO_AUTOSAVE
    ESP_LOGI(TAG, "autosave skipped (HIST_DROP_E1_NO_AUTOSAVE)");
    return;
#endif
    if (s_as_abort)
        spectrum_autosave_consume_abort();
    /* One-shot full write — safe when USB analyzer is silent/disconnected
     * (no 1 Hz burst). Used by offline path and as I2 timing baseline. */
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return; }
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();

    if (!http_io_gate_try_enter()) {
        free(snap);
        spectrum_autosave_note_fail();
        return;
    }
    // P2 (второе независимое ревью 9453113): тот же atomic_write_snapshot,
    // что и spectrum_backup_save() — не копия. Обрыв питания портит только
    // tmp; current.bin остаётся прежним и годным для восстановления.
    // #FW-8 I2 split-timing диагностика (open/write/close us, HIST_DROP_DIAG,
    // default OFF) вместе с этим убрана — измеряла именно инлайновый путь,
    // который здесь больше не существует; при необходимости меряется заново
    // снаружи atomic_write_snapshot() (esp_timer_get_time() вокруг вызова).
    if (!atomic_write_snapshot(AUTOSAVE_TMP_FILE, AUTOSAVE_FILE, snap)) {
        ESP_LOGE(TAG, "Autosave write failed (errno=%d)", errno);
        http_io_gate_leave();
    } else {
        http_io_gate_leave();
        spectrum_autosave_note_ok();
    }
    free(snap);
}

// Перебор уже отсканированного списка have[0..n-1] от самого нового к самому
// старому (backup_newest_index); побитый кандидат пропускается молча.
static bool restore_from_latest_backup_scanned(backup_id_t *have, int n, spectrum_data_t *out)
{
    while (n > 0) {
        int idx = backup_newest_index(have, n);
        if (idx < 0) break;
        char name[40];
        snprintf(name, sizeof(name), BACKUP_NAME_FMT, have[idx].sess, have[idx].seq);
        if (spectrum_backup_load(name, out) == 0 && out->valid) {
            ESP_LOGW(TAG, "Restored autosave from backup %s: %" PRIu32 " counts, %" PRIu32 "s",
                     name, out->total_counts, out->total_time_sec);
            return true;
        }
        ESP_LOGW(TAG, "backup restore: %s unreadable, trying next", name);
        have[idx] = have[n - 1];
        n--;
    }
    return false;
}

// Читает файл целиком в *out; true только если размер точен и valid==true —
// тот же критерий, что и раньше для current.bin, теперь общий для main/tmp.
static bool load_valid_snapshot(const char *path, spectrum_data_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t rd = fread(out, 1, sizeof(*out), f);
    fclose(f);
    return rd == sizeof(*out) && out->valid;
}

// P0 review fix: один буфер (heap, у вызывающего), не три копии по источникам.
// Лог для BACKUP уже дан restore_from_latest_backup_scanned. Вызывать под SPEC_LOCK.
static void restore_apply(restore_source_t src, spectrum_data_t *data)
{
    switch (src) {
    case RESTORE_SRC_MAIN:
        s_spectrum = *data;
        ESP_LOGI(TAG, "Restored autosave from current.bin: %" PRIu32 " counts, %" PRIu32 "s",
                 s_spectrum.total_counts, s_spectrum.total_time_sec);
        return;
    case RESTORE_SRC_TMP:
        s_spectrum = *data;
        ESP_LOGW(TAG, "Restored autosave from tmp (power loss mid-write): %" PRIu32 " counts, %" PRIu32 "s",
                 s_spectrum.total_counts, s_spectrum.total_time_sec);
        return;
    case RESTORE_SRC_BACKUP:
        s_spectrum = *data;
        return;
    default:
        memset(&s_spectrum, 0, sizeof(s_spectrum));
        return;
    }
}

// Хвост restore_resolve(): main/tmp уже проверены — если оба негодны, ровно
// ОДИН обход BACKUP_DIR (P3) и попытка прочитать самый новый годный снимок.
static restore_source_t restore_resolve_backup(spectrum_data_t *buf, bool main_valid, bool tmp_valid)
{
    backup_id_t have[BACKUP_KEEP_MAX * 2];
    int n = 0;
    if (!main_valid && !tmp_valid) {
        n = backup_scan(have, (int)(sizeof(have) / sizeof(have[0])));
        if (n > (int)(sizeof(have) / sizeof(have[0])))
            n = (int)(sizeof(have) / sizeof(have[0]));
    }
    restore_source_t src = restore_pick_source(main_valid, tmp_valid, n > 0);
    if (src == RESTORE_SRC_BACKUP && !restore_from_latest_backup_scanned(have, n, buf)) {
        ESP_LOGE(TAG, "Autosave restore: main+tmp invalid, backup listed but unreadable");
        src = RESTORE_SRC_NONE;
    }
    return src;
}

// Заполняет *buf данными выбранного источника (или не трогает его для NONE) и
// возвращает какой источник выбран; P3: один обход BACKUP_DIR (не два, как в
// f091b01: там max_session() и restore_from_latest_backup() сканировали дважды).
static restore_source_t restore_resolve(spectrum_data_t *buf)
{
    bool main_valid = load_valid_snapshot(AUTOSAVE_FILE, buf);
    bool tmp_valid = !main_valid && load_valid_snapshot(AUTOSAVE_TMP_FILE, buf);
    return restore_resolve_backup(buf, main_valid, tmp_valid);
}

void spectrum_restore_autosave(void)
{
    // P0 (независимое ревью f091b01): раньше 3×spectrum_data_t (~32.8 КБ
    // каждый, bins[8192]) лежали на стеке app_main разом — ~98 КБ против
    // CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584. Теперь один буфер в куче,
    // источники проверяются последовательно, друг друга не переживая.
    spectrum_data_t *buf = malloc(sizeof(*buf));
    if (!buf) {
        ESP_LOGE(TAG, "Autosave restore: malloc(%u) failed, starting empty",
                 (unsigned)sizeof(*buf));
        SPEC_LOCK();
        memset(&s_spectrum, 0, sizeof(s_spectrum));
        SPEC_UNLOCK();
        return;
    }
    restore_source_t src = restore_resolve(buf);

    SPEC_LOCK();
    restore_apply(src, buf);
    SPEC_UNLOCK();
    free(buf);
}

// AWF-3: сохранить базу атомарно (tmp+rename, тот же atomic_write_snapshot,
// что автосейв и бэкапы). Вызывать ВНЕ SPEC_LOCK — сам снимает снимок под ним.
static void spectrum_base_snapshot(spectrum_data_t *buf)
{
    memset(buf, 0, sizeof(*buf));
    SPEC_LOCK();
    memcpy(buf->bins, s_base_bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
    buf->total_time_sec = s_base_time_sec;
    buf->total_counts = s_base_total_counts;
    SPEC_UNLOCK();
    buf->valid = true;
}

// F4 (итоговое ревью 25.09): молча пропускал запись базы, если занят
// http_io_gate — RAM (B1) и flash (B0) расходились до следующего fold.
// Теперь: флаг + ESP_LOGW + повтор на ближайшем тике main.c.
// N3 (ревью-2): main держит gate и пишет B1, CDC после НОВОГО fold видит gate
// занят и ставит pending=true (B2 не попал в этот заход) — затем main
// заканчивает и раньше слепо писал pending=!ok=false, СТИРАЯ true от CDC.
// Фикс: pending СНИМАЕТСЯ сразу при входе в критическую секцию (сразу после
// захвата gate, ДО самой записи на flash — окно гонки схлопывается с «вся
// запись» до одной строки под SPEC_LOCK), а НЕ после записи; завершение
// пишет флаг ТОЛЬКО при неудаче (SET, никогда unconditional CLEAR) — успешная
// запись никогда не стирает true, выставленный конкурентом. Рейт-лимит 10с
// на повтор (доп. защёлка поверх main.c 10с-тика).
static volatile bool s_base_save_pending = false;
static uint32_t s_base_save_last_attempt_ms = 0;
#define BASE_SAVE_RETRY_MIN_MS (10u * 1000u)

static void spectrum_base_save(void)
{
    if (!s_base_bins) return;
    spectrum_data_t *buf = malloc(sizeof(*buf));
    if (!buf) { ESP_LOGE(TAG, "base save: malloc failed"); return; }
    spectrum_base_snapshot(buf);
    if (!http_io_gate_try_enter()) {
        SPEC_LOCK(); s_base_save_pending = true; SPEC_UNLOCK();
        ESP_LOGW(TAG, "base save: http_io_gate busy, deferred (retry on next tick)");
        free(buf);
        return;
    }
    // Claim ДО записи (не после) — сужает окно гонки с конкурентным fold+save.
    SPEC_LOCK(); s_base_save_pending = false; SPEC_UNLOCK();
    bool ok = atomic_write_snapshot(BASE_TMP_FILE, BASE_FILE, buf);
    if (!ok) {
        ESP_LOGE(TAG, "base save: write failed (errno=%d)", errno);
        SPEC_LOCK(); s_base_save_pending = true; SPEC_UNLOCK();
    }
    http_io_gate_leave();
    free(buf);
}

// Вызывать раз в main-тик (main.c) — no-op, если ничего не отложено, либо
// последняя попытка была < BASE_SAVE_RETRY_MIN_MS назад.
void spectrum_base_save_retry_tick(void)
{
    if (!s_base_save_pending) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_base_save_last_attempt_ms < BASE_SAVE_RETRY_MIN_MS) return;
    s_base_save_last_attempt_ms = now_ms;
    spectrum_base_save();
}

static void spectrum_base_apply_loaded(const spectrum_data_t *buf)
{
    SPEC_LOCK();
    // F6 (итоговое ревью 25.09): shown (D) уже восстановлен autosave'ом РАНЬШЕ
    // по порядку (main.c:69-70) — база обязана быть <= shown, иначе на первом
    // же коммите shown_counts-base_counts уйдёт в отрицательное (uint32 wrap
    // в spectrum_base_reset_detected_by_counts) и ложно свернёт МЕНЬШИЙ D
    // поверх настоящего. Такая база сама негодна (прибор сбросился, D
    // восстановлен меньшим через bk_*.bin, а на flash осталась база от
    // прошлой, более долгой сессии) — обнуляем её, не жертвуем D.
    if (buf->total_counts > s_spectrum.total_counts) {
        ESP_LOGW(TAG, "Restored base (%" PRIu32 ") > shown (%" PRIu32 ") -- "
                 "base invalid, zeroing", buf->total_counts, s_spectrum.total_counts);
        SPEC_UNLOCK();
        return;
    }
    memcpy(s_base_bins, buf->bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
    s_base_time_sec = buf->total_time_sec;
    s_base_total_counts = buf->total_counts;
    SPEC_UNLOCK();
    ESP_LOGI(TAG, "Restored base: %" PRIu32 " counts, %" PRIu32 "s",
             s_base_total_counts, s_base_time_sec);
}

// AWF-3 (#3): база из base.bin. Файла нет (обновление с 1.2.23) — база
// нулевая; base.bin битый, а D валиден — тоже нулевая (не жертвуем D ради
// негодной базы, тот же принцип, что restore_pick_source в AWF-1).
void spectrum_restore_base(void)
{
    if (!s_base_bins) return;
    spectrum_data_t *buf = malloc(sizeof(*buf));
    if (!buf) { ESP_LOGE(TAG, "base restore: malloc failed"); return; }
    if (load_valid_snapshot(BASE_FILE, buf))
        spectrum_base_apply_loaded(buf);
    else
        ESP_LOGW(TAG, "base restore: base.bin missing/invalid — zero base");
    free(buf);
}

// #7: наблюдаемость для /api/status.
void spectrum_get_base_info(uint32_t *base_time, uint32_t *base_counts, uint32_t *dev_resets)
{
    SPEC_LOCK();
    if (base_time)   *base_time   = s_base_time_sec;
    if (base_counts) *base_counts = s_base_total_counts;
    if (dev_resets)  *dev_resets  = s_dev_resets;
    SPEC_UNLOCK();
}

int spectrum_delete_from_flash(int index)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", SPEC_DIR, index);
    if (remove(path) != 0) return -1;
    ESP_LOGI(TAG, "Deleted %s", path);
    return 0;
}
// #FW-53: см. декларацию в atomspectra.h. Читается из HTTP-контекста, поэтому
// под тем же SPEC_LOCK, что и остальные публикуемые поля.
void spectrum_get_sweep_stats(uint32_t *commits, uint32_t *drops)
{
    SPEC_LOCK();
    if (commits) *commits = s_hist_commits;
    if (drops)   *drops   = s_hist_drops;
    SPEC_UNLOCK();
}

// #PERF-4 (P-014): снимок БЕЗ bins[8192]. spectrum_get_snapshot() копирует 32 КБ
// под SPEC_LOCK — тем же локом, который берёт spectrum_process_histogram_chunk()
// на каждый чанк от прибора. Потребителям, которым нужны только метаданные
// (серийник, калибровка, cps, температура), это стоило 8,5 % потерянных свипов
// на /api/device. Копируем хвост структуры после bins — ~200 Б вместо 32 КБ.
// offsetof, а не перечисление полей: новое поле структуры попадёт сюда само.
bool spectrum_get_meta(spectrum_data_t *out)
{
    const size_t off = offsetof(spectrum_data_t, total_counts);
    SPEC_LOCK();
    memcpy((uint8_t *)out + off, (const uint8_t *)&s_spectrum + off,
           sizeof(spectrum_data_t) - off);
    SPEC_UNLOCK();
    return out->valid;
}
