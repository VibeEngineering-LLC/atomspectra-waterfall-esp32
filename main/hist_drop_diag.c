#include "hist_drop_diag.h"

#if HIST_DROP_DIAG

#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "hist_drop";

static volatile int64_t s_commit_us;
static volatile int64_t s_burst_start_us;
static volatile int64_t s_prev_commit_us;
static volatile int64_t s_autosave_begin_us;
static volatile int64_t s_autosave_end_us;
static volatile int64_t s_wf_begin_us;
static volatile bool s_autosave_active;
static volatile bool s_wf_active;
static volatile bool s_last_wait_timed_out;
static char s_wf_tag[24];

/* I1: accumulate quiet/burst samples; emit every N sweeps. */
#define QUIET_LOG_EVERY 30
#define QUIET_RING      64
static int64_t s_quiet_ms_ring[QUIET_RING];
static int64_t s_burst_ms_ring[QUIET_RING];
static unsigned s_quiet_n;
static unsigned s_sweep_log_ctr;

static int64_t percentile_sorted(int64_t *a, unsigned n, unsigned pct)
{
    if (n == 0) return -1;
    /* insertion sort — N≤64 */
    for (unsigned i = 1; i < n; i++) {
        int64_t v = a[i];
        unsigned j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
    unsigned idx = (pct * (n - 1)) / 100;
    return a[idx];
}

void hist_drop_diag_note_burst_start(void)
{
    int64_t now = esp_timer_get_time();
    /* quiet = time from previous commit to this burst start */
    if (s_prev_commit_us > 0) {
        int64_t quiet_ms = (now - s_prev_commit_us) / 1000;
        int64_t burst_ms = -1;
        if (s_burst_start_us > 0 && s_prev_commit_us > s_burst_start_us)
            burst_ms = (s_prev_commit_us - s_burst_start_us) / 1000;
        unsigned slot = s_quiet_n % QUIET_RING;
        s_quiet_ms_ring[slot] = quiet_ms;
        s_burst_ms_ring[slot] = burst_ms;
        if (s_quiet_n < QUIET_RING) s_quiet_n++;
        else s_quiet_n++; /* keep counting; ring holds last QUIET_RING */
        if (++s_sweep_log_ctr >= QUIET_LOG_EVERY) {
            s_sweep_log_ctr = 0;
            unsigned n = s_quiet_n < QUIET_RING ? (unsigned)s_quiet_n : QUIET_RING;
            int64_t q[QUIET_RING], b[QUIET_RING];
            for (unsigned i = 0; i < n; i++) {
                q[i] = s_quiet_ms_ring[i];
                b[i] = s_burst_ms_ring[i];
            }
            (void)percentile_sorted(q, n, 50); /* sort ascending */
            int64_t qp05 = q[(5 * (n - 1)) / 100];
            int64_t qp50 = q[(50 * (n - 1)) / 100];
            int64_t qp95 = q[(95 * (n - 1)) / 100];
            (void)percentile_sorted(b, n, 50);
            int64_t bp50 = b[(50 * (n - 1)) / 100];
            ESP_LOGI(TAG,
                     "quiet_ms p05=%lld p50=%lld p95=%lld burst_p50=%lld n=%u",
                     (long long)qp05, (long long)qp50, (long long)qp95,
                     (long long)bp50, n);
        }
    }
    s_burst_start_us = now;
}

void hist_drop_diag_note_commit(void)
{
    s_commit_us = esp_timer_get_time();
    s_prev_commit_us = s_commit_us;
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
    s_wf_tag[0] = '\0';
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

int64_t hist_drop_diag_commit_us(void) { return s_commit_us; }

int64_t hist_drop_diag_quiet_remaining_us(int64_t budget_ms)
{
    if (s_commit_us <= 0 || budget_ms <= 0) return 0;
    int64_t elapsed = esp_timer_get_time() - s_commit_us;
    int64_t budget_us = budget_ms * 1000;
    if (elapsed >= budget_us) return 0;
    return budget_us - elapsed;
}

#endif /* HIST_DROP_DIAG */
