// Резервные снимки спектра (issue #52): разбор имени и план ротации.
// См. backup_plan.h. Ни файловой системы, ни ESP-IDF здесь нет.
#include "backup_plan.h"

#include <stddef.h>
#include <string.h>

// Разбор беззнакового десятичного числа без переполнения и без strtoul
// (locale-независимо, и "+3"/" 3"/"0x3" должны отвергаться, а не молча
// разбираться). Возвращает указатель за последней цифрой либо NULL.
static const char *parse_u32(const char *p, uint32_t *out)
{
    if (!p || *p < '0' || *p > '9') return NULL;
    // Ведущие нули запрещены, и это не косметика: имя удаляемого файла
    // ВОССТАНАВЛИВАЕТСЯ из разобранных чисел (spectrum.c), поэтому "bk_03_1.bin"
    // разобрался бы в {3,1}, а remove() пошёл бы удалять "bk_3_1.bin" — другой
    // файл. Фантом остался бы в каталоге навсегда, занимая слот ротации: план
    // каждый раз выбирал бы его как самый старый, удаление молча не удавалось,
    // и реальные снимки не удалялись бы уже никогда. "0" сам по себе допустим.
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return NULL;
    uint32_t v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (v > (UINT32_MAX - (uint32_t)(*p - '0')) / 10u) return NULL;  // переполнение
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
        if (++digits > 10) return NULL;
    }
    *out = v;
    return p;
}

bool backup_parse_name(const char *name, backup_id_t *out)
{
    if (!name || !out) return false;
    static const char pfx[] = "bk_";
    if (strncmp(name, pfx, sizeof(pfx) - 1) != 0) return false;

    const char *p = name + sizeof(pfx) - 1;
    uint32_t sess = 0, seq = 0;

    p = parse_u32(p, &sess);
    if (!p || *p != '_') return false;
    p = parse_u32(p + 1, &seq);
    if (!p) return false;

    // Хвост обязан быть ровно ".bin": "bk_3_12.bin.bak" — не наш файл, и удалять
    // его нельзя (это чужая копия, а не снимок).
    if (strcmp(p, ".bin") != 0) return false;

    out->sess = sess;
    out->seq  = seq;
    return true;
}

int backup_id_cmp(const backup_id_t *a, const backup_id_t *b)
{
    if (a->sess != b->sess) return (a->sess < b->sess) ? -1 : 1;
    if (a->seq  != b->seq)  return (a->seq  < b->seq)  ? -1 : 1;
    return 0;
}

int backup_rotate_plan(const backup_id_t *have, int n, int keep,
                       backup_id_t *to_delete, int cap)
{
    if (n < 0) n = 0;
    if (n > 0 && !have) return -1;

    // Сколько должно уйти: после записи ОДНОГО нового останется не больше keep.
    int need;
    if (keep <= 0) {
        need = n;              // выключено — вычистить каталог целиком
    } else {
        need = n - keep + 1;   // n существующих + 1 новый ≤ keep
        if (need < 0) need = 0;
        if (need > n) need = n;
    }
    if (need == 0) return 0;
    if (!to_delete || cap < need) return -1;   // частичную ротацию не выполняем

    // n ≤ BACKUP_KEEP_MAX+запас, поэтому выбор минимума за проход — дешевле
    // сортировки и не требует копии массива под мутацию.
    bool taken[BACKUP_KEEP_MAX * 2];
    int  cnt = n;
    if (cnt > (int)(sizeof(taken) / sizeof(taken[0])))
        cnt = (int)(sizeof(taken) / sizeof(taken[0]));
    if (need > cnt) need = cnt;
    memset(taken, 0, sizeof(taken));

    for (int k = 0; k < need; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            if (taken[i]) continue;
            if (best < 0 || backup_id_cmp(&have[i], &have[best]) < 0) best = i;
        }
        if (best < 0) return k;      // не должно случаться: need ≤ cnt
        taken[best] = true;
        to_delete[k] = have[best];
    }
    return need;
}

int backup_newest_index(const backup_id_t *have, int n)
{
    if (n <= 0 || !have) return -1;
    int best = 0;
    for (int i = 1; i < n; i++)
        if (backup_id_cmp(&have[i], &have[best]) > 0) best = i;
    return best;
}
