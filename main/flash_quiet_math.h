#pragma once
// Pure quiet-budget arithmetic (host-testable). Used by flash_quiet.c.
#include <stdint.h>
#include <stdbool.h>

#ifndef FLASH_QUIET_BUDGET_MS
#define FLASH_QUIET_BUDGET_MS 300
#endif

#ifndef FLASH_QUIET_SLICE_GUARD_US
#define FLASH_QUIET_SLICE_GUARD_US 180000
#endif

/**
 * Remaining quiet budget microseconds.
 * Offline / no USB analyzer → full budget (writes must complete; no 1 Hz burst).
 * commit_us <= 0 → exhausted while USB live (no commit yet / unknown).
 */
static inline int64_t flash_quiet_calc_remaining_us(int64_t commit_us, int64_t now_us,
                                                    int budget_ms, bool usb_connected)
{
    if (!usb_connected)
        return (int64_t)budget_ms * 1000;
    if (commit_us <= 0)
        return 0;
    int64_t elapsed = now_us - commit_us;
    if (elapsed < 0)
        return 0; /* torn 64-bit read or clock skew — refuse write this tick */
    int64_t budget_us = (int64_t)budget_ms * 1000;
    if (elapsed >= budget_us)
        return 0;
    return budget_us - elapsed;
}

static inline bool flash_quiet_calc_can_write(int64_t commit_us, int64_t now_us,
                                             int budget_ms, bool usb_connected,
                                             int64_t min_us)
{
    return flash_quiet_calc_remaining_us(commit_us, now_us, budget_ms, usb_connected) >= min_us;
}

static inline bool flash_quiet_calc_can_start_slice(int64_t commit_us, int64_t now_us,
                                                    int budget_ms, bool usb_connected)
{
    return flash_quiet_calc_can_write(commit_us, now_us, budget_ms, usb_connected,
                                      (int64_t)FLASH_QUIET_SLICE_GUARD_US);
}
