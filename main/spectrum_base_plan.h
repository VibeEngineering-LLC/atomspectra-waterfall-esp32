// AWF-3: слияние «базы» (спектр до последнего сброса анализатора) с текущей
// накопительной гистограммой самого анализатора. Свободен от ESP-IDF.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Допуск на дрожание времени STAT — тот же порог, что и раньше использовался
// для «протухшего STAT» (#FW-12: «откат >=5с — рестарт прибора»).
#define SPECTRUM_BASE_RESET_TOLERANCE_S 5u

// true, если время НОВОГО STAT прибора явно МЕНЬШЕ времени, которое прибор
// уже должен был набрать с последнего сохранённого состояния (shown-base),
// с учётом допуска. shown_time_sec >= base_time_sec (инвариант shown=base+dev).
static inline bool spectrum_base_reset_detected(uint32_t dev_time_now,
                                                 uint32_t base_time_sec,
                                                 uint32_t shown_time_sec)
{
    uint32_t dev_elapsed_since_base = shown_time_sec - base_time_sec;
    if (dev_time_now + SPECTRUM_BASE_RESET_TOLERANCE_S >= dev_elapsed_since_base)
        return false;
    return true;
}

// P1-b: сброс ещё и по СЧЁТУ, независимо от STAT — гистограмма накопительная
// (монотонна внутри сессии прибора), свежий свип меньше, чем прибор уже
// должен был набрать с последнего fold (shown-база), — тоже верный признак
// рестарта, даже если STAT на этом коммите не пришёл вообще. Допуск 0: в
// отличие от времени (STAT дрожит по протоколу), сумма гистограммы — точное
// число, легитимной просадки без сброса не бывает.
#define SPECTRUM_BASE_RESET_TOLERANCE_COUNTS 0u

static inline bool spectrum_base_reset_detected_by_counts(uint32_t dev_total_now,
                                                           uint32_t base_counts,
                                                           uint32_t shown_total_counts)
{
    uint32_t dev_expected_since_base = shown_total_counts - base_counts;
    if (dev_total_now + SPECTRUM_BASE_RESET_TOLERANCE_COUNTS >= dev_expected_since_base)
        return false;
    return true;
}

// P2 (ревью 97b71d0..6426bb8): показываемое время/счётчик = база + прибор,
// НАСЫЩАЮЩЕЕ сложение (один оператор — total, bins[], время). База копится
// через рестарты платы годами — без насыщения переполнение uint32
// (~4.3×10^9) обернулось бы отрицательным/крошечным показанным значением
// молча. sum<base — верный признак переполнения (сумма меньше слагаемого).
static inline uint32_t spectrum_base_merge(uint32_t base, uint32_t dev)
{
    uint32_t sum = base + dev;
    if (sum < base) return UINT32_MAX;
    return sum;
}

// P1-b orchestration (живой тест 25.09: первый коммит после рестарта прибора
// слил dev в bins ДО проверки сброса, если STAT ещё не пришёл свежим на этом
// коммите, — база получила уже склеенное 0+крошечный_dev вместо старого
// показанного спектра). Порядок зафиксирован ЭТОЙ функцией, не комментарием:
// единственное место, которое трогает base_bins/shown_bins.
typedef struct {
    uint32_t *base_bins;      // [n], база (мутируется при сворачивании)
    uint32_t  base_time;
    uint32_t  base_counts;
    uint32_t *shown_bins;     // [n], ПОКАЗЫВАЕМЫЙ спектр (мутируется = base+dev)
    uint32_t  shown_time;     // только читается; время пишет caller (#FW-12 отдельно)
    uint32_t  shown_counts;
} spectrum_base_state_t;

// F3 (итоговое ревью 25.09): свёртка ТОЛЬКО по счёту. Раньше просадка ОДНОГО
// времени (при согласованном счёте) тоже триггерила fold — a base:=shown
// (в shown УЖЕ есть dev), новый shown=base+dev=старая_база+2·dev: удвоение.
// Просадка STAT при согласованном счёте — пересинхронизация времени (было
// до ветки: «принимаем абсолют»), её отдельно делает caller (spectrum.c
// commit_apply_time_stat_fresh_locked), fold она больше не триггерит.
// stat_fresh/dev_time_now остаются в сигнатуре для caller'а (не в решении).
static inline bool spectrum_base_commit(spectrum_base_state_t *st, const uint32_t *dev_bins,
                                        uint32_t dev_total, size_t n,
                                        bool stat_fresh, uint32_t dev_time_now)
{
    (void)stat_fresh; (void)dev_time_now;
    bool reset = spectrum_base_reset_detected_by_counts(dev_total, st->base_counts, st->shown_counts);
    if (reset) {
        for (size_t i = 0; i < n; i++) st->base_bins[i] = st->shown_bins[i];
        st->base_time = st->shown_time;
        st->base_counts = st->shown_counts;
    }
    for (size_t i = 0; i < n; i++)
        st->shown_bins[i] = spectrum_base_merge(st->base_bins[i], dev_bins[i]);
    st->shown_counts = spectrum_base_merge(st->base_counts, dev_total);
    return reset;
}
