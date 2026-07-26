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
    // Peek without taking: if take fails immediately, busy.
    if (xSemaphoreTake(s_slot, 0) == pdTRUE) {
        xSemaphoreGive(s_slot);
        return false;
    }
    return true;
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

bool http_io_gate_enter_or_503(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_spin);
    s_waiters++;
    portEXIT_CRITICAL(&s_spin);

    if (http_io_gate_try_enter()) {
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
