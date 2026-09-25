// AWF-2a (#1): расписание пауз реконнекта (main/wifi_reconnect_plan.h).
#include "wifi_reconnect_plan.h"
#include "test_util.h"

void test_wifi_reconnect_plan(void)
{
    CHECK(wifi_reconnect_delay_s(0) == 1);
    CHECK(wifi_reconnect_delay_s(1) == 2);
    CHECK(wifi_reconnect_delay_s(2) == 5);
    CHECK(wifi_reconnect_delay_s(3) == 10);
    CHECK(wifi_reconnect_delay_s(4) == 30);
    CHECK(wifi_reconnect_delay_s(5) == 60);
    CHECK(wifi_reconnect_delay_s(6) == 60);   // за пределами расписания — повтор последнего шага
    CHECK(wifi_reconnect_delay_s(100) == 60);
    CHECK(wifi_reconnect_delay_s(-1) == 1);   // защита от мусора

    // Сумма 1+2+5+10+30+60 = 108с < 300с порога — расписание одно фазу fallback не покрывает.
    CHECK(!wifi_reconnect_should_fallback(108));
    CHECK(!wifi_reconnect_should_fallback(299));
    CHECK(wifi_reconnect_should_fallback(300));
    CHECK(wifi_reconnect_should_fallback(301));
    CHECK(!wifi_reconnect_should_fallback(0));
}
