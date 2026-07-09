// #MON-1 (#FW-40): мониторинг CPS на плате — кольцо базовых 1с-сэмплов
// (дельты по коммитам свипов). Браузер больше НЕ копит историю сам (фоновая
// вкладка троттлит таймер до ~60 с → рваные Δt, закрытая — дыра в данных),
// а только дорисовывает серию через GET /api/monitor/series?since=<seq>.
#pragma once

#include <stdint.h>
#include <stddef.h>

// Базовый сэмпл серии: дельта одного (или нескольких слитых) коммитов свипа.
// 10 Б в упаковке — 21600 шт ≈ 211 КБ PSRAM на 6 ч при 1 Гц.
typedef struct __attribute__((packed)) {
    uint32_t end_sec;   // total_time_sec прибора на момент коммита (наборное время)
    uint32_t counts;    // прирост total_counts за dur
    uint16_t dur;       // прирост total_time_sec, с (обычно 1; больше при пропусках свипов)
} monitor_sample_t;

// Поднять кольцо + задачу-подписчика на коммиты свипов. Вызывать из app_main()
// ПОСЛЕ spectrum_init() (spectrum_get_totals ходит под SPEC_LOCK).
void monitor_init(void);

// Снимок хвоста серии под внутренним локом (атомарно: сэмплы + курсоры + epoch
// одной точки времени). Копирует в out до max сэмплов с seq >= since+1
// (клампится к старейшему в кольце). Возвращает число скопированных.
// *out_first_seq — seq сэмпла out[0] (= *out_next_seq, если отдать нечего);
// *out_next_seq  — seq будущего сэмпла (последний записанный + 1);
// *out_epoch     — эпоха серии (меняется при откате счётчиков прибора и
//                  рандомизирована на старте платы — перезапуск виден клиенту).
size_t monitor_copy_since(uint32_t since, monitor_sample_t *out, size_t max,
                          uint32_t *out_first_seq, uint32_t *out_next_seq,
                          uint32_t *out_epoch);
