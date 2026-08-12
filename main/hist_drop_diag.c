#include "hist_drop_diag.h"

#if HIST_DROP_DIAG

#include "esp_timer.h"
#include <string.h>

static volatile int64_t s_commit_us;
static volatile int64_t s_autosave_begin_us;
static volatile int64_t s_autosave_end_us;
static volatile int64_t s_wf_begin_us;
static volatile bool s_autosave_active;
static volatile bool s_wf_active;
static volatile bool s_last_wait_timed_out;
static char s_wf_tag[24];

void hist_drop_diag_note_commit(void)
{
    s_commit_us = esp_timer_get_time();
}

void hist_drop_diag_autosave_begin(bool wait_timed_out)
{
    s_last_wait_timed_out = wait_timed_out;
    s_autosave_active = true;
    s_autosave_begin_us = esp_timer_get_time();
}

void hist_drop_diag_autosave_end(void)
{
    s_autosave_active = false;
    s_autosave_end_us = esp_timer_get_time();
}

void hist_drop_diag_wf_flash_begin(const char *tag)
{
    s_wf_active = true;
    s_wf_begin_us = esp_timer_get_time();
    if (tag) {
        strncpy(s_wf_tag, tag, sizeof(s_wf_tag) - 1);
        s_wf_tag[sizeof(s_wf_tag) - 1] = '\0';
    } else {
        s_wf_tag[0] = '\0';
    }
}

void hist_drop_diag_wf_flash_end(void)
{
    s_wf_active = false;
}

bool hist_drop_diag_autosave_active(void) { return s_autosave_active; }
bool hist_drop_diag_wf_active(void) { return s_wf_active; }
bool hist_drop_diag_last_wait_timed_out(void) { return s_last_wait_timed_out; }

int64_t hist_drop_diag_ms_since_commit(void)
{
    if (s_commit_us <= 0) return -1;
    return (esp_timer_get_time() - s_commit_us) / 1000;
}

int64_t hist_drop_diag_ms_since_autosave_end(void)
{
    if (s_autosave_end_us <= 0) return -1;
    return (esp_timer_get_time() - s_autosave_end_us) / 1000;
}

const char *hist_drop_diag_wf_tag(void) { return s_wf_tag; }

#endif /* HIST_DROP_DIAG */
