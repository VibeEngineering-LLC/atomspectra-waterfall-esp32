// Host tests for #FW-8 flash quiet-budget arithmetic (P0-2 class).
#include "flash_quiet_math.h"
#include "test_util.h"
#include <stdio.h>

static void test_offline_full_budget(void)
{
    int64_t rem = flash_quiet_calc_remaining_us(/*commit*/0, /*now*/1000000,
                                                FLASH_QUIET_BUDGET_MS, /*usb*/false);
    CHECK(rem == (int64_t)FLASH_QUIET_BUDGET_MS * 1000);
    CHECK(flash_quiet_calc_can_start_slice(0, 1000000, FLASH_QUIET_BUDGET_MS, false) == true);
}

static void test_live_no_commit_exhausted(void)
{
    int64_t rem = flash_quiet_calc_remaining_us(0, 1000000, FLASH_QUIET_BUDGET_MS, true);
    CHECK(rem == 0);
    CHECK(flash_quiet_calc_can_start_slice(0, 1000000, FLASH_QUIET_BUDGET_MS, true) == false);
}

static void test_live_mid_budget(void)
{
    int64_t commit = 1 * 1000000;
    int64_t now = commit + 100 * 1000; /* 100 ms into 300 ms budget */
    int64_t rem = flash_quiet_calc_remaining_us(commit, now, FLASH_QUIET_BUDGET_MS, true);
    CHECK(rem == 200 * 1000);
    CHECK(flash_quiet_calc_can_start_slice(commit, now, FLASH_QUIET_BUDGET_MS, true) == true);
}

static void test_live_guard_boundary(void)
{
    int64_t commit = 1000000;
    int64_t budget_us = (int64_t)FLASH_QUIET_BUDGET_MS * 1000;
    /* remaining == guard → can start; remaining == guard-1 → cannot */
    int64_t now_ok = commit + (budget_us - FLASH_QUIET_SLICE_GUARD_US);
    int64_t now_no = now_ok + 1;
    CHECK(flash_quiet_calc_can_start_slice(commit, now_ok, FLASH_QUIET_BUDGET_MS, true) == true);
    CHECK(flash_quiet_calc_can_start_slice(commit, now_no, FLASH_QUIET_BUDGET_MS, true) == false);
}

static void test_torn_read_negative_elapsed(void)
{
    /* now < commit → treat as exhausted (refuse write) */
    CHECK(flash_quiet_calc_remaining_us(2000000, 1000000, FLASH_QUIET_BUDGET_MS, true) == 0);
}

void flash_quiet_suite(void)
{
    printf("flash_quiet: offline full budget\n");
    test_offline_full_budget();
    printf("flash_quiet: live no commit\n");
    test_live_no_commit_exhausted();
    printf("flash_quiet: live mid budget\n");
    test_live_mid_budget();
    printf("flash_quiet: guard boundary\n");
    test_live_guard_boundary();
    printf("flash_quiet: torn read\n");
    test_torn_read_negative_elapsed();
}
