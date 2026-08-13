// Pure histogram staging continuity (no ESP-IDF) — used by
// spectrum_process_histogram_chunk (#FW-8) and tests/host.
#include "spectrum_hist_stage.h"
#include <stdint.h>

void spectrum_hist_stage_reset(spectrum_hist_stage_t *st)
{
    if (!st) return;
    st->next_offset = UINT32_MAX; /* idle: no sweep in progress */
    st->ok = false;
}

void spectrum_hist_stage_note_chunk(spectrum_hist_stage_t *st, uint32_t offset,
                                    uint32_t bin_count)
{
    if (!st) return;
    if (offset == 0) {
        st->next_offset = 0;
        st->ok = true;
    } else if (st->next_offset == UINT32_MAX || offset != st->next_offset) {
        st->ok = false;
    }
    st->next_offset = offset + bin_count;
}

bool spectrum_hist_stage_complete(const spectrum_hist_stage_t *st,
                                  uint32_t channels)
{
    if (!st) return false;
    if (st->next_offset == UINT32_MAX) return false;
    return st->next_offset >= channels;
}
