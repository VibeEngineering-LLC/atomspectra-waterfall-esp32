#include "flash_quiet.h"
#include "flash_quiet_math.h"
#include "atomspectra.h"
#include "esp_timer.h"
#include "freertos/portmacro.h"

static int64_t s_commit_us;
static portMUX_TYPE s_commit_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_writer;

void flash_quiet_init(void)
{
    if (!s_writer)
        s_writer = xSemaphoreCreateMutex();
}

void flash_quiet_note_commit(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_commit_mux);
    s_commit_us = now;
    portEXIT_CRITICAL(&s_commit_mux);
}

static int64_t commit_us_load(void)
{
    int64_t v;
    portENTER_CRITICAL(&s_commit_mux);
    v = s_commit_us;
    portEXIT_CRITICAL(&s_commit_mux);
    return v;
}

int64_t flash_quiet_remaining_us(void)
{
    return flash_quiet_calc_remaining_us(commit_us_load(), esp_timer_get_time(),
                                         FLASH_QUIET_BUDGET_MS,
                                         usb_host_cdc_is_connected());
}

bool flash_quiet_can_write(int64_t min_us)
{
    return flash_quiet_calc_can_write(commit_us_load(), esp_timer_get_time(),
                                      FLASH_QUIET_BUDGET_MS,
                                      usb_host_cdc_is_connected(), min_us);
}

bool flash_quiet_can_start_slice(void)
{
    return flash_quiet_calc_can_start_slice(commit_us_load(), esp_timer_get_time(),
                                            FLASH_QUIET_BUDGET_MS,
                                            usb_host_cdc_is_connected());
}

bool flash_quiet_writer_lock(TickType_t wait_ticks)
{
    if (!s_writer) flash_quiet_init();
    if (!s_writer) return false;
    return xSemaphoreTake(s_writer, wait_ticks) == pdTRUE;
}

void flash_quiet_writer_unlock(void)
{
    if (s_writer) xSemaphoreGive(s_writer);
}
