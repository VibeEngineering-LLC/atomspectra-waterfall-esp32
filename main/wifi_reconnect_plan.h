// AWF-2a (#1): паузы между попытками STA-реконнекта с нарастанием, планируются
// через esp_timer в wifi_manager.c (не блокируя обработчик событий). Модуль
// свободен от ESP-IDF — host-тестируется на обычном gcc (образец acq_watch.h).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define WIFI_RECONNECT_STEPS 6
static const uint32_t WIFI_RECONNECT_SCHEDULE_S[WIFI_RECONNECT_STEPS] =
    { 1, 2, 5, 10, 30, 60 };

// До ухода в fallback AP должно пройти не меньше ~5 минут — короткая
// перезагрузка роутера (1-2 мин) не должна уводить плату в поле раньше, чем
// роутер успевает подняться.
#define WIFI_RECONNECT_FALLBACK_S 300u

// Пауза перед (attempt+1)-й попыткой (attempt считается с 0). После
// исчерпания расписания повторяет последний шаг, пока не сработает fallback.
static inline uint32_t wifi_reconnect_delay_s(int attempt)
{
    if (attempt < 0) attempt = 0;
    if (attempt >= WIFI_RECONNECT_STEPS) attempt = WIFI_RECONNECT_STEPS - 1;
    return WIFI_RECONNECT_SCHEDULE_S[attempt];
}

// true — суммарное время без связи достигло порога, пора в полевую точку.
static inline bool wifi_reconnect_should_fallback(uint32_t elapsed_disconnected_s)
{
    return elapsed_disconnected_s >= WIFI_RECONNECT_FALLBACK_S;
}
