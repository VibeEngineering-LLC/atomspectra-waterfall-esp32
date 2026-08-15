// Host tests for #FW-8 histogram staging gap detection.
#include "spectrum_hist_stage.h"
#include "test_util.h"
#include <stdio.h>

#define CH 8192
#define BINS 64

static void feed_contiguous(spectrum_hist_stage_t *st)
{
    for (uint32_t off = 0; off < CH; off += BINS)
        spectrum_hist_stage_note_chunk(st, off, BINS);
}

static void test_full_sweep_ok(void)
{
    spectrum_hist_stage_t st;
    spectrum_hist_stage_reset(&st);
    feed_contiguous(&st);
    CHECK(st.ok == true);
    CHECK(spectrum_hist_stage_complete(&st, CH) == true);
    CHECK(st.next_offset == CH);
}

static void test_gap_exp256_got448(void)
{
    spectrum_hist_stage_t st;
    spectrum_hist_stage_reset(&st);
    /* First 4 chunks: 0,64,128,192 → next=256 */
    for (uint32_t off = 0; off < 256; off += BINS)
        spectrum_hist_stage_note_chunk(&st, off, BINS);
    CHECK(st.ok == true);
    CHECK(st.next_offset == 256);
    /* Gap: got 448 instead of 256 (lab capture class A) */
    spectrum_hist_stage_note_chunk(&st, 448, BINS);
    CHECK(st.ok == false);
    CHECK(st.next_offset == 448 + BINS);
    /* Finish to 8192 — still not ok */
    for (uint32_t off = 448 + BINS; off < CH; off += BINS)
        spectrum_hist_stage_note_chunk(&st, off, BINS);
    CHECK(st.ok == false);
    CHECK(spectrum_hist_stage_complete(&st, CH) == true);
}

static void test_offset0_resets(void)
{
    spectrum_hist_stage_t st;
    spectrum_hist_stage_reset(&st);
    spectrum_hist_stage_note_chunk(&st, 0, BINS);
    spectrum_hist_stage_note_chunk(&st, 128, BINS); /* gap */
    CHECK(st.ok == false);
    spectrum_hist_stage_note_chunk(&st, 0, BINS); /* new sweep */
    CHECK(st.ok == true);
    CHECK(st.next_offset == BINS);
}

static void test_idle_gap_without_offset0(void)
{
    spectrum_hist_stage_t st;
    spectrum_hist_stage_reset(&st);
    CHECK(st.next_offset == 0xFFFFFFFFu);
    CHECK(st.ok == false);
    CHECK(spectrum_hist_stage_complete(&st, CH) == false);
    /* Stray mid-sweep chunk while idle → gap (firmware UINT32_MAX idle). */
    spectrum_hist_stage_note_chunk(&st, 128, BINS);
    CHECK(st.ok == false);
    CHECK(st.next_offset == 128 + BINS);
}

void hist_stage_suite(void)
{
    printf("hist_stage: full sweep\n");
    test_full_sweep_ok();
    printf("hist_stage: gap 256→448\n");
    test_gap_exp256_got448();
    printf("hist_stage: offset0 reset\n");
    test_offset0_resets();
    printf("hist_stage: idle gap without offset0\n");
    test_idle_gap_without_offset0();
}
