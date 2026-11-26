/* FT8/FT4 receive band hopping - see ft8_hop.h. */
#include "ft8_hop.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adif/adif_log.h"
#include "cat/cat.h"
#include "ft8_hound.h"
#include "ft8_pileup.h"
#include "ft8_qso.h"
#include "ft8_rcvd_hold.h"
#include "ft8_test.h"
#include "ft8_tx.h"
#include "storage/settings.h"
#include "ui/ft8_screen_view.h"   /* ft8_preset_list(): the dials the page tunes to */

static const char *TAG = "ft8_hop";

/* Stay on each band: settings ft8_hop_dwell_min, default 2 minutes (WSJT-X
 * improved's period for FT8/FT4/MSK144, mainwindow.cpp bandHopping). The hop
 * is on a UTC multiple of the stay, so every allowed value must
 *  - be a whole number of FT8 (15 s) and FT4 (7.5 s) slots: any whole minute
 *    is, so the stay ends on a slot boundary in both modes;
 *  - divide 1440, so the grid does not slip at midnight UTC.
 * Any other stored value (a hand-edited config file) reads as 2. */
static const uint8_t kDwellMin[] = { 1, 2, 4, 6, 10, 15, 30 };

int ft8_hop_dwell_choices(const uint8_t **out)
{
    if (out) *out = kDwellMin;
    return (int)(sizeof kDwellMin / sizeof kDwellMin[0]);
}

bool ft8_hop_dwell_valid(int min)
{
    for (size_t i = 0; i < sizeof kDwellMin; i++)
        if (kDwellMin[i] == min) return true;
    return false;
}

static int dwell_sec(void)
{
    int m = settings_get_ft8_hop_dwell_min();
    return (ft8_hop_dwell_valid(m) ? m : 2) * 60;
}

static int64_t period_ms(void)
{
    return ft8_op_mode_get() == FT8_OP_MODE_FT4 ? 7500 : 15000;
}

/* Time the retune needs before the next boundary. ⚠ NOT MEASURED YET - the
 * QMX retune time has to be measured on the bench before this number is
 * trusted. The early cut ends the capture at 13.2 s (FT8) / 5.6 s (FT4), so
 * the gap is about 1.8 s; a hop that would leave less than this is skipped
 * for that cycle and the next dwell tries again. */
#define FT8_HOP_MIN_LEAD_MS 600

/* How long after a hop a band change seen on the CAT poll is taken to be the
 * hop's own. The FA poll confirms within about a second. */
#define HOP_OWN_WINDOW_US (10LL * 1000 * 1000)

static const char *const kHopBands[] = {
    "160", "80", "60", "40", "30", "20", "17", "15", "12", "10", "6",
};
#define N_HOP_BANDS ((int)(sizeof(kHopBands) / sizeof(kHopBands[0])))
_Static_assert(N_HOP_BANDS <= 16, "the mask is a uint16_t");

int ft8_hop_band_count(void) { return N_HOP_BANDS; }

const char *ft8_hop_band_name(int i)
{
    return (i >= 0 && i < N_HOP_BANDS) ? kHopBands[i] : NULL;
}

void ft8_hop_mask_text(uint16_t mask, char *buf, size_t len)
{
    if (!buf || !len) return;
    buf[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < N_HOP_BANDS && off + 5 < len; i++) {
        if (!(mask & (1u << i)) || !cat_radio_has_band(kHopBands[i])) continue;
        off += (size_t)snprintf(buf + off, len - off, "%s%s", off ? " " : "", kHopBands[i]);
    }
}

/* Band index of a dial, or -1. adif's table says "20M"; ours says "20". */
static int band_index_for(uint32_t hz)
{
    const char *b = adif_log_band_for_freq(hz);
    if (!b || !b[0]) return -1;
    char name[6];
    snprintf(name, sizeof name, "%s", b);
    size_t n = strlen(name);
    if (n && (name[n - 1] == 'M' || name[n - 1] == 'm')) name[n - 1] = '\0';
    for (int i = 0; i < N_HOP_BANDS; i++)
        if (strcmp(name, kHopBands[i]) == 0) return i;
    return -1;
}

/* The page's dial for this band in this sub-mode, or 0 (FT4 has no 160/60). */
static uint32_t dial_for(int band, ft8_op_mode_t mode)
{
    int n = 0;
    const ft8_preset_t *t = ft8_preset_list(mode, &n);
    for (int k = 0; t && k < n; k++)
        if (strcmp(t[k].band, kHopBands[band]) == 0) return t[k].freq_hz;
    return 0;
}

/* ---- per-slot dial latch ---------------------------------------------------- */

#define LATCH_N 8   /* 8 slots: 2 min of FT8, 1 min of FT4 - decodes land within ~6 s */
static struct { int64_t slot_sec; uint32_t dial_hz; } s_latch[LATCH_N];
static portMUX_TYPE s_latch_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_latch_next;

void ft8_slot_dial_latch(int64_t slot_sec, uint32_t dial_hz)
{
    portENTER_CRITICAL(&s_latch_mux);
    s_latch[s_latch_next].slot_sec = slot_sec;
    s_latch[s_latch_next].dial_hz  = dial_hz;
    s_latch_next = (s_latch_next + 1) % LATCH_N;
    portEXIT_CRITICAL(&s_latch_mux);
}

uint32_t ft8_slot_dial(int64_t slot_sec)
{
    uint32_t hz = 0;
    portENTER_CRITICAL(&s_latch_mux);
    for (int i = 0; i < LATCH_N; i++)
        if (s_latch[i].dial_hz && s_latch[i].slot_sec == slot_sec) { hz = s_latch[i].dial_hz; break; }
    portEXIT_CRITICAL(&s_latch_mux);
    return hz;
}

/* ---- gates --------------------------------------------------------------------
 * Each line is a reason a hop would be wrong now; the string is shown to the
 * operator.
 *
 * ⭐ HOPPING RUNS UNDER THE AUTOMATION (operator, 2026-11-26): "the whole idea
 * with hopping is to be able do all that you would normally do (CQ calling,
 * Auto-answer CQ, Auto-work pileup etc) but the band changes automatically.
 * And yes, if any automatic qso is ongoing when the stay-window is used then
 * wait until finished". Until then the hop was for an idle page only and
 * auto-answer or an armed CQ stopped it.
 *
 * So a CQ run with nobody answering, and auto-answer with nobody being worked,
 * do NOT block. A QSO with a partner, its 73 watch and report hold DO block;
 * the hop then waits (ft8_hop_due() is true on any later boundary once the
 * stay is used up, not only on the next stay boundary).
 *
 * The radio transmits on every ticked band. That is the operator's choice
 * when he ticks it, the same as WSPR hopping. Every key-down still passes the
 * band guard (check_tx_guard.py), so a ticked band outside the licence never
 * transmits. */
/* A pileup entry heard in the last two slots. */
static bool pileup_has_fresh_caller(void)
{
    ft8_pileup_entry_t pe[FT8_PILEUP_MAX];
    const int n = ft8_pileup_get_all(pe, FT8_PILEUP_MAX);
    const int64_t now = (int64_t)time(NULL);
    const int64_t win = 2 * period_ms() / 1000 + 1;
    for (int i = 0; i < n; i++)
        if (pe[i].call[0] && now - pe[i].last_seen_utc <= win) return true;
    return false;
}

/* ⛔ NO qmx_settings_t IN THIS FILE. It is 4268 B, and ft8_hop_get_state()
 * runs on httpd (/api/status) and taskLVGL (1 Hz): settings_load_all() here
 * crashed httpd with a Stack protection fault (bench1, 2026-11-25, 7005d57a).
 * Narrow getters only. */
static const char *blocked_reason(void)
{
    ft8_op_mode_t m = ft8_op_mode_get();
    if (m != FT8_OP_MODE_FT8 && m != FT8_OP_MODE_FT4) return "JS8";
    if (!cat_is_ready())                               return "radio not ready";
    if (cat_user_pause_active())                       return "radio paused";
    if (ft8_hound_enabled(ft8_hound_mode()) || ft8_qso_is_hound_active()) return "Hound";
    /* The CQ loop is "busy" with an empty target until somebody answers. */
    char who[16] = "";
    const bool busy = ft8_qso_is_busy(who, sizeof who);
    const bool cq_only = busy && !who[0] && ft8_qso_get_state() == FT8_QSO_CQ;
    if (busy && !cq_only)                              return "QSO in progress";
    /* An armed or on-air CQ goes with us to the next band; any other armed
     * TX (a reply, free text, a manual message) holds the hop. */
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE && !cq_only) return "TX armed";
    /* Only while re-sends of our final remain. A station that never hears
     * our RR73 keeps sending R-report; once the budget is spent the run goes
     * on, and so must the hop (Sim 2026-11-26: held 7.5 min on a 1 min stay). */
    {
        int rs = 0, mx = 0;
        if (ft8_qso_final_watch(NULL, 0, &rs, &mx) && rs < mx) return "waiting for 73";
    }
    if (ft8_qso_done_log_held() || ft8_rcvd_hold_any()) return "report hold";
    if (cat_radio_transmitting())                      return "transmitting";
    if (cat_poll_held_by_other(0))                     return "radio busy";
    /* Auto-work pileup: a caller still calling us is an automatic QSO about
     * to start. Older entries do not hold the hop (the list keeps them an
     * hour); the hop clears them, they were heard on the old band. */
    if (settings_get_ft8_auto_pileup() && pileup_has_fresh_caller()) return "callers waiting";
    return NULL;
}

/* Mask bits for bands this radio has and this sub-mode has a dial for. */
static uint16_t usable_mask(uint16_t mask, ft8_op_mode_t mode)
{
    uint16_t u = 0;
    for (int i = 0; i < N_HOP_BANDS; i++)
        if ((mask & (1u << i)) && cat_radio_has_band(kHopBands[i]) && dial_for(i, mode))
            u |= (uint16_t)(1u << i);
    return u;
}

/* Next usable band AFTER the current one, wrapping - table order, so the
 * operator can predict it. -1 when none differs from the current band. */
static int next_band(uint16_t usable, int cur)
{
    for (int step = 1; step <= N_HOP_BANDS; step++) {
        int i = (cur < 0) ? (step - 1) : (cur + step) % N_HOP_BANDS;
        if (usable & (1u << i)) return (i == cur) ? -1 : i;
    }
    return -1;
}

static uint32_t         s_own_hz;
static volatile int64_t s_own_us;

/* The band the radio is on. cat_get_frequency() follows the FA poll, so for
 * about a second after a hop it still names the old band; inside the hop's
 * own window the hop's target is the answer. */
static int current_band(void)
{
    const int64_t t = s_own_us;
    if (t && esp_timer_get_time() - t <= HOP_OWN_WINDOW_US) return band_index_for(s_own_hz);
    return band_index_for(cat_get_frequency());
}

uint32_t ft8_hop_dial_now(void)
{
    const int64_t t = s_own_us;
    if (t && esp_timer_get_time() - t <= HOP_OWN_WINDOW_US && s_own_hz) return s_own_hz;
    return cat_get_frequency();
}

bool ft8_hop_wait_confirmed(int timeout_ms)
{
    const int64_t t = s_own_us;
    if (!t || esp_timer_get_time() - t > HOP_OWN_WINDOW_US) return true;   /* no hop pending */
    const int want = band_index_for(s_own_hz);
    if (want < 0) return true;
    /* The BAND, not the exact Hz: an operator who nudges the dial inside the
     * window must not lose the slot. The FA poll runs every 150 ms. */
    for (int waited = 0; ; waited += 20) {
        if (band_index_for(cat_get_frequency()) == want) {
            if (waited) ESP_LOGI(TAG, "hop confirmed by the radio after %d ms", waited);
            return true;
        }
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- the stay -----------------------------------------------------------------
 * s_stay_from_ms is the boundary the current band's stay began on. A stay
 * normally starts on a UTC multiple of the stay time. A hop that had to wait
 * for a QSO starts its new stay where it happened, so the next band gets a
 * full stay; the grid is not caught up. A band change by the operator (or the
 * first look at a band) starts the stay on the grid point before it.
 * Written on ft8_task only; read elsewhere under s_stay_mux. */
static portMUX_TYPE s_stay_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_stay_from_ms;
static int          s_stay_band = -1;

static void stay_set(int64_t from_ms, int band)
{
    portENTER_CRITICAL(&s_stay_mux);
    s_stay_from_ms = from_ms;
    s_stay_band    = band;
    portEXIT_CRITICAL(&s_stay_mux);
}

static int64_t stay_from_ms(void)
{
    portENTER_CRITICAL(&s_stay_mux);
    int64_t v = s_stay_from_ms;
    portEXIT_CRITICAL(&s_stay_mux);
    return v;
}

/* Keep the stay start current for the band the radio is on now. */
static void stay_track(int64_t next_boundary_ms)
{
    const int band = current_band();
    if (band == s_stay_band && s_stay_from_ms) return;
    const int64_t d = (int64_t)dwell_sec() * 1000;
    stay_set(((next_boundary_ms - 1) / d) * d, band);
}

bool ft8_hop_due(int64_t next_boundary_ms)
{
    if (__builtin_popcount(usable_mask(settings_get_ft8_hop_mask(), ft8_op_mode_get())) < 2) return false;
    stay_track(next_boundary_ms);
    if (next_boundary_ms < stay_from_ms() + (int64_t)dwell_sec() * 1000) return false;
    return blocked_reason() == NULL;
}

/* First slot index k with k*p >= t and k of the wanted parity (k even = EVEN). */
static int64_t first_slot_from(int64_t t, int64_t p, int odd)
{
    int64_t k = (t + p - 1) / p;
    if ((int)(k & 1) != odd) k++;
    return k;
}

int ft8_hop_cq_plan(int *parity)
{
    if (!settings_get_ft8_hop_cq_split()) return -1;
    if (__builtin_popcount(usable_mask(settings_get_ft8_hop_mask(), ft8_op_mode_get())) < 2) return -1;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t now = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    const int64_t p = period_ms(), d = (int64_t)dwell_sec() * 1000;

    /* The stay this CQ falls in. ft8_task tracks it once a slot; before that
     * (the first CQ of a run, Sim 2026-11-26: it went out ANY) or on a band
     * the tracker has not seen yet, use the grid stay that holds now - the
     * same start stay_track() will give it. */
    int64_t from;
    int band;
    portENTER_CRITICAL(&s_stay_mux);
    from = s_stay_from_ms;
    band = s_stay_band;
    portEXIT_CRITICAL(&s_stay_mux);
    if (!from || band != current_band()) from = (now / d) * d;
    const int64_t half = from + d / 2, end = from + d;

    /* ⛔ PARITY ALONE CANNOT SAY "ODD, BUT NOT BEFORE HALF-WAY". ft8_tx fires
     * a request on the next slot of its parity counted from NOW. Sim
     * 2026-11-26: CQ ODD at :15, in the 1st half of a 1 min stay. So when the
     * next slot of the wanted parity is not in the wanted half, the plan is
     * WAIT: the caller does not arm, and the CQ run's per-slot re-arm
     * (cq_listen_for_answers -> arm_current_if_idle) asks again next slot. */
    const int64_t e = first_slot_from(now, p, 0) * p;
    if (e < half) { *parity = 0; return 0; }                 /* 1st half: EVEN */
    const int64_t o_want = first_slot_from(now > half ? now : half, p, 1) * p;
    if (o_want < end) {                                        /* 2nd half: ODD */
        *parity = 1;
        return (first_slot_from(now, p, 1) * p == o_want) ? 0 : 1;
    }
    *parity = 0;                                 /* the next stay begins EVEN */
    return (e >= end) ? 0 : 1;
}

int ft8_hop_cq_parity(void)
{
    int par = -1;
    return ft8_hop_cq_plan(&par) < 0 ? -1 : par;
}

bool ft8_hop_run(int64_t next_boundary_ms)
{
    if (!ft8_hop_due(next_boundary_ms)) return false;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms  = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t lead_ms = next_boundary_ms - now_ms;
    if (lead_ms < FT8_HOP_MIN_LEAD_MS) {
        ESP_LOGW(TAG, "hop skipped: %lld ms to the boundary, need %d",
                 (long long)lead_ms, FT8_HOP_MIN_LEAD_MS);
        return false;
    }

    ft8_op_mode_t mode = ft8_op_mode_get();
    uint32_t cur_hz = cat_get_frequency();
    int cur = current_band();
    int nb  = next_band(usable_mask(settings_get_ft8_hop_mask(), mode), cur);
    if (nb < 0) return false;
    uint32_t hz = dial_for(nb, mode);
    if (!hz || hz == cur_hz) return false;

    /* Marked BEFORE the write: the FA poll can report the new band before
     * cat_set_frequency_forced() returns, and ui.c must already know it is ours. */
    s_own_hz = hz;
    s_own_us = esp_timer_get_time();
    if (cat_set_frequency_forced(hz) != ESP_OK) {
        s_own_us = 0;
        ESP_LOGW(TAG, "hop to %s m (%lu Hz) failed - CAT refused the write",
                 kHopBands[nb], (unsigned long)hz);
        return false;
    }
    ESP_LOGI(TAG, "band hop %s -> %s m (%lu Hz), %lld ms before the boundary",
             cur >= 0 ? kHopBands[cur] : "?", kHopBands[nb], (unsigned long)hz,
             (long long)lead_ms);
    stay_set(next_boundary_ms, nb);
    /* Callers in the pileup were heard on the old band; auto-work would call
     * them here. The operator's own band change clears it the same way
     * (ft8_band_change_stand_down); the hop keeps the CQ run and auto-answer,
     * so only this part of that function applies. */
    ft8_pileup_clear();
    return true;
}

bool ft8_hop_slot_on_current_band(int64_t slot_sec)
{
    const uint32_t d = ft8_slot_dial(slot_sec);
    if (!d) return true;                     /* not latched: as before the latch */
    const int a = band_index_for(d), b = current_band();
    return a < 0 || b < 0 || a == b;
}

bool ft8_hop_run_at_boundary(int64_t boundary_ms)
{
    /* Due at THIS boundary: the stay ended at or before it and the hop in
     * the gap before it did not run. */
    if (!ft8_hop_due(boundary_ms)) return false;
    ft8_op_mode_t mode = ft8_op_mode_get();
    uint32_t cur_hz = cat_get_frequency();
    int cur = current_band();
    int nb  = next_band(usable_mask(settings_get_ft8_hop_mask(), mode), cur);
    if (nb < 0) return false;
    uint32_t hz = dial_for(nb, mode);
    if (!hz || hz == cur_hz) return false;
    s_own_hz = hz;
    s_own_us = esp_timer_get_time();
    if (cat_set_frequency_forced(hz) != ESP_OK) {
        s_own_us = 0;
        ESP_LOGW(TAG, "late hop to %s m failed - CAT refused the write", kHopBands[nb]);
        return false;
    }
    ESP_LOGI(TAG, "band hop %s -> %s m (%lu Hz) AT the boundary (the gap before it was too short)",
             cur >= 0 ? kHopBands[cur] : "?", kHopBands[nb], (unsigned long)hz);
    stay_set(boundary_ms, nb);
    ft8_pileup_clear();
    return true;
}

bool ft8_hop_owns_band_change(uint32_t dial_hz)
{
    int64_t t = s_own_us;
    if (!t || esp_timer_get_time() - t > HOP_OWN_WINDOW_US) return false;
    int a = band_index_for(dial_hz), b = band_index_for(s_own_hz);
    return a >= 0 && a == b;
}

void ft8_hop_get_state(ft8_hop_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    ft8_op_mode_t mode = ft8_op_mode_get();
    uint16_t usable = usable_mask(settings_get_ft8_hop_mask(), mode);
    ft8_hop_mask_text(usable, out->bands, sizeof out->bands);
    out->hopping = __builtin_popcount(usable) >= 2;
    if (!out->hopping) return;
    out->blocked = blocked_reason();
    int nb = next_band(usable, band_index_for(cat_get_frequency()));
    if (nb >= 0) snprintf(out->next_band, sizeof out->next_band, "%s", kHopBands[nb]);
    time_t now = time(NULL);
    int ds = dwell_sec();
    out->dwell_min = ds / 60;
    const int64_t from = stay_from_ms();
    if (now > 1600000000 && from) {
        int64_t left = (from / 1000 + ds) - (int64_t)now;
        out->secs_to_hop = left < 0 ? 0 : (int)left;
    } else {
        out->secs_to_hop = -1;
    }
    out->cq_period = ft8_hop_cq_parity();
}
