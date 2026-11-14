/* RTTY receive on the device - see rtty_rx.h. R005. */
#include "rtty_rx.h"
#include "rtty_demod.h"
#include "rtty_baudot.h"
#include "rtty_auto.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rx_audio.h"
#include "rtty_tx.h"
#include "settings.h"

static const char *TAG = "rtty";

#define RTTY_RX_AFC_HZ   150.0f

#define TEXT_RING 2048          /* power of two */
#define LOG_LINE  96

/* Requested by rtty_rx_apply (any task), applied by the feed (audio task).
 * The seq is bumped after the struct is written; a torn read only means the
 * next frame restarts once more. */
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
/* Defaults agreed 2026-11-12 (R011): 45.45 Bd, 170 Hz, AFC and squelch on.
 * The one copy - config_io reads it through rtty_rx_defaults(). */
static const rtty_rx_cfg_t RTTY_DEFAULTS = {
    .on = false, .offset_hz = 1500, .reverse = false,
    .baud = 45.45f, .shift_hz = 170.0f, .afc = true, .squelch = true,
    .auto_mode = true,   /* R028: 45.45/170 stations change nothing; others are followed */
};
static rtty_rx_cfg_t s_cfg = RTTY_DEFAULTS;
static volatile uint32_t s_want_seq;
static volatile bool s_loaded;           /* R019: saved settings read into s_cfg */

void rtty_rx_defaults(rtty_rx_cfg_t *c) { *c = RTTY_DEFAULTS; }

/* R019: the settings survive a reboot. Read once, on the first use - which is
 * the page's show (boot restore included), long after settings_init(). Every
 * rtty_rx_apply() writes them back; settings dedupes and debounces the NVS
 * write, so a run of taps costs one write. `on` is not saved: the page owns it.
 * Before this, offset/reverse/speed/AFC/squelch came back as the defaults
 * after every boot (seen 2026-11-12: -1960 Hz returned as +1500). */
static void load_once(void)
{
    if (s_loaded) return;
    s_loaded = true;
    settings_rtty_t r;
    settings_get_rtty(&r);
    if (!r.valid) return;
    rtty_rx_cfg_t c = RTTY_DEFAULTS;
    c.offset_hz = r.offset_hz;
    c.shift_hz  = (float)r.shift_hz;
    c.baud      = r.baud_x100 / 100.0f;
    c.reverse   = r.reverse != 0;
    c.afc       = r.afc != 0;
    c.squelch   = r.squelch != 0;
    c.auto_mode = r.auto_mode != 0;
    if (!(c.baud >= 20.0f && c.baud <= 300.0f)) c.baud = RTTY_DEFAULTS.baud;
    if (!(c.shift_hz >= 50.0f && c.shift_hz <= 1000.0f)) c.shift_hz = RTTY_DEFAULTS.shift_hz;
    if (c.offset_hz < -3000 || c.offset_hz > 4500) c.offset_hz = RTTY_DEFAULTS.offset_hz;
    portENTER_CRITICAL(&s_cfg_mux);
    c.on = s_cfg.on;
    s_cfg = c;
    portEXIT_CRITICAL(&s_cfg_mux);
    ESP_LOGI(TAG, "settings: offset %d Hz %.2f Bd %.0f Hz%s afc=%d sq=%d", c.offset_hz,
             (double)c.baud, (double)c.shift_hz, c.reverse ? " reverse" : "", (int)c.afc, (int)c.squelch);
}

static void save(const rtty_rx_cfg_t *c)
{
    settings_rtty_t r = {
        .offset_hz = (int16_t)c->offset_hz,
        .shift_hz  = (uint16_t)(c->shift_hz + 0.5f),
        .baud_x100 = (uint16_t)(c->baud * 100.0f + 0.5f),
        .reverse = c->reverse, .afc = c->afc, .squelch = c->squelch,
        .valid = 1, .auto_mode = c->auto_mode,
    };
    settings_set_rtty(&r);
}

/* Owned by the audio task. */
static rtty_demod_t *s_dem;              /* PSRAM, ~18 KB, allocated once */
static rtty_auto_t  *s_auto;             /* R028, PSRAM, allocated on first auto use */
/* R028: a verdict that differs from the running cfg must hold this long, in
 * whole seconds of the same verdict, before it is applied. R120RB (weak, deep
 * QSB) showed a lone 1-s 50 Bd verdict in 54 s of 45.45 (harness, 2026-11-14). */
#define RTTY_AUTO_HOLD_S 3
static int      s_auto_same;             /* consecutive seconds of the pending verdict */
static float    s_auto_pb, s_auto_ps;
static bool     s_auto_pr;
static int64_t  s_auto_next_us;
static volatile bool s_observe;          /* estimator runs, applies nothing */

/* Development I/Q recorder: see rtty_rx_iqcap_start(). */
static float   *s_iq;
static volatile uint32_t s_iq_n, s_iq_cap;
static volatile bool s_iq_run;
static float    s_iq_fs;

bool rtty_rx_iqcap_start(float seconds)
{
    if (seconds < 1) seconds = 1;
    if (seconds > 20) seconds = 20;
    uint32_t cap = (uint32_t)(seconds * 6000.0f);
    s_iq_run = false;
    if (!s_iq || s_iq_cap < cap) {
        heap_caps_free(s_iq);
        s_iq = heap_caps_malloc((size_t)cap * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!s_iq) { s_iq_cap = 0; return false; }
    }
    s_iq_cap = cap; s_iq_n = 0; s_iq_run = true;
    ESP_LOGI(TAG, "iqcap: recording %.0f s (%u pairs, %u B PSRAM)", (double)seconds,
             (unsigned)cap, (unsigned)(cap * 2 * sizeof(float)));
    return true;
}

const float *rtty_rx_iqcap_data(uint32_t *n_pairs, float *fs_hz, bool *running)
{
    if (n_pairs) *n_pairs = s_iq_n;
    if (fs_hz) *fs_hz = s_iq_fs;
    if (running) *running = s_iq_run;
    return (s_iq && s_iq_n) ? s_iq : NULL;
}

void rtty_rx_set_observe(bool on) { s_observe = on; s_want_seq++; }
static rtty_baudot_t s_bd;
static rtty_rx_cfg_t s_run_cfg;
static uint32_t s_have_seq;
static float    s_have_fs;
static int      s_have_nco;
static bool     s_running;
static char     s_line[LOG_LINE + 1];
static int      s_line_n;

/* Shared: text ring + status snapshot. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char     s_text[TEXT_RING];
static uint32_t s_text_w;
static uint32_t s_text_floor;            /* Clear moves this; readers never go below */
static rtty_rx_status_t s_st;

void rtty_rx_get_cfg(rtty_rx_cfg_t *c)
{
    load_once();
    portENTER_CRITICAL(&s_cfg_mux);
    *c = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
}

void rtty_rx_apply(const rtty_rx_cfg_t *c)
{
    load_once();   /* or a later first get_cfg would overwrite this apply */
    rtty_rx_cfg_t n = *c;
    if (!(n.baud >= 20.0f && n.baud <= 300.0f)) n.baud = 45.45f;
    if (!(n.shift_hz >= 50.0f && n.shift_hz <= 1000.0f)) n.shift_hz = 170.0f;
    if (n.offset_hz < -3000) n.offset_hz = -3000;
    if (n.offset_hz >  4500) n.offset_hz =  4500;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = n;
    portEXIT_CRITICAL(&s_cfg_mux);
    s_want_seq++;
    save(&n);
    ESP_LOGI(TAG, "%s offset %d Hz %.2f Bd %.0f Hz%s afc=%d sq=%d auto=%d", n.on ? "ON" : "off",
             n.offset_hz, (double)n.baud, (double)n.shift_hz, n.reverse ? " reverse" : "",
             (int)n.afc, (int)n.squelch, (int)n.auto_mode);
    if (n.on) rx_audio_wake();   /* the chain may be asleep with no listener */
}

void rtty_rx_set(bool on, int offset_hz, bool reverse)
{
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.on = on; c.offset_hz = offset_hz; c.reverse = reverse;
    rtty_rx_apply(&c);
}

bool rtty_rx_is_on(void) { return s_cfg.on; }

void rtty_rx_status(rtty_rx_status_t *st)
{
    portENTER_CRITICAL(&s_mux);
    *st = s_st;
    portEXIT_CRITICAL(&s_mux);
    rtty_rx_get_cfg(&st->cfg);
    st->sq_open_lvl = RTTY_SQ_OPEN;
}

size_t rtty_rx_text_tail(char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t w = s_text_w;
    uint32_t held = w - s_text_floor;
    size_t len = held < TEXT_RING ? held : TEXT_RING;
    if (len > n - 1) len = n - 1;
    for (size_t i = 0; i < len; i++) buf[i] = s_text[(w - len + i) & (TEXT_RING - 1)];
    portEXIT_CRITICAL(&s_mux);
    buf[len] = 0;
    return len;
}

size_t rtty_rx_text_read(uint32_t *cursor, char *buf, size_t n)
{
    if (!cursor || !buf || n == 0) return 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t w = s_text_w, c = *cursor;
    if ((int32_t)(c - s_text_floor) < 0) c = s_text_floor;
    if (w - c > TEXT_RING) c = w - TEXT_RING;
    size_t len = w - c;
    if (len > n) len = n;
    for (size_t i = 0; i < len; i++) buf[i] = s_text[(c + i) & (TEXT_RING - 1)];
    *cursor = c + (uint32_t)len;
    portEXIT_CRITICAL(&s_mux);
    return len;
}

static volatile uint32_t s_clear_gen;

void rtty_rx_text_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_text_floor = s_text_w;
    portEXIT_CRITICAL(&s_mux);
    s_clear_gen++;
}

uint32_t rtty_rx_text_clear_gen(void) { return s_clear_gen; }

uint32_t rtty_rx_text_count(void) { return s_text_w; }

static void flush_line(void)
{
    if (s_line_n == 0) return;
    s_line[s_line_n] = 0;
    ESP_LOGI(TAG, "rx: %s", s_line);
    s_line_n = 0;
}

static void on_char(const rtty_char_t *c, void *user)
{
    (void)user;
    int ch = rtty_baudot_decode(&s_bd, c->code);
    if (ch == 0) return;
    if (ch == '\a') ch = '*';
    portENTER_CRITICAL(&s_mux);
    s_text[s_text_w & (TEXT_RING - 1)] = (char)ch;
    s_text_w++;
    s_st.chars++;
    portEXIT_CRITICAL(&s_mux);
    if (ch == '\r') return;
    if (ch == '\n') { flush_line(); return; }
    s_line[s_line_n++] = (char)ch;
    if (s_line_n >= LOG_LINE) flush_line();
}

static bool start(float fs_hz, int nco_hz)
{
    if (!s_dem) {
        s_dem = heap_caps_calloc(1, sizeof(*s_dem), MALLOC_CAP_SPIRAM);
        if (!s_dem) { ESP_LOGE(TAG, "no PSRAM for the demodulator (%u B)", (unsigned)sizeof(*s_dem)); return false; }
    }
    rtty_demod_cfg_t cfg = {
        .fs_hz = fs_hz, .baud = s_run_cfg.baud, .shift_hz = s_run_cfg.shift_hz,
        /* the tone centre's RF offset, moved to where NCO1 put baseband 0 */
        .centre_hz = (float)(s_run_cfg.offset_hz - nco_hz),
        .reverse = s_run_cfg.reverse,
        .squelch = s_run_cfg.squelch ? RTTY_SQ_OPEN : 0.0f,
        .afc_hz = s_run_cfg.afc ? RTTY_RX_AFC_HZ : 0.0f,
    };
    if (!rtty_demod_init(s_dem, &cfg)) {
        ESP_LOGE(TAG, "demod init refused fs %.0f baud %.2f", (double)fs_hz, (double)cfg.baud);
        return false;
    }
    rtty_baudot_init(&s_bd, true);
    s_line_n = 0;
    if (s_run_cfg.auto_mode || s_observe) {
        if (!s_auto) s_auto = heap_caps_calloc(1, sizeof(*s_auto), MALLOC_CAP_SPIRAM);
        if (s_auto) rtty_auto_init(s_auto, fs_hz, cfg.centre_hz);
        else ESP_LOGE(TAG, "no PSRAM for auto (%u B) - auto off this run", (unsigned)sizeof(*s_auto));
        s_auto_same = 0;
    }
    ESP_LOGI(TAG, "demod at %.0f Hz, baseband centre %+.0f Hz (offset %d, nco %d), %u B",
             (double)fs_hz, (double)cfg.centre_hz, s_run_cfg.offset_hz, nco_hz, (unsigned)sizeof(*s_dem));
    return true;
}

void rtty_rx_feed(const float *re, const float *im, int n, float fs_hz, int nco_hz)
{
    uint32_t seq = s_want_seq;
    if (seq != s_have_seq) rtty_rx_get_cfg(&s_run_cfg);
    if (!s_run_cfg.on) {
        s_have_seq = seq;
        if (s_running) { flush_line(); s_running = false; ESP_LOGI(TAG, "stopped"); }
        return;
    }
    /* (Re)start on new settings, a new rate (decimation follows the mode) or
     * a retune of rx_audio's first mix (mode or CW offset changed). */
    if (!s_running || seq != s_have_seq || fs_hz != s_have_fs || nco_hz != s_have_nco) {
        s_have_seq = seq; s_have_fs = fs_hz; s_have_nco = nco_hz;
        s_running = start(fs_hz, nco_hz);
        if (!s_running) {
            portENTER_CRITICAL(&s_cfg_mux);
            s_cfg.on = false;
            portEXIT_CRITICAL(&s_cfg_mux);
            return;
        }
        portENTER_CRITICAL(&s_mux);
        s_st.chars = 0;
        portEXIT_CRITICAL(&s_mux);
    }
    /* R008: what the demodulator costs on core 1 (rx_audio's core). One
     * 10 s window per log line: busy us per wall second = share of a core,
     * and the worst single call, which is what eats into the 21.3 ms frame
     * budget. The acquisition DFT runs every 0.5 s, so max sees it. */
    static int64_t  s_cpu_win_us;
    static uint64_t s_cpu_busy_us;
    static uint32_t s_cpu_max_us, s_cpu_calls, s_cpu_smp;
    int64_t t_a = esp_timer_get_time();
    rtty_demod_feed(s_dem, re, im, n, on_char, NULL);
    const bool run_auto = (s_run_cfg.auto_mode || s_observe) && s_auto;
    if (s_iq_run && s_iq) {            /* development recorder: the exact input */
        uint32_t k = s_iq_n;
        for (int i = 0; i < n && k < s_iq_cap; i++, k++) { s_iq[2 * k] = re[i]; s_iq[2 * k + 1] = im ? im[i] : 0.0f; }
        s_iq_fs = fs_hz; s_iq_n = k;
        if (k >= s_iq_cap) { s_iq_run = false; ESP_LOGI(TAG, "iqcap: done, %u pairs", (unsigned)k); }
    }
    if (run_auto) rtty_auto_feed(s_auto, re, im, n);   /* inside the cpu window on purpose */
    int64_t t_b = esp_timer_get_time();
    uint32_t d_us = (uint32_t)(t_b - t_a);
    s_cpu_busy_us += d_us;
    if (d_us > s_cpu_max_us) s_cpu_max_us = d_us;
    s_cpu_calls++;
    s_cpu_smp += (uint32_t)n;
    if (s_cpu_win_us == 0) s_cpu_win_us = t_a;
    else if (t_b - s_cpu_win_us >= 10000000) {
        int64_t win = t_b - s_cpu_win_us;
        uint32_t permille = (uint32_t)(s_cpu_busy_us * 1000ULL / (uint64_t)win);
        /* stack left = rx_audio's (this runs in it): R028's first build put
         * 2 KB of FFT arrays on this 4 KB stack and overflowed it. */
        ESP_LOGI(TAG, "cpu: %lu us/s = %lu.%lu%% of core 1, call avg %lu max %lu us, %lu calls, %lu smp/s at %.0f Hz, stack left %u B%s",
                 (unsigned long)(s_cpu_busy_us * 1000000ULL / (uint64_t)win),
                 (unsigned long)(permille / 10), (unsigned long)(permille % 10),
                 (unsigned long)(s_cpu_calls ? s_cpu_busy_us / s_cpu_calls : 0),
                 (unsigned long)s_cpu_max_us, (unsigned long)s_cpu_calls,
                 (unsigned long)((uint64_t)s_cpu_smp * 1000000ULL / (uint64_t)win), (double)fs_hz,
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                 run_auto ? ", auto on" : "");
        s_cpu_win_us = t_b; s_cpu_busy_us = 0; s_cpu_max_us = 0; s_cpu_calls = 0; s_cpu_smp = 0;
    }

    portENTER_CRITICAL(&s_mux);
    s_st.sig_lvl = s_dem->sig_lvl;
    s_st.present = s_dem->present;
    s_st.mark_lvl = s_dem->mark_lvl;
    s_st.space_lvl = s_dem->space_lvl;
    s_st.mark_db   = s_dem->mark_snr_db;
    s_st.space_db  = s_dem->space_snr_db;
    s_st.afc_hz = s_dem->afc_off;
    s_st.afc_held = s_dem->acq_held;
    s_st.acq_jumps = s_dem->n_acq;
    s_st.fs_hz = fs_hz;
    s_st.dem_chars = s_dem->n_chars; s_st.dem_framing = s_dem->n_framing;
    s_st.dem_false_start = s_dem->n_false_start; s_st.dem_unlock = s_dem->n_unlock;
    s_st.dem_grid = s_dem->n_grid;
    s_st.auto_ok = run_auto && s_auto->shift_ok && s_auto->baud_ok;
    s_st.auto_run = run_auto;
    if (run_auto) {
        s_st.auto_shift_ok = s_auto->shift_ok; s_st.auto_baud_ok = s_auto->baud_ok;
        s_st.auto_pair_db = s_auto->pair_db; s_st.auto_pair_centre = s_auto->pair_centre;
        s_st.auto_runs = s_auto->ntrans;
        for (int k = 0; k < 4 && k < RTTY_AUTO_NBAUD; k++) s_st.auto_score[k] = s_auto->score[k];
        memcpy(s_st.auto_hist, s_auto->hist, sizeof s_st.auto_hist);
        if (s_auto->shift_ok) s_st.auto_shift = s_auto->shift;
    }
    if (s_st.auto_ok) {
        s_st.auto_baud = s_auto->baud;
        s_st.auto_shift = s_auto->shift;
        /* The baseband is RF-oriented: mark is the HIGHER frequency unless
         * the station is reversed. mark_low = the station is reversed. */
        s_st.auto_reverse = s_auto->mark_low;
    }
    portEXIT_CRITICAL(&s_mux);

    /* R028: once a second, a verdict that differs from what runs must hold
     * RTTY_AUTO_HOLD_S seconds before it is applied. Not while keyed: our
     * own over is not a station to follow. */
    if (run_auto && s_run_cfg.auto_mode && t_b >= s_auto_next_us) {
        s_auto_next_us = t_b + 1000000;
        bool ok = s_st.auto_ok;
        float b = s_st.auto_baud, sh = s_st.auto_shift; bool rv = s_st.auto_reverse;
        bool differs = ok && (fabsf(b - s_run_cfg.baud) > 0.01f || fabsf(sh - s_run_cfg.shift_hz) > 0.5f ||
                              rv != s_run_cfg.reverse);
        if (!differs || rtty_tx_keyed()) s_auto_same = 0;
        else if (s_auto_same > 0 && b == s_auto_pb && sh == s_auto_ps && rv == s_auto_pr) s_auto_same++;
        else { s_auto_same = 1; s_auto_pb = b; s_auto_ps = sh; s_auto_pr = rv; }
        if (s_auto_same >= RTTY_AUTO_HOLD_S) {
            rtty_rx_cfg_t c;
            rtty_rx_get_cfg(&c);
            ESP_LOGI(TAG, "auto: %.2f Bd %.0f Hz%s -> %.2f Bd %.0f Hz%s (held %d s)",
                     (double)c.baud, (double)c.shift_hz, c.reverse ? " rev" : "",
                     (double)b, (double)sh, rv ? " rev" : "", s_auto_same);
            c.baud = b; c.shift_hz = sh; c.reverse = rv;
            s_auto_same = 0;
            rtty_rx_apply(&c);         /* restarts demod + estimator on the next frame */
        }
    }
}
