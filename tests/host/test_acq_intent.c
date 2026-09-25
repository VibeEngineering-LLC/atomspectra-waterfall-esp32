// Намерение набора по команде (main/acq_intent.h). Команды — как их шлют UI и страница «Сервис».
#include "acq_intent.h"
#include "test_util.h"

void acq_intent_suite(void)
{
    const struct { const char *cmd; uint8_t cur, exp; } c[] = {
        {"-sta",     ACQ_INTENT_UNKNOWN, ACQ_INTENT_RUN},     // кнопка «Старт»
        {"-sto",     ACQ_INTENT_RUN,     ACQ_INTENT_STOP},    // кнопка «Стоп»
        {"-sta 60",  ACQ_INTENT_RUN,     ACQ_INTENT_UNKNOWN}, // набор на 60 с — сам остановится
        {"-sta -s",  ACQ_INTENT_RUN,     ACQ_INTENT_UNKNOWN}, // тихий режим — гистограмм нет по замыслу
        {"-sto ",    ACQ_INTENT_RUN,     ACQ_INTENT_STOP},    // хвостовой пробел из поля ввода
        {"-sta\r\n", ACQ_INTENT_STOP,    ACQ_INTENT_RUN},     // перевод строки
        {"-stt",     ACQ_INTENT_RUN,     ACQ_INTENT_RUN},     // статус — не команда набора
        {"-inf",     ACQ_INTENT_STOP,    ACQ_INTENT_STOP},    // запрос параметров
        {"-stax",    ACQ_INTENT_RUN,     ACQ_INTENT_RUN},     // не -sta
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        uint8_t got = acq_intent_for_cmd(c[i].cmd, c[i].cur);
        if (got != c[i].exp) printf("acq_intent case %u: got %u\n", i, got);
        CHECK(got == c[i].exp);
    }
}

// P1-a: cmd_is_device_reset — «-rst» с пробелами/CR/LF, соседние команды НЕ сброс.
void cmd_is_device_reset_suite(void)
{
    const struct { const char *cmd; bool exp; } c[] = {
        {"-rst",     true},   {"-rst ",  true}, {"-rst\r\n", true}, {"-rst\t", true},
        {" -rst",    false},  {"-rsto",  false}, {"-rs",      false}, {"-rst1", false},
        {"-sta",     false},  {"-sto",   false}, {"-inf",     false}, {"",      false},
        // F5 (итоговое ревью 25.09): «-sta ... -r ...» — сброс перед стартом
        // (PROTOCOL.md:39), -r в любой позиции среди аргументов.
        {"-sta -r",       true}, {"-sta 60 -r",   true}, {"-sta -r -s",  true},
        {"-sta -s -r 60", true}, {"-sta -r\r\n",  true}, {"-sta  -r",    true},
        {"-sta -s",       false}, {"-sta 60",     false}, {"-sta",        false},
        {"-sta -rrandom", false}, {"-sta -run",   false}, // "-r" внутри другого токена — не флаг
        {"-startxyz -r",  false},                          // не -sta вовсе
        // F5 residual (ревью-2): разделитель — любой пробельный (таб тоже).
        {"-sta\t-r",      true}, {"-sta\t60\t-r", true}, {"-sta\t\t-r", true},
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        bool got = cmd_is_device_reset(c[i].cmd);
        if (got != c[i].exp) printf("cmd_is_device_reset case %u ('%s'): got %d\n", i, c[i].cmd, got);
        CHECK(got == c[i].exp);
    }
}
