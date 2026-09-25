// AWF-2a (#2): возврат из полевой точки (Field AP), в которую вошли по
// одноразовому fallback (ap_fb_once) — НЕ из принудительного Outdoor
// (ap_mode, липкий по замыслу, сюда не возвращаемся). Свободен от ESP-IDF.
#pragma once

#include <stdbool.h>

// Сканировать эфир на сохранённый SSID можно, только когда к точке не
// подключён ни один клиент (сканирование уводит AP с рабочего канала).
static inline bool wifi_return_scan_allowed(bool entered_by_fallback, int ap_clients)
{
    if (!entered_by_fallback) return false;
    return ap_clients <= 0;
}

// Ребут обратно в STA — только когда вход был по fallback (не forced
// Outdoor), клиентов на точке сейчас нет, и сохранённый SSID РЕАЛЬНО увиден
// сканированием. Слепой возврат по таймеру запрещён: ребут рвёт связь с
// прибором каждый раз.
static inline bool wifi_return_should_reboot_to_sta(bool entered_by_fallback,
                                                     int ap_clients,
                                                     bool saved_ssid_visible)
{
    if (!entered_by_fallback) return false;
    if (ap_clients > 0) return false;
    return saved_ssid_visible;
}
