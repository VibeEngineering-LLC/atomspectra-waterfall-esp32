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

// N1 (ревью-2): модель по МЕТКЕ ПОСЛЕДНЕГО пользовательского запроса, не по
// состоянию сокета (main/http_activity_plan.h).
#define QUIET_MS (600u * 1000u)   // 10 минут, как WIFI_RETURN_ACTIVITY_QUIET_MS

void test_http_activity_quiet_model(void)
{
    http_activity_state_t st;
    // 1. Зомби-сокет (телефон ушёл из зоны, TCP ESTABLISHED навсегда, новых
    // запросов нет): один запрос в t=0, дальше НИЧЕГО — тихо через 10 мин
    // НЕЗАВИСИМО от сокета (сокет тут вообще не участвует в модели).
    http_activity_init(&st);
    http_activity_note_request(&st, true, 0);
    CHECK(!http_activity_quiet(&st, QUIET_MS - 1, QUIET_MS));
    CHECK(http_activity_quiet(&st, QUIET_MS, QUIET_MS));
    CHECK(http_activity_quiet(&st, 24u * 3600 * 1000u, QUIET_MS));   // зонд: 24ч

    // 2. Опрос раз в 2с (index.html/system.html) 30 минут подряд — активен
    // ВСЁ ЭТО ВРЕМЯ (метка не успевает отстать на порог у работающего юзера).
    http_activity_init(&st);
    for (uint32_t t = 0; t <= 30u * 60 * 1000u; t += 2000u) {
        http_activity_note_request(&st, true, t);
        CHECK(!http_activity_quiet(&st, t, QUIET_MS));
    }

    // 3. Только пробы — метка никогда не поднимается, тихо сразу после порога.
    http_activity_init(&st);
    http_activity_note_request(&st, false, 0);
    http_activity_note_request(&st, false, 100000u);
    CHECK(http_activity_quiet(&st, 700000u, QUIET_MS));
}
