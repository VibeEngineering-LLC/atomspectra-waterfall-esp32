// AWF-2a (#2): возврат из полевой точки (Field AP), в которую вошли по
// одноразовому fallback (ap_fb_once) — НЕ из принудительного Outdoor
// (ap_mode, липкий по замыслу, сюда не возвращаемся). Свободен от ESP-IDF.
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// P1 (независимое ревью f091b01): SSID виден, но STA не держится (пароль
// сменился, DHCP молчит) -> без бэкоффа каждый цикл проверки уходил бы в
// esp_restart — ребут каждые ~4 мин, каждый рвёт набор. Нарастающие паузы
// между попытками возврата, счётчик сбрасывается только по GOT_IP.
#define WIFI_RETURN_BACKOFF_STEPS 6
static const uint32_t WIFI_RETURN_BACKOFF_S[WIFI_RETURN_BACKOFF_STEPS] =
    { 150, 300, 600, 1200, 2400, 3600 };   // 2.5, 5, 10, 20, 40, 60 мин

// Пауза перед попыткой №(fail_count+1); после исчерпания шагов — 60 мин, не больше.
static inline uint32_t wifi_return_backoff_s(uint32_t fail_count)
{
    if (fail_count >= WIFI_RETURN_BACKOFF_STEPS) fail_count = WIFI_RETURN_BACKOFF_STEPS - 1;
    return WIFI_RETURN_BACKOFF_S[fail_count];
}

// true, если с last_attempt_ms (монотонные мс этой Field-AP сессии; 0 = "с
// момента входа в неё") прошло достаточно для очередной попытки. Вычитание в
// uint32 переживает переполнение millis (~49.7 сут) — тот же приём, что
// acq_watch_resend_due (now_ms - last_ts_ms в беззнаковой арифметике).
static inline bool wifi_return_backoff_elapsed(uint32_t fail_count, uint32_t now_ms,
                                               uint32_t last_attempt_ms)
{
    uint32_t need_ms = wifi_return_backoff_s(fail_count) * 1000u;
    uint32_t elapsed_ms = now_ms - last_attempt_ms;
    return elapsed_ms >= need_ms;
}

// Счётчик неудачных попыток — растёт до потолка расписания, дальше не растёт.
static inline uint32_t wifi_return_fail_count_bump(uint32_t fail_count)
{
    if (fail_count < WIFI_RETURN_BACKOFF_STEPS - 1) fail_count++;
    return fail_count;
}
