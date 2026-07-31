#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "atomspectra.h"

// Shared 2s spectrum snapshot for LIVE HTTP (JSON / binary / meta).
// Single-flight refresh: N concurrent tabs share one render.

#define SPECTRUM_HTTP_CACHE_TTL_US  (2000000LL)  // 2 s

void spectrum_http_cache_init(void);

// Refresh cache if stale; returns false if no spectrum yet.
bool spectrum_http_cache_ensure(void);

// Pointers valid until next ensure that rebuilds (call under no long sleep).
// Prefer copying bins/meta for send, or send while holding via helpers below.
const spectrum_data_t *spectrum_http_cache_data(void);

uint32_t spectrum_http_cache_render_count(void);

// Send cached full JSON (builds once per TTL). Adds X-Spectrum-Render-Count.
esp_err_t spectrum_http_send_json(httpd_req_t *req);

// Send bins only (uint32 LE × SPECTRUM_CHANNELS).
esp_err_t spectrum_http_send_bins(httpd_req_t *req);

// Send meta JSON without bins (cps, total, calib, …).
esp_err_t spectrum_http_send_meta(httpd_req_t *req);
