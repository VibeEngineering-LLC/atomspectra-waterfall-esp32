#pragma once
// Разбор уровня строки esp_log для решения «пускать ли её на UART».
//
// Вынесено из debug_log_ring.c в отдельный заголовок без зависимостей от
// ESP-IDF, чтобы правило про CSI проверялось host-тестом
// (tests/host/test_debug_log_filter.c), а не только на живой плате: дефект с
// ANSI-раскраской не воспроизводится, если у разработчика в локальном
// sdkconfig выключен CONFIG_LOG_COLORS.
#include <stdbool.h>

// Формат строки ESP-IDF: "E (12345) TAG: msg" — уровень это первый непробельный
// символ. При CONFIG_LOG_COLORS=y перед ним идёт CSI ("\033[0;32m"), и без
// пропуска escape-последовательности первым непробельным окажется \033: уровень
// не определится и DEBUG уедет на UART.
//
// Возвращает false только для D (debug) и V (verbose).
static inline bool dbglog_line_goes_to_uart(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == ' ' || c == '\t') continue;
        if (c == '\033') {
            int j = i + 1;
            if (j < n && buf[j] == '[') {
                j++;
                while (j < n && (unsigned char)buf[j] >= 0x30
                             && (unsigned char)buf[j] <= 0x3F) j++;  // параметры
                while (j < n && (unsigned char)buf[j] >= 0x20
                             && (unsigned char)buf[j] <= 0x2F) j++;  // intermediate
                if (j < n) j++;                                      // final byte
            }
            i = j - 1;  // компенсируем i++ в заголовке цикла
            continue;
        }
        return !(c == 'D' || c == 'V');
    }
    return true;
}
