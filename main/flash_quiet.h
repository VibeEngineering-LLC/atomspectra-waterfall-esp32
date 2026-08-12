#pragma once
// #FW-8 residual F1: shared quiet-window budget after a histogram commit.
// Write LittleFS only while remaining quiet budget > guard (USB between 1 Hz bursts).
// Offline (no USB analyzer) → budget is treated as full so writers always complete.
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "flash_quiet_math.h"

/** Bytes to prefer per slice (4 KiB ≈ one NOR erase page class). */
#ifndef FLASH_QUIET_SLICE_BYTES
#define FLASH_QUIET_SLICE_BYTES 4096
#endif

/** Writer-lock wait: must exceed documented erase/hold cliffs (150–340 ms). */
#ifndef FLASH_QUIET_WRITER_LOCK_MS
#define FLASH_QUIET_WRITER_LOCK_MS 500
#endif

void flash_quiet_init(void);

/** Call on every successful full-sweep hist commit (end of USB burst). */
void flash_quiet_note_commit(void);

/** Remaining quiet-budget microseconds since last hist commit; 0 if unknown/exhausted. */
int64_t flash_quiet_remaining_us(void);

/** True if at least min_us of quiet budget remains. */
bool flash_quiet_can_write(int64_t min_us);

/** True if a new slice may start (remaining >= FLASH_QUIET_SLICE_GUARD_US). */
bool flash_quiet_can_start_slice(void);

/**
 * Serialize LittleFS writers (autosave vs WF). Hold only around flash I/O,
 * never across commit waits.
 */
bool flash_quiet_writer_lock(TickType_t wait_ticks);
void flash_quiet_writer_unlock(void);

static inline int flash_quiet_budget_ms(void) { return FLASH_QUIET_BUDGET_MS; }
static inline int64_t flash_quiet_slice_guard_us(void)
{
    return (int64_t)FLASH_QUIET_SLICE_GUARD_US;
}
static inline TickType_t flash_quiet_writer_lock_ticks(void)
{
    return pdMS_TO_TICKS(FLASH_QUIET_WRITER_LOCK_MS);
}
