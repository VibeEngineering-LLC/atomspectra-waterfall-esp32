// Host-тесты фильтра уровня для UART (#FW-50, main/debug_log_level_filter.h).
//
// Дефект, из-за которого тест появился: при CONFIG_LOG_COLORS=y строка esp_log
// начинается с CSI-последовательности, первым непробельным символом оказывается
// \033, буква уровня не находится и DEBUG уходит на UART. На машине, где
// CONFIG_LOG_COLORS выключен, это не воспроизводится вообще — поэтому правило
// проверяется на хосте, а не «глазами в терминале».
#include "debug_log_level_filter.h"
#include "test_util.h"
#include <string.h>

static bool uart(const char *s)
{
    return dbglog_line_goes_to_uart(s, (int)strlen(s));
}

// Раскраска ESP-IDF: E=31, W=33, I=32, D и V идут без цвета либо с 0;36m.
#define CSI(color) "\033[0;" color "m"

void dbglog_filter_suite(void)
{
    // Без раскраски — базовое поведение.
    CHECK(uart("E (12345) wifi_mgr: connect failed") == true);
    CHECK(uart("W (12345) wifi_mgr: retry") == true);
    CHECK(uart("I (12345) dbglog: ring enabled") == true);
    CHECK(uart("D (12345) dbglog: seq=42") == false);
    CHECK(uart("V (12345) dbglog: dump") == false);

    // CONFIG_LOG_COLORS=y — ровно тот случай, который чинится.
    CHECK(uart(CSI("31") "E (12345) wifi_mgr: connect failed") == true);
    CHECK(uart(CSI("33") "W (12345) wifi_mgr: retry") == true);
    CHECK(uart(CSI("32") "I (12345) dbglog: ring enabled") == true);
    CHECK(uart(CSI("36") "D (12345) dbglog: seq=42") == false);
    CHECK(uart(CSI("36") "V (12345) dbglog: dump") == false);

    // Сброс атрибутов без параметров ("\033[m") — тоже валидный CSI.
    CHECK(uart("\033[m" "D (1) t: x") == false);

    // Несколько CSI подряд и ведущие пробелы.
    CHECK(uart("  " CSI("0") CSI("36") "  D (1) t: x") == false);
    CHECK(uart("  " CSI("0") CSI("31") "  E (1) t: x") == true);

    // Обрезанные последовательности не должны уводить парсер за буфер и не
    // должны молча глушить строку: MAX_LINE в кольце режет длинные строки.
    CHECK(uart("\033") == true);
    CHECK(uart("\033[") == true);
    CHECK(uart("\033[0;3") == true);
    CHECK(dbglog_line_goes_to_uart("", 0) == true);

    // Не-CSI escape (ESC без '[') пропускается как один символ, следующий
    // символ разбирается как уровень.
    CHECK(uart("\033D (1) t: x") == false);

    // Строка без буквы уровня вообще (сырой printf) остаётся на UART.
    CHECK(uart("plain printf without level") == true);

    // Буква уровня в теле сообщения не должна влиять — читается только первый
    // непробельный символ.
    CHECK(uart("I (1) t: DEBUG-like text with D and V") == true);
}
