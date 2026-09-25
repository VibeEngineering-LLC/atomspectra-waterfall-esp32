// AWF-2a (#2): возврат из полевой точки (Field AP), в которую вошли по
// одноразовому fallback (ap_fb_once) — НЕ из принудительного Outdoor
// (ap_mode, липкий по замыслу, сюда не возвращаемся). Свободен от ESP-IDF.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// AWF-2a настройка (живой тест 25.09, шаг 2): раньше блокировал ЛЮБОЙ
// подключённый к Field AP клиент (ap_clients>0) — телефон оператора, САМ
// подключившийся к сохранённой сети, блокировал возврат 10+ минут, хотя
// ничего не запрашивал у веб-сервера платы. Теперь блокирует только
// АКТИВНОСТЬ — HTTP-запрос к веб-серверу за последние 10 минут, не сам факт
// подключения к AP. Простаивающий клиент возврат не блокирует: скан уводит
// AP с канала на пару секунд, он переподключится сам.
#define WIFI_RETURN_ACTIVITY_QUIET_MS (600u * 1000u)   // 10 минут без HTTP-активности

// true, если с last_activity_ms (монотонные мс ЭТОЙ Field-AP сессии; 0 =
// «активности ещё не было» с начала сессии) прошло >= порога тишины.
// Вычитание в uint32 переживает переполнение millis — тот же приём, что
// wifi_return_backoff_elapsed.
static inline bool wifi_return_activity_quiet(uint32_t now_ms, uint32_t last_activity_ms)
{
    uint32_t elapsed_ms = now_ms - last_activity_ms;
    return elapsed_ms >= WIFI_RETURN_ACTIVITY_QUIET_MS;
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

// Сканировать эфир на сохранённый SSID можно, только когда HTTP-активность
// на Field AP стихла (сканирование уводит AP с рабочего канала).
static inline bool wifi_return_scan_allowed(bool entered_by_fallback,
                                             uint32_t now_ms, uint32_t last_activity_ms)
{
    if (!entered_by_fallback) return false;
    return wifi_return_activity_quiet(now_ms, last_activity_ms);
}

// Ребут обратно в STA — только когда вход был по fallback (не forced
// Outdoor), HTTP-активность стихла, и сохранённый SSID РЕАЛЬНО увиден
// сканированием. Слепой возврат по таймеру запрещён: ребут рвёт связь с
// прибором каждый раз.
static inline bool wifi_return_should_reboot_to_sta(bool entered_by_fallback,
                                                     uint32_t now_ms, uint32_t last_activity_ms,
                                                     bool saved_ssid_visible)
{
    if (!entered_by_fallback) return false;
    if (!wifi_return_activity_quiet(now_ms, last_activity_ms)) return false;
    return saved_ssid_visible;
}
