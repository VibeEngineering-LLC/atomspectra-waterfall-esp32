// AWF-2a captive (дефект в 98e42cf): классификация URI — активность или
// captive-проба/landing. Тело дописывается Edit-ами (лимит хука 25 строк).
#pragma once
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

// Список — ДОСЛОВНО из регистрации обработчиков (main/web_server.c:2291-2297,
// не по памяти): 6 captive-проб ОС (Android/iOS/macOS/Windows) шлют сами,
// периодически, без участия человека (DNS-hijack в Field AP заворачивает все
// имена на 192.168.4.1 — wifi_manager.c start_field_ap) + сама лёгкая
// captive-landing /captive. Всё остальное (в т.ч. /favicon.ico — не
// зарегистрирован вовсе, сюда никогда не доходит) — АКТИВНОСТЬ по умолчанию:
// забыть исключить НОВУЮ пробу возможно, забыть разрешить НОВЫЙ UI-путь —
// невозможно по построению (default=true). Свободна от ESP-IDF.
static inline bool http_uri_is_activity(const char *uri)
{
    if (!uri) return true;
    // Путь без query string (?...) — регистрация обработчиков сравнивает по
    // точному пути, параметры к решению не относятся.
    char path[80];
    size_t n = 0;
    while (uri[n] && uri[n] != '?' && n + 1 < sizeof(path)) { path[n] = uri[n]; n++; }
    path[n] = '\0';

    static const char *const NOT_ACTIVITY[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html",
        "/library/test/success.html", "/ncsi.txt", "/connecttest.txt",
        "/captive",
    };
    for (size_t i = 0; i < sizeof(NOT_ACTIVITY) / sizeof(NOT_ACTIVITY[0]); i++)
        if (strcmp(path, NOT_ACTIVITY[i]) == 0) return false;
    return true;
}

// Незарегистрированный URI (реальный 404 — esp_http_server вызывает это ТОЛЬКО
// когда uri_match_fn не нашёл НИ ОДНОГО обработчика; 404, который шлёт САМ
// зарегистрированный обработчик через httpd_resp_send_err — другой путь, сюда
// не попадает, это настоящая активность). Незарегистрированный путь почти
// всегда — ещё один OS/браузер-пробник вне нашего списка (Firefox
// detectportal `/success.txt`, `/canonical.html` и т.п.), а не UI/API
// человека: у реальной страницы все ссылки/ресурсы — на зарегистрированные
// пути. Поэтому default здесь ПРОТИВОПОЛОЖНЫЙ http_uri_is_activity() — ложь.
static inline bool http_404_is_activity(const char *uri)
{
    (void)uri;
    return false;
}
