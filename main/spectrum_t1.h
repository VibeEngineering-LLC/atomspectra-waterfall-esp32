#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Vendor T1 invalid >=4s after instrument cold start; +1s margin.
 * usb_connect_task period is 2s → follow-up TX lands at ~5–7s after CDC-open. */
#define SPECTRUM_T1_REFRESH_MS 5000u

float spectrum_temp_from_token(const char *p);
int   spectrum_temp_json(char *buf, size_t cap, float t);

/* First parsed -inf of a USB session, or any -inf before open+5s
 * (reconnect retries / backup). uint32 subtraction matches refresh_due. */
static inline bool spectrum_t1_hold_active(uint32_t session_inf,
                                           uint32_t now_ms,
                                           uint32_t open_ms)
{
    return session_inf == 0 || (now_ms - open_ms) < SPECTRUM_T1_REFRESH_MS;
}
