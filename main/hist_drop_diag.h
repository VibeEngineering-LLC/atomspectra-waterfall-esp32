#pragma once
// #FW-8 residual — histogram sweep drop diagnostics (lab investigation 2026-08-12).
// Compile defaults ON for local soak builds. Disable: -DHIST_DROP_DIAG=0
// Experiment knobs (also -D…):
//   HIST_DROP_E1_NO_AUTOSAVE=1     — skip spectrum_autosave body (E1)
//   HIST_DROP_E3_AUTOSAVE_TICKS=N  — default 6 (=60s); 30 → 5 min (E3)
//   HIST_DROP_E4_NO_WAIT_COMMIT=1  — do not wait commit listener (E4)

#ifndef HIST_DROP_DIAG
#define HIST_DROP_DIAG 1
#endif

#ifndef HIST_DROP_E1_NO_AUTOSAVE
#define HIST_DROP_E1_NO_AUTOSAVE 0
#endif

#ifndef HIST_DROP_E3_AUTOSAVE_TICKS
#define HIST_DROP_E3_AUTOSAVE_TICKS 6
#endif

#ifndef HIST_DROP_E4_NO_WAIT_COMMIT
#define HIST_DROP_E4_NO_WAIT_COMMIT 0
#endif

#include <stdbool.h>
#include <stdint.h>

#if HIST_DROP_DIAG

void hist_drop_diag_note_commit(void);
void hist_drop_diag_autosave_begin(bool wait_timed_out);
void hist_drop_diag_autosave_end(void);
void hist_drop_diag_wf_flash_begin(const char *tag);
void hist_drop_diag_wf_flash_end(void);

bool hist_drop_diag_autosave_active(void);
bool hist_drop_diag_wf_active(void);
bool hist_drop_diag_last_wait_timed_out(void);
int64_t hist_drop_diag_ms_since_commit(void);
int64_t hist_drop_diag_ms_since_autosave_end(void);
const char *hist_drop_diag_wf_tag(void);

#else

static inline void hist_drop_diag_note_commit(void) {}
static inline void hist_drop_diag_autosave_begin(bool wait_timed_out) { (void)wait_timed_out; }
static inline void hist_drop_diag_autosave_end(void) {}
static inline void hist_drop_diag_wf_flash_begin(const char *tag) { (void)tag; }
static inline void hist_drop_diag_wf_flash_end(void) {}
static inline bool hist_drop_diag_autosave_active(void) { return false; }
static inline bool hist_drop_diag_wf_active(void) { return false; }
static inline bool hist_drop_diag_last_wait_timed_out(void) { return false; }
static inline int64_t hist_drop_diag_ms_since_commit(void) { return -1; }
static inline int64_t hist_drop_diag_ms_since_autosave_end(void) { return -1; }
static inline const char *hist_drop_diag_wf_tag(void) { return ""; }

#endif
