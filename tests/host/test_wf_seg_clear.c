/* #FW-65: collect-then-unlink vs unlink-during-walk (LittleFS compact). */
#include "wf_seg_clear_plan.h"
#include "test_util.h"
#include <stdio.h>

static void fill_live(int *present, int n)
{
    for (int i = 0; i < n; i++) present[i] = 1;
}

static void test_walk_compact_leaves_remainder(void)
{
    int present[8];
    int n = 8;
    fill_live(present, n);
    wf_seg_clear_walk_unlink_compact(present, &n);
    CHECK(n > 0);
    CHECK(n == 4); /* 8 live, skip every other after compact → 4 leftover */
}

static void test_collect_then_unlink_empties(void)
{
    int present[8];
    int n = 8;
    fill_live(present, n);
    wf_seg_clear_collect_unlink(present, &n);
    CHECK(n == 0);
}

static void test_capped_needs_second_pass(void)
{
    int present[8];
    int n = 8;
    fill_live(present, n);
    wf_seg_clear_collect_unlink_capped(present, &n, 3, 1);
    CHECK(n == 5);
    wf_seg_clear_collect_unlink_capped(present, &n, 3, WF_SEG_CLEAR_PASSES);
    CHECK(n == 0);
}

void wf_seg_clear_suite(void)
{
    printf("wf_seg_clear: walk compact leaves remainder\n");
    test_walk_compact_leaves_remainder();
    printf("wf_seg_clear: collect then unlink empties\n");
    test_collect_then_unlink_empties();
    printf("wf_seg_clear: capped collect needs another pass\n");
    test_capped_needs_second_pass();
}
