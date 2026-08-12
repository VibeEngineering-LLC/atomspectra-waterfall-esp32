#include "flash_quiet.h"
#include "esp_timer.h"

static volatile int64_t s_commit_us;
static SemaphoreHandle_t s_writer;

void flash_quiet_init(void)
{
    if (!s_writer)
        s_writer = xSemaphoreCreateMutex();
}

void flash_quiet_note_commit(void)
{
    s_commit_us = esp_timer_get_time();
}

int64_t flash_quiet_remaining_us(void)
{
    if (s_commit_us <= 0) return 0;
    int64_t elapsed = esp_timer_get_time() - s_commit_us;
    int64_t budget_us = (int64_t)FLASH_QUIET_BUDGET_MS * 1000;
    if (elapsed >= budget_us) return 0;
    return budget_us - elapsed;
}

bool flash_quiet_can_write(int64_t min_us)
{
    return flash_quiet_remaining_us() >= min_us;
}

bool flash_quiet_can_start_slice(void)
{
    return flash_quiet_can_write(FLASH_QUIET_SLICE_GUARD_US);
}

bool flash_quiet_writer_lock(TickType_t wait_ticks)
{
    if (!s_writer) flash_quiet_init();
    return xSemaphoreTake(s_writer, wait_ticks) == pdTRUE;
}

void flash_quiet_writer_unlock(void)
{
    if (s_writer) xSemaphoreGive(s_writer);
}
