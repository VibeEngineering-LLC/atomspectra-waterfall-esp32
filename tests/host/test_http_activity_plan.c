// AWF-2a captive: http_uri_is_activity (main/http_activity_plan.h).
#include "http_activity_plan.h"
#include "test_util.h"

void test_http_activity_plan(void)
{
    // Все 6 captive-проб ОС (web_server.c:2291-2296) — НЕ активность.
    CHECK(!http_uri_is_activity("/generate_204"));
    CHECK(!http_uri_is_activity("/gen_204"));
    CHECK(!http_uri_is_activity("/hotspot-detect.html"));
    CHECK(!http_uri_is_activity("/library/test/success.html"));
    CHECK(!http_uri_is_activity("/ncsi.txt"));
    CHECK(!http_uri_is_activity("/connecttest.txt"));
    // Captive-landing (web_server.c:2297) — тоже НЕ активность.
    CHECK(!http_uri_is_activity("/captive"));
    // С query string — тот же путь, тот же вердикт.
    CHECK(!http_uri_is_activity("/generate_204?x=1"));

    // Реальные UI/API пути — АКТИВНОСТЬ.
    CHECK(http_uri_is_activity("/"));
    CHECK(http_uri_is_activity("/api/status"));
    CHECK(http_uri_is_activity("/api/command"));
    CHECK(http_uri_is_activity("/system"));
    CHECK(http_uri_is_activity("/api/spectrum.json"));
    // Не зарегистрирован вовсе (браузер шлёт сам вместе с открытой страницей) —
    // по умолчанию активность, до классификатора и не доходит в проде.
    CHECK(http_uri_is_activity("/favicon.ico"));
    // Похож на пробу, но НЕ точное совпадение — не должен ложно исключаться.
    CHECK(http_uri_is_activity("/generate_204x"));
    CHECK(http_uri_is_activity(NULL) == true);
}

// Обработчик 404 (незарегистрированный URI) — Firefox detectportal и любой
// другой пробник вне нашего списка исключений тоже НЕ активность.
void test_http_404_activity(void)
{
    CHECK(!http_404_is_activity("/success.txt"));
    CHECK(!http_404_is_activity("/canonical.html"));
    CHECK(!http_404_is_activity("/whatever-random-junk"));
    CHECK(!http_404_is_activity(NULL));
}

// F1: модель активности на уровне СОКЕТА (main/http_activity_plan.h).
#define QUIET_MS (600u * 1000u)   // 10 минут, как WIFI_RETURN_ACTIVITY_QUIET_MS

void test_http_activity_socket_model(void)
{
    http_activity_state_t st;

    // 1. Keep-alive: один и тот же сокет опрашивают 30 минут подряд — активен
    // ВСЁ ЭТО ВРЕМЯ, БЕЗ таймера (сокет открыт и уже user с первого запроса).
    http_activity_init(&st);
    http_activity_open(&st, 5);
    http_activity_request(&st, 5, true, 0);
    CHECK(!http_activity_quiet(&st, 30u * 60 * 1000u, QUIET_MS));

    // 2. WebSocket: одно рукопожатие — активность, затем долгая тишина БЕЗ
    // новых фреймов (WS сам по себе не шлёт запросов) — сокет всё ещё открыт.
    http_activity_init(&st);
    http_activity_open(&st, 6);
    http_activity_request(&st, 6, true, 0);
    CHECK(!http_activity_quiet(&st, 3600u * 1000u, QUIET_MS));

    // 3. Только пробы на сокете — тишина (никогда не стал user).
    http_activity_init(&st);
    http_activity_open(&st, 7);
    http_activity_request(&st, 7, false, 0);
    http_activity_request(&st, 7, false, 100000u);
    CHECK(http_activity_quiet(&st, 700000u, QUIET_MS));

    // 4. favicon (проба/404) ПОСЛЕ реального запроса на том же сокете — не
    // понижает: сокет остаётся user, активность не прерывается.
    http_activity_init(&st);
    http_activity_open(&st, 8);
    http_activity_request(&st, 8, true, 100);
    http_activity_request(&st, 8, false, 200);   // favicon.ico -> handle_404
    CHECK(!http_activity_quiet(&st, 100000000u, QUIET_MS));
    // 5. Закрыли вкладку — тишина ровно через 10 мин ОТ CLOSE, не раньше.
    http_activity_init(&st);
    http_activity_open(&st, 9);
    http_activity_request(&st, 9, true, 0);
    http_activity_close(&st, 9, 1000u);
    CHECK(!http_activity_quiet(&st, 1000u + QUIET_MS - 1, QUIET_MS));
    CHECK(http_activity_quiet(&st, 1000u + QUIET_MS, QUIET_MS));

    // Слот переиспользуется по fd%16: fd=3 и fd=19 делят слот. Новый accept()
    // (open) на fd=19 обязан затереть устаревший is_user от закрытого fd=3,
    // не протекать его активность в новый (несвязанный) сокет.
    http_activity_init(&st);
    http_activity_open(&st, 3);
    http_activity_request(&st, 3, true, 0);
    http_activity_close(&st, 3, 10u);
    http_activity_open(&st, 19);
    CHECK(http_activity_quiet(&st, 10u + QUIET_MS, QUIET_MS));   // только проба
    http_activity_request(&st, 19, false, 20u);
    CHECK(http_activity_quiet(&st, 10u + QUIET_MS, QUIET_MS));
}
