#pragma once
// Разбор уровня строки esp_log для решения «пускать ли её на UART».
//
// Вынесено из debug_log_ring.c в отдельный заголовок без зависимостей от
// ESP-IDF, чтобы правило про CSI проверялось host-тестом
// (tests/host/test_debug_log_filter.c), а не только на живой плате, где
// раскраска зависит от локального sdkconfig.
#include <stdbool.h>

// Формат строки ESP-IDF: "E (12345) TAG: msg" — уровень это первый непробельный
// символ. Если перед ним идёт CSI ("\033[0;32m"), без пропуска escape-
// последовательности первым непробельным окажется \033: уровень не определится
// и строка уйдёт на UART как «не D».
//
// Важно: на IDF 5.4.2 это упрочнение, а не починка живого дефекта. LOG_COLOR_D
// и LOG_COLOR_V в esp_log_color.h — пустые строки даже при CONFIG_LOG_COLORS=y
// (цвет заведён только для E/W/I), поэтому штатные ESP_LOGD/ESP_LOGV префикса
// не получают и утечки DEBUG на UART сейчас нет. Пропуск CSI держим потому,
// что от него зависит корректность фильтра при любом источнике раскраски —
// смене таблицы цветов в будущем IDF, ESP_LOG_LEVEL_LOCAL с явным цветом или
// сторонних вызовах esp_log_write с готовым префиксом.
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
