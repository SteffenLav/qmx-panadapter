/* FT8/FT4 receive band hopping - see ft8_hop.h. */
#include "ft8_hop.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "adif/adif_log.h"
#include "cat/cat.h"
#include "ft8_hound.h"
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
 * Hopping is for an idle page only. Each line is a reason a hop would be wrong;
 * the string is shown to the operator. */
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
    if (settings_get_ft8_robot_en())                   return "auto-answer on";
    if (ft8_hound_enabled(ft8_hound_mode()) || ft8_qso_is_hound_active()) return "Hound";
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) return "TX armed";
    if (ft8_qso_is_busy(NULL, 0))                      return "QSO in progress";
    if (ft8_qso_final_watch(NULL, 0, NULL, NULL))      return "waiting for 73";
    if (ft8_qso_done_log_held() || ft8_rcvd_hold_any()) return "report hold";
    if (cat_radio_transmitting())                      return "transmitting";
    if (cat_poll_held_by_other(0))                     return "radio busy";
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

bool ft8_hop_due(int64_t next_boundary_ms)
{
    if ((next_boundary_ms / 1000) % dwell_sec() != 0 || next_boundary_ms % 1000 != 0) return false;
    if (__builtin_popcount(usable_mask(settings_get_ft8_hop_mask(), ft8_op_mode_get())) < 2) return false;
    return blocked_reason() == NULL;
}

static uint32_t         s_own_hz;
static volatile int64_t s_own_us;

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
    int cur = band_index_for(cur_hz);
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
    out->secs_to_hop = (now > 1600000000) ? (int)(ds - (now % ds)) : -1;
}
