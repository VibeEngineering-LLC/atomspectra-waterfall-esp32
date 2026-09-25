/* issue #52: разбор имени снимка и план ротации (main/backup_plan.c).
 *
 * Проверяется ровно то, что решает, какой файл будет стёрт с флеша: порядок
 * старшинства и количество удаляемых. Ошибка здесь молча уносит данные, ради
 * которых бэкап и делается, поэтому набор идёт от границ (n<keep, n==keep,
 * n>keep, keep=0) и от мусорных имён, а не от одного счастливого пути. */
#include "backup_plan.h"
#include "test_util.h"

#include <string.h>

static backup_id_t ID(uint32_t sess, uint32_t seq)
{
    backup_id_t v = { sess, seq };
    return v;
}

static void test_parse_ok(void)
{
    backup_id_t id;

    CHECK(backup_parse_name("bk_3_12.bin", &id) && id.sess == 3 && id.seq == 12);
    CHECK(backup_parse_name("bk_0_1.bin", &id) && id.sess == 0 && id.seq == 1);
    /* Сессия растёт весь срок жизни платы — крайнее значение обязано разбираться. */
    CHECK(backup_parse_name("bk_4294967295_7.bin", &id) && id.sess == 4294967295u && id.seq == 7);
}

static void test_parse_reject(void)
{
    backup_id_t id;

    /* Чужие файлы того же каталога и соседних: удалить их нельзя. */
    CHECK(!backup_parse_name("spec_0001.bin", &id));
    CHECK(!backup_parse_name("current.bin", &id));
    CHECK(!backup_parse_name("calib.bin", &id));
    /* Резервная копия снимка, сделанная человеком, — не снимок. */
    CHECK(!backup_parse_name("bk_3_12.bin.bak", &id));
    CHECK(!backup_parse_name("bk_3_12.bi", &id));
    CHECK(!backup_parse_name("bk_3_12", &id));
    /* Формы, которые strtoul проглотил бы, а имени файла соответствовать не могут. */
    CHECK(!backup_parse_name("bk_-1_2.bin", &id));
    CHECK(!backup_parse_name("bk_+1_2.bin", &id));
    CHECK(!backup_parse_name("bk_ 1_2.bin", &id));
    CHECK(!backup_parse_name("bk_0x3_2.bin", &id));
    CHECK(!backup_parse_name("bk__2.bin", &id));
    CHECK(!backup_parse_name("bk_3__2.bin", &id));
    CHECK(!backup_parse_name("bk_3_2_9.bin", &id));
    CHECK(!backup_parse_name("bk_99999999999_2.bin", &id));  /* переполнение u32 */
    /* Ведущие нули: имя удаляемого файла восстанавливается из чисел, поэтому
     * "bk_03_1.bin" разобрался бы в {3,1} и удалять пошли бы ДРУГОЙ файл, а
     * фантом навсегда занял бы слот ротации. */
    CHECK(!backup_parse_name("bk_03_1.bin", &id));
    CHECK(!backup_parse_name("bk_3_01.bin", &id));
    CHECK(!backup_parse_name("bk_00_0.bin", &id));
    /* Одиночный ноль — законное значение, не «ведущий ноль». */
    CHECK(backup_parse_name("bk_0_0.bin", &id) && id.sess == 0 && id.seq == 0);
    CHECK(!backup_parse_name("bk_", &id));
    CHECK(!backup_parse_name("", &id));
    CHECK(!backup_parse_name(NULL, &id));

    /* Отвергнутое имя не должно записывать в out (вызывающий держит там
     * предыдущий разбор и полагается на возвращённое значение). */
    backup_id_t keep = ID(7, 7);
    id = keep;
    CHECK(!backup_parse_name("spec_0001.bin", &id));
    CHECK(id.sess == keep.sess && id.seq == keep.seq);
}

static void test_cmp(void)
{
    CHECK(backup_id_cmp(&(backup_id_t){2, 1}, &(backup_id_t){3, 1}) < 0);
    CHECK(backup_id_cmp(&(backup_id_t){3, 1}, &(backup_id_t){2, 9}) > 0);
    CHECK(backup_id_cmp(&(backup_id_t){3, 1}, &(backup_id_t){3, 2}) < 0);
    CHECK(backup_id_cmp(&(backup_id_t){3, 2}, &(backup_id_t){3, 2}) == 0);
    /* Сессия старше номера: снимок №1 новой сессии моложе снимка №9 старой. */
    CHECK(backup_id_cmp(&(backup_id_t){4, 1}, &(backup_id_t){3, 9}) > 0);
}

static void test_plan_counts(void)
{
    backup_id_t have[8], del[8];

    for (int i = 0; i < 8; i++) have[i] = ID(1, (uint32_t)(i + 1));

    /* Место ещё есть — не удаляем ничего. */
    CHECK(backup_rotate_plan(have, 0, 3, del, 8) == 0);
    CHECK(backup_rotate_plan(have, 1, 3, del, 8) == 0);
    CHECK(backup_rotate_plan(have, 2, 3, del, 8) == 0);
    /* Каталог полон: один вон, один придёт. */
    CHECK(backup_rotate_plan(have, 3, 3, del, 8) == 1);
    /* Настройку уменьшили с 8 до 3 — за один проход догоняем. */
    CHECK(backup_rotate_plan(have, 8, 3, del, 8) == 6);
    /* keep=1: остаётся только новый. */
    CHECK(backup_rotate_plan(have, 4, 1, del, 8) == 4);
    /* Выключено — вычистить всё, включая последний. */
    CHECK(backup_rotate_plan(have, 8, 0, del, 8) == 8);
    CHECK(backup_rotate_plan(have, 0, 0, del, 8) == 0);
}

static void test_plan_picks_oldest(void)
{
    /* readdir отдаёт файлы в произвольном порядке — план обязан выбрать
     * старшие сам, а не довериться порядку обхода. */
    backup_id_t have[5] = { ID(4, 2), ID(3, 9), ID(4, 1), ID(3, 1), ID(5, 1) };
    backup_id_t del[5];

    int n = backup_rotate_plan(have, 5, 3, del, 5);
    CHECK(n == 3);
    CHECK(del[0].sess == 3 && del[0].seq == 1);
    CHECK(del[1].sess == 3 && del[1].seq == 9);
    CHECK(del[2].sess == 4 && del[2].seq == 1);

    /* Уцелеть обязаны ровно самые молодые. */
    for (int i = 0; i < n; i++) {
        CHECK(!(del[i].sess == 4 && del[i].seq == 2));
        CHECK(!(del[i].sess == 5 && del[i].seq == 1));
    }
}

static void test_plan_guards(void)
{
    backup_id_t have[4] = { ID(1, 1), ID(1, 2), ID(1, 3), ID(1, 4) };
    backup_id_t del[4];

    /* Буфер меньше нужного: лучше отказ, чем частичная ротация (иначе каталог
     * растёт молча, а вызывающий считает, что место освобождено). */
    CHECK(backup_rotate_plan(have, 4, 2, del, 2) == -1);
    CHECK(backup_rotate_plan(have, 4, 2, NULL, 4) == -1);
    CHECK(backup_rotate_plan(NULL, 4, 2, del, 4) == -1);
    /* Пустой каталог при отсутствующем массиве — законный ноль. */
    CHECK(backup_rotate_plan(NULL, 0, 3, del, 4) == 0);
    /* Ровно впритык — не отказ. */
    CHECK(backup_rotate_plan(have, 4, 2, del, 3) == 3);
}

// AWF-1 (#2): индекс самого нового снимка — для restore_from_latest_backup.
static void test_newest_index(void)
{
    backup_id_t have[5] = { ID(4, 2), ID(3, 9), ID(4, 1), ID(3, 1), ID(5, 1) };
    CHECK(backup_newest_index(have, 5) == 4);          // {5,1} — самая новая сессия
    CHECK(backup_newest_index(have, 1) == 0);
    CHECK(backup_newest_index(have, 0) == -1);
    CHECK(backup_newest_index(NULL, 0) == -1);
    CHECK(backup_newest_index(NULL, 3) == -1);

    backup_id_t same_sess[3] = { ID(2, 5), ID(2, 9), ID(2, 1) };
    CHECK(backup_newest_index(same_sess, 3) == 1);     // тот же sess — больший seq
}

void test_backup_plan(void)
{
    test_parse_ok();
    test_parse_reject();
    test_cmp();
    test_plan_counts();
    test_plan_picks_oldest();
    test_plan_guards();
    test_newest_index();
}
