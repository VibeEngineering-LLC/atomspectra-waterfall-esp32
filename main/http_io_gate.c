#include "http_io_gate.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "http_io";

static SemaphoreHandle_t s_slot;      // binary: taken = HEAVY in progress
static portMUX_TYPE s_spin = portMUX_INITIALIZER_UNLOCKED;
static int s_waiters;
static uint32_t s_rejects;

void http_io_gate_init(void)
{
    if (!s_slot) {
        s_slot = xSemaphoreCreateBinary();
        if (s_slot) xSemaphoreGive(s_slot);
        ESP_LOGI(TAG, "HEAVY I/O gate ready (concurrency=1)");
    }
}

bool http_io_gate_try_enter(void)
{
    if (!s_slot) http_io_gate_init();
    return xSemaphoreTake(s_slot, 0) == pdTRUE;
}

void http_io_gate_leave(void)
{
    if (s_slot) xSemaphoreGive(s_slot);
}

bool http_io_gate_busy(void)
{
    if (!s_slot) return false;
    // Неразрушающая проверка: у бинарного семафора count==0 == слот занят.
    // Через take+give нельзя: между Take и Give фоновая задача отбирает слот у
    // реального HEAVY-запроса, и тот получает 503 на пустом месте.
    return uxSemaphoreGetCount(s_slot) == 0;
}

int http_io_gate_waiters(void)
{
    int w;
    portENTER_CRITICAL(&s_spin);
    w = s_waiters;
    portEXIT_CRITICAL(&s_spin);
    return w;
}

uint32_t http_io_gate_reject_count(void)
{
    uint32_t r;
    portENTER_CRITICAL(&s_spin);
    r = s_rejects;
    portEXIT_CRITICAL(&s_spin);
    return r;
}

static bool gate_enter_common(httpd_req_t *req, uint32_t wait_ms)
{
    if (!s_slot) http_io_gate_init();

    portENTER_CRITICAL(&s_spin);
    s_waiters++;
    portEXIT_CRITICAL(&s_spin);

    bool got = s_slot && xSemaphoreTake(s_slot, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
    if (got) {
        portENTER_CRITICAL(&s_spin);
        s_waiters--;
        portEXIT_CRITICAL(&s_spin);
        return true;
    }

    portENTER_CRITICAL(&s_spin);
    s_waiters--;
    s_rejects++;
    int pos = s_waiters + 1;
    portEXIT_CRITICAL(&s_spin);

    char body[96];
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"class\":\"heavy\",\"queue_pos\":%d,\"err\":\"busy\"}", pos);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Retry-After", "1");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return false;
}

bool http_io_gate_enter_or_503(httpd_req_t *req)
{
    return gate_enter_common(req, 0);
}

bool http_io_gate_enter_wait_or_503(httpd_req_t *req, uint32_t wait_ms)
{
    return gate_enter_common(req, wait_ms);
}
