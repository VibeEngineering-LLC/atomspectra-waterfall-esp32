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

/* P1-a + F5 (итоговое ревью 25.09): команды, обнуляющие прибор — «-rst»
 * (PROTOCOL.md:43, без параметров) И «-sta [xx] [-r] [-s]» с флагом «-r»
 * (PROTOCOL.md:39: «-r — сброс спектра перед стартом»), в любой позиции
 * среди пробельно-разделённых аргументов после -sta. До F5 «-sta ... -r»
 * не распознавался: прибор обнулялся, шлюз по счёту видел «сброс» и
 * сворачивал старый спектр в базу — пользователь чистил прибор, а Web UI
 * показывал старый спектр плюс новый. Тот же приём обрезки хвостовых
 * пробелов/CR/LF, что acq_intent_for_cmd; ведущие НЕ обрезаются. */
static inline bool cmd_is_device_reset(const char *cmd)
{
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t' || cmd[n - 1] == '\r' || cmd[n - 1] == '\n'))
        n--;
    if (n == 4 && strncmp(cmd, "-rst", 4) == 0) return true;
    if (!(n >= 4 && strncmp(cmd, "-sta", 4) == 0 && (n == 4 || cmd[4] == ' '))) return false;
    size_t i = 4;
    while (i < n) {
        while (i < n && cmd[i] == ' ') i++;
        size_t start = i;
        while (i < n && cmd[i] != ' ') i++;
        if (i - start == 2 && cmd[start] == '-' && cmd[start + 1] == 'r') return true;
    }
    return false;
}
