#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef enum {
    DBGLOG_LEVEL_STANDARD = 0,
    DBGLOG_LEVEL_DETAILED = 1,
    DBGLOG_LEVEL_DEBUG    = 2,
} dbglog_level_t;

// Load NVS; if enabled, alloc ring + install hooks (call after spectrum_init, before spectrogram_init).
void debug_log_ring_boot(void);

bool debug_log_ring_enabled(void);
dbglog_level_t debug_log_ring_level(void);

// Persist + apply. enabled=off → full teardown (no heartbeat, no Layer A, no ring writes).
esp_err_t debug_log_ring_set_config(bool enabled, dbglog_level_t level);

uint32_t debug_log_ring_next_seq(void);
uint32_t debug_log_ring_dropped(void);
// Строки, отброшенные из-за занятого мьютекса кольца (не путать с вытеснением
// по кругу): ring_append не ждёт слот, чтобы не паниковать в ISR/critical.
uint32_t debug_log_ring_lost_busy(void);
uint32_t debug_log_ring_gen(void);
int      debug_log_ring_fill_pct(void);

// Append a line directly (bypasses esp_log / Layer A). Used by heartbeat.
void debug_log_ring_write_raw(const char *line);

// Dump text since seq into http response (text/plain). Sets X-Log-* headers.
esp_err_t debug_log_ring_http_dump(httpd_req_t *req, uint32_t since);

// Flush if gen matches (0 = any). Clears ring, bumps gen, resets seq.
esp_err_t debug_log_ring_flush(uint32_t upto_seq, uint32_t gen);

// Fill JSON object fields into caller buffer (null-terminated). Returns bytes written.
int debug_log_ring_meta_json(char *buf, size_t bufsz);
