// AWF-1 (#2): выбор источника восстановления накопленного спектра. Модуль
// намеренно свободен от ESP-IDF и файловой системы (образец — acq_watch.h).
#pragma once

#include <stdbool.h>

typedef enum {
    RESTORE_SRC_NONE = 0,
    RESTORE_SRC_MAIN,
    RESTORE_SRC_TMP,
    RESTORE_SRC_BACKUP,
} restore_source_t;

// Старшинство: current.bin (подтверждён rename-ом) > tmp (полон и valid,
// обрыв случился между fclose и rename) > свежий бэкап (issue #52) > ничего.
static inline restore_source_t restore_pick_source(bool main_valid, bool tmp_valid,
                                                    bool have_backup)
{
    if (main_valid) return RESTORE_SRC_MAIN;
    if (tmp_valid)  return RESTORE_SRC_TMP;
    if (have_backup) return RESTORE_SRC_BACKUP;
    return RESTORE_SRC_NONE;
}
