#pragma once
// #FW-8: pure offset-continuity helper for histogram staging (host-testable).
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t next_offset;
    bool ok;
} spectrum_hist_stage_t;

void spectrum_hist_stage_reset(spectrum_hist_stage_t *st);
void spectrum_hist_stage_note_chunk(spectrum_hist_stage_t *st, uint32_t offset,
                                    uint32_t bin_count);
bool spectrum_hist_stage_complete(const spectrum_hist_stage_t *st,
                                  uint32_t channels);
