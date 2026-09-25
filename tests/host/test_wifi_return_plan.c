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

// P1: расписание бэкоффа возврата (счётчик неудач, переполнение uint32 мс).
void test_wifi_return_backoff(void)
{
    CHECK(wifi_return_backoff_s(0) == 150);
    CHECK(wifi_return_backoff_s(1) == 300);
    CHECK(wifi_return_backoff_s(5) == 3600);
    CHECK(wifi_return_backoff_s(6) == 3600);   // за пределами расписания — потолок 60 мин
    CHECK(wifi_return_backoff_s(1000000) == 3600);

    CHECK(wifi_return_fail_count_bump(0) == 1);
    CHECK(wifi_return_fail_count_bump(4) == 5);
    CHECK(wifi_return_fail_count_bump(5) == 5);   // потолок — дальше не растёт
    CHECK(wifi_return_fail_count_bump(9999) == 9999);  // уже за потолком — не трогаем

    // Обычный ход времени: 149999 мс < 150000 (шаг 0) — рано; 150000 — пора.
    CHECK(!wifi_return_backoff_elapsed(0, 149999u, 0u));
    CHECK(wifi_return_backoff_elapsed(0, 150000u, 0u));
    CHECK(!wifi_return_backoff_elapsed(5, 3599999u, 0u));
    CHECK(wifi_return_backoff_elapsed(5, 3600000u, 0u));

    // Переполнение uint32 мс (~49.7 сут = 4294967296 мс): last=0xFFFFFFF0,
    // now перевалило через 0 — разность в беззнаковой арифметике всё равно
    // корректна (тот же приём, что acq_watch_resend_due).
    uint32_t last = 0xFFFFFFF0u;                   // почти на границе переполнения
    CHECK(wifi_return_backoff_elapsed(0, last + 200000u, last));   // 200000мс >= 150000, ушли за 0
    CHECK(!wifi_return_backoff_elapsed(0, last + 100u, last));     // 100мс < 150000, тоже за 0
}
