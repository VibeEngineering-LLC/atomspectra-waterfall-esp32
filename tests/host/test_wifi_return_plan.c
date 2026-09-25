// AWF-2a (#2): решение о возврате из Field AP (main/wifi_return_plan.h).
#include "wifi_return_plan.h"
#include "test_util.h"

void test_wifi_return_plan(void)
{
    // forced Outdoor (ap_mode) — липкий по замыслу, сканировать/возвращаться нельзя никогда.
    CHECK(!wifi_return_scan_allowed(false, 0));
    CHECK(!wifi_return_should_reboot_to_sta(false, 0, true));

    // fallback, есть клиенты — сканировать нельзя (увело бы AP с канала под пользователем).
    CHECK(!wifi_return_scan_allowed(true, 1));
    CHECK(!wifi_return_should_reboot_to_sta(true, 1, true));

    // fallback, клиентов нет — можно сканировать.
    CHECK(wifi_return_scan_allowed(true, 0));

    // SSID не виден — не возвращаемся, даже если клиентов нет.
    CHECK(!wifi_return_should_reboot_to_sta(true, 0, false));

    // Единственный законный путь к возврату: fallback + 0 клиентов + SSID виден.
    CHECK(wifi_return_should_reboot_to_sta(true, 0, true));
}
