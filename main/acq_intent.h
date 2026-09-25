#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "acq_watch.h"

/* Намерение набора по текстовой команде шлюза (PROTOCOL.md:39-40: `-sta [xx] [-r] [-s]`, `-sto`).
 * Голый -sta → RUN. -sta с параметрами (таймер, тихий режим) → UNKNOWN: сторож не должен превращать
 * ограниченный или тихий набор в бесконечный. -sto → STOP. Прочие команды намерение не меняют. */
static inline uint8_t acq_intent_for_cmd(const char *cmd, uint8_t cur)
{
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t' || cmd[n - 1] == '\r' || cmd[n - 1] == '\n'))
        n--;
    if (n < 4 || strncmp(cmd, "-st", 3) != 0) return cur;
    if (cmd[3] == 'o' && (n == 4 || cmd[4] == ' ')) return ACQ_INTENT_STOP;
    if (cmd[3] == 'a' && n == 4) return ACQ_INTENT_RUN;
    if (cmd[3] == 'a' && cmd[4] == ' ') return ACQ_INTENT_UNKNOWN;
    return cur;
}

/* P1-a: команда — «-rst» (сброс прибора, PROTOCOL.md:43, без параметров).
 * Тот же приём обрезки хвостовых пробелов/CR/LF, что acq_intent_for_cmd —
 * любой путь, которым «-rst» уходит прибору (свои команды, /api/command,
 * TCP-мост), обязан очищать базу так же, как кнопка UI. Ведущие пробелы НЕ
 * обрезаются (симметрично acq_intent_for_cmd) — " -rst" не считается. */
static inline bool cmd_is_device_reset(const char *cmd)
{
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t' || cmd[n - 1] == '\r' || cmd[n - 1] == '\n'))
        n--;
    return n == 4 && strncmp(cmd, "-rst", 4) == 0;
}
