#pragma once

#include <stdbool.h>

// #FW-2/#FW-3: поведение прошивки при старте платы (persist в NVS, namespace "boot").
// Все флаги по умолчанию false — из коробки ничего не запускается и не чистится
// (решение оператора 2026-06-29). Пользователь включает галочки один раз в Web UI
// (страница «Система» → «Поведение при старте платы»); значения переживают
// ребут / OTA / safe-mode.
typedef struct {
    bool autostart_spectrum;    // на первом USB-коннекте после ребута послать -sta (набор спектра)
    bool autostart_waterfall;   // на первом USB-коннекте начать запись водопада (spectrogram_start + -sta)
    bool clear_spectrum;        // при старте очистить накопленный спектр (ESP spectrum_reset + -rst прибору)
    bool clear_waterfall;       // при старте очистить водопад (spectrogram_clear)
} boot_config_t;

// Читает конфиг из NVS. Любой отсутствующий ключ → false. Безопасно вызывать
// после nvs_flash_init() (выполняется в wifi_manager_init на boot).
void boot_config_load(boot_config_t *out);

// Пишет конфиг в NVS (namespace "boot"). Возвращает 0 при успехе, -1 при ошибке.
int  boot_config_save(const boot_config_t *in);
