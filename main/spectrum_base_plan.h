// AWF-3: слияние «базы» (спектр до последнего сброса анализатора) с текущей
// накопительной гистограммой самого анализатора. Свободен от ESP-IDF.
#pragma once

#include <stdint.h>
#include <stdbool.h>

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

// Показываемое время/счётчик = база + прибор (один оператор — total и bins[]).
static inline uint32_t spectrum_base_merge(uint32_t base, uint32_t dev)
{
    return base + dev;
}
