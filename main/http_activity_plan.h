// AWF-2a captive (дефект в 98e42cf): классификация URI — активность или
// captive-проба/landing. Тело дописывается Edit-ами (лимит хука 25 строк).
#pragma once
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

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

// F1 (итоговое ревью 25.09): метка активности на уровне СОКЕТА, не запроса и
// не открытия соединения. Прежняя модель метила открытие сокета — keep-alive
// (index.html опрос раз в 2с) и WS водопада держат ОДНО соединение часами,
// метка не обновлялась, через 10 мин работающий пользователь считался
// бездействующим. Модель: сокет становится «пользовательским» на ПЕРВОМ
// неpробном запросе; пока открыт хоть один такой сокет — активность есть
// БЕЗУСЛОВНО; отсчёт тишины — с последнего user-события среди закрытых.
#define HTTP_ACTIVITY_MAX_SOCKS 16
typedef struct {
    int  fd;
    bool used;      // слот занят (сокет открыт СЕЙЧАС)
    bool is_user;   // был хотя бы один НЕ-пробный запрос
} http_activity_sock_t;

typedef struct {
    http_activity_sock_t socks[HTTP_ACTIVITY_MAX_SOCKS];
    uint32_t last_user_event_ms;   // последний user-запрос ИЛИ close user-сокета
} http_activity_state_t;

static inline int http_activity_slot(int fd)
{
    return (int)(((unsigned)fd) % HTTP_ACTIVITY_MAX_SOCKS);
}

static inline void http_activity_init(http_activity_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

// Сокет открылся (httpd open_fn). Слот переиспользуется по fd%N — ДОЛЖЕН
// вызываться на каждый accept, иначе устаревший is_user другого fd в этом же
// слоте протечёт в новый сокет.
static inline void http_activity_open(http_activity_state_t *st, int fd)
{
    int i = http_activity_slot(fd);
    st->socks[i].fd = fd;
    st->socks[i].used = true;
    st->socks[i].is_user = false;
}

// Запрос на сокете fd, is_activity_uri — уже посчитанный вердикт
// http_uri_is_activity()/http_404_is_activity() (URI-парсинг ВНЕ этой
// функции). Проба (is_activity_uri=false) НЕ понижает is_user, если он уже
// true — только не поднимает его на этом запросе.
static inline void http_activity_request(http_activity_state_t *st, int fd,
                                          bool is_activity_uri, uint32_t now_ms)
{
    int i = http_activity_slot(fd);
    if (!(st->socks[i].used && st->socks[i].fd == fd) || !is_activity_uri) return;
    st->socks[i].is_user = true;
    st->last_user_event_ms = now_ms;
}

// Сокет закрылся (httpd close_fn). Если он был user — момент закрытия и есть
// «последнее user-событие» (пока сокет открыт, activity_any_user_open() уже
// даёт true без таймера — «позже» из требования оказывается здесь, при close).
static inline void http_activity_close(http_activity_state_t *st, int fd, uint32_t now_ms)
{
    int i = http_activity_slot(fd);
    if (!(st->socks[i].used && st->socks[i].fd == fd)) return;
    if (st->socks[i].is_user) st->last_user_event_ms = now_ms;
    st->socks[i].used = false;
}

static inline bool http_activity_any_user_open(const http_activity_state_t *st)
{
    for (int i = 0; i < HTTP_ACTIVITY_MAX_SOCKS; i++)
        if (st->socks[i].used && st->socks[i].is_user) return true;
    return false;
}

// Тишина >= threshold_ms: НЕТ открытого user-сокета И now-last_user_event_ms
// >= порога. Вычитание в uint32 переживает переполнение millis (тот же приём,
// что wifi_return_backoff_elapsed).
static inline bool http_activity_quiet(const http_activity_state_t *st,
                                        uint32_t now_ms, uint32_t threshold_ms)
{
    if (http_activity_any_user_open(st)) return false;
    return (now_ms - st->last_user_event_ms) >= threshold_ms;
}
