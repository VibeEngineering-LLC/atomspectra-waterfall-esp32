#pragma once

/* #FW-65: host-testable model of waterfall Clear.
 * Production uses WF_SEG_CLEAR_PASSES only. The walk helpers simulate
 * LittleFS compact-dir vs collect-then-unlink — they are not linked into
 * the firmware (spectrogram.c talks to DIR/unlink itself). */

#include <string.h>

#define WF_SEG_CLEAR_PASSES 8

/* present[0 .. *n) is a packed list of live files (each slot = 1).
 * LittleFS compact + unlink-during-readdir: removing slot i shifts the
 * tail left, then the walker still advances — the new occupant is skipped. */
static inline void wf_seg_clear_walk_unlink_compact(int *present, int *n)
{
    int i = 0;
    while (i < *n) {
        if (!present[i]) {
            i++;
            continue;
        }
        memmove(&present[i], &present[i + 1],
                (size_t)(*n - i - 1) * sizeof(present[0]));
        (*n)--;
        i++; /* skip the entry that just slid into i */
    }
}

/* Snapshot every live name, then unlink the snapshot — no skip. */
static inline void wf_seg_clear_collect_unlink(int *present, int *n)
{
    (void)present;
    *n = 0;
}

/* Same, but at most cap names per pass (overflow → another pass). */
static inline void wf_seg_clear_collect_unlink_capped(int *present, int *n,
                                                      int cap, int max_passes)
{
    for (int pass = 0; pass < max_passes && *n > 0; pass++) {
        int take = *n < cap ? *n : cap;
        memmove(present, present + take,
                (size_t)(*n - take) * sizeof(present[0]));
        *n -= take;
    }
}
