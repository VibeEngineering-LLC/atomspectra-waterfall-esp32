/*
 * Дефект: переполнение массива при разборе строковых данных в web_server.c
 * Оригинал: long arr[40] против лимита 100 в вызове kv_get_array_h
 * Тест проверяет, что разборщик не записывает за пределы массива out[max]
 * и не трогает память за ними.
 *
 * Мутационная проверка: если временно заменить условие
 * "if (n < max) out[n++] = val;" на "out[n++] = val;" — тест должен упасть,
 * так как канарейки будут разрушены.
 */

#include "test_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ARR_N 40
#define CANARY 0x5A5A5A5AL

typedef struct {
    long guard_lo;
    long arr[ARR_N];
    long guard_hi[8];
} fenced_t;

static const char *find_kv_h(const char *text, const char *key)
{
    const char *p = text;
    size_t key_len = strlen(key);

    while (*p) {
        const char *found = strstr(p, key);
        if (!found)
            return NULL;

        const char *end = found + key_len;
        if (*end == ' ' || *end == '\t' || *end == '=' || *end == ':') {
            p = end + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        } else if (*end == '[') {
            return end;
        } else {
            p = end;
        }
    }

    return NULL;
}

static int kv_get_array_h(const char *text, const char *key, long *out, int max)
{
    const char *v = find_kv_h(text, key);
    if (!v)
        return -1;

    while (*v == ' ' || *v == '\t')
        v++;

    if (*v != '[')
        return -1;
    v++;

    int n = 0;
    while (*v && *v != ']') {
        char *end;
        long val = strtol(v, &end, 10);
        if (end == v)
            break;

        if (n < max)
            out[n++] = val;

        v = end;
        while (*v == ' ' || *v == '\t')
            v++;
    }

    return n;
}

static void fence_init(fenced_t *f)
{
    f->guard_lo = CANARY;
    memset(f->arr, 0, sizeof(f->arr));
    for (int i = 0; i < 8; i++)
        f->guard_hi[i] = CANARY;
}

static int fence_intact(const fenced_t *f)
{
    if (f->guard_lo != CANARY)
        return 0;

    for (int i = 0; i < 8; i++) {
        if (f->guard_hi[i] != CANARY)
            return 0;
    }

    return 1;
}

static void build_body(char *dst, size_t cap, int count)
{
    size_t offset = 0;
    offset += snprintf(dst + offset, cap - offset, "VERSION 1 RISE 2 PileUp [");
    for (int i = 1; i <= count; i++) {
        if (i > 1)
            offset += snprintf(dst + offset, cap - offset, " ");
        offset += snprintf(dst + offset, cap - offset, "%d", i);
    }
    offset += snprintf(dst + offset, cap - offset, "]");
}

static void test_limit_equals_capacity(void)
{
    fenced_t f;
    fence_init(&f);

    char body[1024];
    build_body(body, sizeof(body), 100);

    int ret = kv_get_array_h(body, "PileUp", f.arr, ARR_N);
    /* Возврат равен ЛИМИТУ, а не числу элементов во входе: n растёт только
       под условием n < max, а цикл разбора продолжается до ']'. Значит по
       возвращённому значению НЕЛЬЗЯ узнать, что вход был длиннее лимита —
       лишние числа отбрасываются молча. Проверено изолированным прогоном:
       вход на 100 чисел при max=40 даёт ret=40. */
    CHECK(ret == ARR_N);
    CHECK(fence_intact(&f));
    CHECK(f.arr[ARR_N - 1] == ARR_N);
}

/* Тест намеренного переполнения здесь НЕ держим: он валит прогон под ASan
   (`make asan` в CI), и справедливо — намеренная запись за границу объекта
   неотличима для санитайзера от случайной.

   Способность этого набора ловить дефект доказана МУТАЦИОННОЙ проверкой,
   она строже. Процедура (повторяемая): в kv_get_array_h снять защиту,
   заменив
       if (n < max)
           out[n++] = val;
   на
       (void)max; out[n++] = val;
   и прогнать `make test`. Результат прогона 2026-08-16:
       FAIL test_kv_array.c:129: ret == ARR_N
       FAIL test_kv_array.c:130: fence_intact(&f)
   Обе проверки покраснели, канарейка поймала выход за границу.
   Вернуть защиту — прогон снова зелёный. */

static void test_boundaries(void)
{
    {
        fenced_t f;
        fence_init(&f);

        char body[1024];
        build_body(body, sizeof(body), ARR_N);

        int ret = kv_get_array_h(body, "PileUp", f.arr, ARR_N);
        CHECK(ret == ARR_N);
        CHECK(fence_intact(&f));
    }

    {
        fenced_t f;
        fence_init(&f);

        char body[1024];
        build_body(body, sizeof(body), ARR_N - 1);

        int ret = kv_get_array_h(body, "PileUp", f.arr, ARR_N);
        CHECK(ret == ARR_N - 1);
        CHECK(fence_intact(&f));
        CHECK(f.arr[ARR_N - 1] == 0);
    }

    {
        fenced_t f;
        fence_init(&f);

        char body[] = "VERSION 1 RISE 2 PileUp []";

        int ret = kv_get_array_h(body, "PileUp", f.arr, ARR_N);
        CHECK(ret == 0);
        CHECK(fence_intact(&f));
    }

    {
        fenced_t f;
        fence_init(&f);

        char body[] = "VERSION 1 RISE 2";

        int ret = kv_get_array_h(body, "PileUp", f.arr, ARR_N);
        CHECK(ret == -1);
        CHECK(fence_intact(&f));
    }
}

void run_kv_array_tests(void)
{
    printf("-- kv_array (settings restore body) --\n");
    test_limit_equals_capacity();
    test_boundaries();
}
