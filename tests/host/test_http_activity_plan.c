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
