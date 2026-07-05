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
#define WF_INTERVAL_MIN       5               // нижняя граница (UI: 5 с)
// Верхняя граница 3600 с (1 ч): автономная многосуточная запись на Flash.
// Ёмкость storage ≈ 763 строки → при 10-мин интервале ≈ 5.3 сут, при 1-ч ≈ 31 сут.
#define WF_INTERVAL_MAX       3600            // верхняя граница (UI: до 60 мин)

// #REC-11-A1: запись сегментами /storage/wf/seg_NNNNN.aswf (каждый — валидный .aswf).
// Сегмент = единица отправки (фаза A2/B), единица кольца keep-last и независимый файл
// для склейки в браузере. Открытый сегмент финализируется по достижении MAX_ROWS
// (~1 МБ) ИЛИ MAX_AGE (чтобы при больших интервалах не висел открытым часами).
#define WF_SEG_MAX_ROWS       64              // 64 * 16 КБ = 1 МБ payload на сегмент
#define WF_SEG_MAX_AGE_SEC    600             // финализировать открытый сегмент не реже 10 мин
#define WF_HDR_RESERVE        4096            // .aswf JSON-заголовок (добивается пробелами)
#define WF_SEG_HEADER         (8 + WF_HDR_RESERVE)  // offset payload в сегменте (= 4104)

// #FW-5: формат сегмента v2 — к каждой строке в файле (16384 Б спектра uint16 LE)
// приписаны 2 Б реальной длительности среза (uint16 LE, секунды = дельта живого
// времени прибора total_time_sec). Запись фикс. размера WF_ROW_STRIDE → reconcile
// по размеру файла остаётся тривиальным. Старые сегменты v1 (без длительностей,
// stride = WF_ROW_BYTES) распознаются по отсутствию "row_stride" в JSON-шапке.
// Шапка v2 самоописываема: "version":2, "row_stride":16386,
// "row_time":{"dtype":"uint16","unit":"sec","offset":16384}.
#define WF_DUR_BYTES          2                              // uint16 LE, секунды
#define WF_ROW_STRIDE         (WF_ROW_BYTES + WF_DUR_BYTES)  // = 16386, запись строки в сегмент

typedef struct {
    bool     recording;
    bool     persist;        // писать строки во флэш
    bool     flash_full;     // #REC-11-A1: кольцо активно (старые сегменты затираются)
    bool     ready;          // PSRAM выделена
    uint32_t interval_sec;
    uint32_t ring_capacity;  // ёмкость кольца, строк
    uint32_t ring_count;     // валидных строк в кольце
    uint32_t total_rows;     // строк записано с момента start (монотонно)
    uint32_t flash_rows;     // строк во флэш записано за сессию (монотонно)
    uint32_t seg_count;      // #REC-11-A1: завершённых сегментов на Flash сейчас
    uint32_t seg_dropped;    // #REC-11-A1: сегментов удалено кольцом с момента boot
    time_t   started_at;
    // #FW-21: длительность ТЕКУЩЕЙ сессии записи по монотонным часам (esp_timer),
    // НЕ зависит от NTP/wall-time. Источник истины для «Время записи» в UI —
    // устойчив к невалидному started_at, ребуту (restore заводит заново) и F5
    // (клиент берёт готовое число с платы, не считает сам). 0 когда не пишем.
    uint32_t elapsed_sec;
} wf_status_t;

// Колбэк рассылки новой строки (web_server регистрирует для WS-броадкаста).
typedef void (*wf_row_cb_t)(const uint16_t *row, size_t bytes, uint32_t total_index);

void   spectrogram_init(void);
// #REC-11-A1: автономное возобновление после ребута/сбоя питания.
// Сначала сверяет каталог сегментов (/storage/wf): пропатчивает счётчики у
// недописанных файлов, удаляет пустые, восстанавливает s_seg_next/seg_count.
// Если запись была активна с persist — продолжает писать в НОВЫЙ сегмент
// (без mid-segment append — каждый файл остаётся валидным .aswf).
// Вызывать на boot ПОСЛЕ spectrogram_init() (см. main.c).
void   spectrogram_restore(void);
// true, если водопад сейчас в режиме записи (для USB-реконнекта: переслать -sta).
bool   spectrogram_is_recording(void);
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

// #FW-5: копирует реальные длительности (сек) до max_rows новейших строк кольца,
// выровненные со spectrogram_copy_window/stream_window (старейшая из окна первой).
// Возвращает число элементов. Элемент 0 = device-время не продвинулось за тик
// (потребитель N42 подставляет номинальный interval_sec при делении на длительность).
size_t spectrogram_copy_window_durations(uint16_t *dst, size_t max_rows);

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

// Каталог сегментов на Flash (/storage/wf). Файлы seg_NNNNN.aswf — каждый
// самостоятельный валидный .aswf. Используется web-слоем для листинга/отдачи.
const char *spectrogram_seg_dir(void);

// Индекс СЕЙЧАС открытого (недописанного) сегмента или 0xFFFFFFFF, если открытого
// нет. Web-слой помечает его finalized:false в /segments (его шапка ещё не
// пропатчена — saved_rows=0; забирать его в браузер не нужно до finalize).
uint32_t spectrogram_seg_open_index(void);

// #REC-11-A2: координация автономной выгрузки с кольцом keep-last.
// claim — застолбить (pin) старейший завершённый сегмент: кольцо его не удалит,
//   пока он выгружается. Возвращает true и заполняет idx/name/path/size, либо
//   false (уже занято другой выгрузкой, либо нечего слать).
// done — вызвать после подтверждённого 2xx: удалить файл с Flash и снять пин.
// release — снять пин без удаления (выгрузка не удалась, файл оставить для повтора).
bool spectrogram_offload_claim(uint32_t *idx_out, char *name_out, size_t name_cap,
                               char *path_out, size_t path_cap, long *size_out);
void spectrogram_offload_done(uint32_t idx);
void spectrogram_offload_release(uint32_t idx);

// #REC-11 pull: удалить завершённый сегмент по индексу (PC подтвердил приём через
// POST /api/waterfall/segment/delete). Не трогает открытый/pinned сегмент. true=удалён.
bool spectrogram_seg_delete(uint32_t idx);