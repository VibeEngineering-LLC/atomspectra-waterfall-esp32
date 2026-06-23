#pragma once

#include "esp_http_server.h"
#include <stdbool.h>

// Регистрирует все HTTP/WS-обработчики водопада на уже запущенном сервере
// и подписывает WS-броадкаст на новые строки спектрограммы.
void web_waterfall_register(httpd_handle_t server);

// Экспорт CSRF-проверки из web_server.c (csrf_check там static).
bool web_csrf_check(httpd_req_t *req);