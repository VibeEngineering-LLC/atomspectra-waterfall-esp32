#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Водопад (спектрограмма): каждая строка — дельта накопительного спектра за
// интервал, uint16 на канал. Источник спектра — модуль spectrum (cmd 0x01).

#define WF_CHANNELS           8192            // = SPECTRUM_CHANNELS
#define WF_ROW_BYTES          (WF_CHANNELS * 2)
#define WF_RING_ROWS_DEFAULT  256             // окно в PSRAM (256 * 16 КБ = 4 МБ)
#define WF_INTERVAL_DEFAULT   5               // сек между строками
#define WF_INTERVAL_MIN       5               // нижняя граница (UI: 5–60 с)
#define WF_INTERVAL_MAX       60              // верхняя граница (UI: 5–60 с)

typedef struct {
    bool     recording;
    bool     persist;        // писать строки во флэш
    bool     flash_full;     // место кончилось — persist остановлен (кольцо/WS живут)
    bool     ready;          // PSRAM выделена
    uint32_t interval_sec;
    uint32_t ring_capacity;  // ёмкость кольца, строк
    uint32_t ring_count;     // валидных строк в кольце
    uint32_t total_rows;     // строк записано с момента start (монотонно)
    uint32_t flash_rows;     // строк во флэш-файле
    time_t   started_at;
} wf_status_t;

// Колбэк рассылки новой строки (web_server регистрирует для WS-броадкаста).
typedef void (*wf_row_cb_t)(const uint16_t *row, size_t bytes, uint32_t total_index);

void   spectrogram_init(void);
void   spectrogram_get_status(wf_status_t *out);
int    spectrogram_start(void);
int    spectrogram_stop(void);
int    spectrogram_clear(void);
void   spectrogram_set_interval(uint32_t sec);
void   spectrogram_set_persist(bool on);
void   spectrogram_set_row_cb(wf_row_cb_t cb);

// Копирует до max_rows новейших строк кольца в dst (row-major uint16,
// хронологически — старейшая из окна первой). Возвращает число строк;
// *first_total_index = total-индекс первой возвращённой строки.
size_t spectrogram_copy_window(uint16_t *dst, size_t max_rows, uint32_t *first_total_index);

// Колбэк отдачи одной строки окна (возврат false прерывает стрим).
typedef bool (*wf_emit_cb_t)(void *ctx, const uint16_t *row, size_t bytes);

// Потоковая отдача окна кольца БЕЗ большого буфера: каждая строка копируется
// под локом в bounce (размер WF_ROW_BYTES), затем emit() вызывается ВНЕ лока
// (сетевой I/O не держит мьютекс рекордера). Отдаёт все ring_count строк
// (хронологически), не упираясь в PSRAM. Возвращает число отданных строк;
// *first_total_index = total-индекс первой строки (выставляется до первого emit).
size_t spectrogram_stream_window(uint16_t *bounce, size_t max_rows,
                                 uint32_t *first_total_index,
                                 wf_emit_cb_t emit, void *ctx);

// Путь к флэш-файлу payload (для стриминга в /api/waterfall/export).
const char *spectrogram_data_path(void);