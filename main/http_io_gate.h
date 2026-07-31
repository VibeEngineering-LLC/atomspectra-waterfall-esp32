#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// HEAVY HTTP lane: concurrency=1. LIVE endpoints must not call these.
// Busy → short 503 + Retry-After (socket freed immediately).

void http_io_gate_init(void);

// Returns true if this request owns the HEAVY slot (must call leave).
bool http_io_gate_try_enter(void);
void http_io_gate_leave(void);

bool http_io_gate_busy(void);
int  http_io_gate_waiters(void);   // approximate queue interest counter

// Convenience: try_enter or send 503 JSON and return false.
bool http_io_gate_enter_or_503(httpd_req_t *req);

// То же, но сначала ждём слот до wait_ms. Для эндпоинтов, у которых есть
// клиенты без retry на 503 (scripts/wf_pull_client.py, внешний wf-recorder):
// им лучше подождать флеш-запись, чем получить ошибку. 503 остаётся как
// страховка, если слот не освободился за отведённое время.
bool http_io_gate_enter_wait_or_503(httpd_req_t *req, uint32_t wait_ms);

uint32_t http_io_gate_reject_count(void);
