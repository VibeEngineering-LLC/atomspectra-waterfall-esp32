// Резервные снимки спектра (issue #52): разбор имени и план ротации.
//
// Модуль намеренно свободен от ESP-IDF и файловой системы — как
// wf_seg_clear_plan.h у водопада. Всё, что решает «какие файлы удалить»,
// проверяется host-тестом на обычном gcc; сторона с fopen/remove живёт в
// spectrum.c и решений не принимает.
#pragma once

#include <inttypes.h>   // PRIu32 в BACKUP_NAME_FMT
#include <stdbool.h>
#include <stdint.h>

// Имя файла снимка: "bk_<sess>_<seq>.bin", каталог /storage/bk.
// sess — номер сессии платы (NVS boot/sess, +1 на каждую загрузку),
// seq — порядковый номер снимка внутри сессии, с 1.
#define BACKUP_NAME_FMT   "bk_%" PRIu32 "_%" PRIu32 ".bin"
#define BACKUP_KEEP_MAX   20

typedef struct {
    uint32_t sess;
    uint32_t seq;
} backup_id_t;

// "bk_3_12.bin" -> {3,12}. Всё прочее (в т.ч. "bk_3_12.bin.bak", "bk_-1_2.bin",
// "spec_0001.bin", лишние разделители) — false, out не трогается.
bool backup_parse_name(const char *name, backup_id_t *out);

// Старшинство: меньший sess старше; при равном sess — меньший seq.
// <0 если a старше b, >0 если младше, 0 при равенстве.
int backup_id_cmp(const backup_id_t *a, const backup_id_t *b);

// Сколько и каких файлов удалить, чтобы ПОСЛЕ добавления одного нового
// в каталоге осталось не больше keep штук.
//   have/n   — что лежит сейчас (порядок любой, readdir не сортирован),
//   keep     — настройка X (bk_n); keep<=0 значит «удалить всё».
//   to_delete/cap — выход, самые старые первыми.
// Возвращает число заполненных элементов, либо -1 при cap меньше нужного
// (вызывающий не должен молча удалить часть).
int backup_rotate_plan(const backup_id_t *have, int n, int keep,
                       backup_id_t *to_delete, int cap);

// AWF-1: индекс самого нового (старшего по backup_id_cmp) элемента среди
// have[0..n-1] — для восстановления "последней линией обороны". -1 при n<=0.
int backup_newest_index(const backup_id_t *have, int n);
