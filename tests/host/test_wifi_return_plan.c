// AWF-2a (#2): решение о возврате из Field AP (main/wifi_return_plan.h).
#include "wifi_return_plan.h"
#include "test_util.h"

void test_wifi_return_plan(void)
{
    // forced Outdoor (ap_mode) — липкий по замыслу, сканировать/возвращаться нельзя никогда.
    CHECK(!wifi_return_scan_allowed(false, 0, 0));
    CHECK(!wifi_return_should_reboot_to_sta(false, 0, 0, true));

    // AWF-2a доработка (живой тест 25.09): раньше блокировал ЛЮБОЙ клиент,
    // теперь — только АКТИВНОСТЬ (HTTP за последние 10 мин = 600000 мс).
    // Активность 0с назад (только что) — блокирует.
    CHECK(!wifi_return_scan_allowed(true, 0, 0));
    CHECK(!wifi_return_should_reboot_to_sta(true, 0, 0, true));
    // 599с назад — всё ещё блокирует (граница снизу).
    CHECK(!wifi_return_scan_allowed(true, 599000u, 0));
    // 601с назад — тишина, можно.
    CHECK(wifi_return_scan_allowed(true, 601000u, 0));
    // Телефон подключён без активности (клиент есть, но не листает страницы) —
    // возврат теперь РАЗРЕШЁН, если активности не было достаточно долго.
    CHECK(wifi_return_scan_allowed(true, 700000u, 0));

    // SSID не виден — не возвращаемся, даже если активность стихла.
    CHECK(!wifi_return_should_reboot_to_sta(true, 700000u, 0, false));

    // Единственный законный путь к возврату: fallback + активность стихла + SSID виден.
    CHECK(wifi_return_should_reboot_to_sta(true, 700000u, 0, true));

    // Переполнение uint32 мс: last_activity_ms почти на границе, now перевалило
    // через 0 — беззнаковая разность всё равно корректна (тот же приём, что
    // wifi_return_backoff_elapsed).
    uint32_t last = 0xFFFFFFF0u;
    CHECK(wifi_return_scan_allowed(true, last + 700000u, last));   // далеко за порог, за 0
    CHECK(!wifi_return_scan_allowed(true, last + 100u, last));     // рядом, тоже за 0
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
