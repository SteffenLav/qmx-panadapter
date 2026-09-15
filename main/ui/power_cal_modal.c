// Calibrate Power modal - full-screen overlay that sweeps QMX Max. PA
// voltage through 5 fixed points and measures real RF output (PC;) at each,
// on a dummy load. Answers "which W/dBm can I actually reach on this band"
// instead of trusting Hans's own "6.0 V is about 1 W" rule of thumb, which
// CLAUDE.md already records as an unverified generalisation.
//
// ⛔ FIRST VERSION reused QMX SWR Tune mode (MD8;, the same mechanism
// tune_modal.c uses for Antenna Tune) to key the test carrier, on the
// reasoning that it was already a safe, proven path. WRONG - confirmed by
// the operator on his own bench: Tune mode transmits at a SCALED-DOWN power
// level, unrelated to Max. PA voltage, so every reading was low by roughly
// the same factor regardless of how long the measurement waited for the
// value to settle (a real bug, fixed alongside this, kept because it's
// correct on its own - see the PC_MEASURE stability loop below).
//
// ⛔ SECOND VERSION switched to CW mode and keyed a bare TX;, reasoning that
// CW needs no audio to produce output. ALSO WRONG - hardware-confirmed: a
// bare TX; with nothing else produced no RF at all, and a naive "3 readings
// in a row agree" stability check was fooled by three identical ZEROS,
// finishing every step in under a second with 0.00 W across the board.
//
// This version keys a real carrier the way WSPR/FT8 actually do: force
// DiGi mode, TX;, then a TA<freq>; audio tone RESENT on roughly their own
// 160 ms symbol cadence for as long as the carrier should stay up (a single
// TA; is not enough - the QMX drops the envelope if it isn't refreshed).
// TA0; drops the tone, then RX;, then the prior mode is restored. Same
// TX;/TA;/RX; primitives an FT8/WSPR burst is built from, just without
// their symbol-accurate timing or tone-hopping - this only ever needs one
// steady tone. Needs no particular firmware version (TX;/TA;/RX;/DiGi have
// existed since 1_03) - the drawer button still gates on 1_04+ only because
// it shares its section with Antenna Tune; lifting that is possible later,
// just not done here.
//
// Every PA-voltage write goes through cat_request_pa_voltage_x10(), which
// already does the full MM-write -> MU; -> Q9 reassert -> read-back dance
// (see cat.c's own comment on that function) - this file never duplicates
// that CAT sequence, only waits for cat_get_pa_voltage_x10() to confirm it.

#include "power_cal_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "cat.h"
#include "ft8_tx.h"
#include "wspr_rx.h"
#include "wspr_tx.h"    // WSPR_STD_DBM[] - the known reference levels the results table is indexed by
#include "adif/adif_log.h"
#include "storage/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "power_cal";

// Bounded waits - never trust a CAT write to land, always give up and move
// on (or restore-and-close) rather than hang the sweep or the window.
#define PWRCAL_WRITE_TIMEOUT_MS   3000  // MM write + MU; + Q9 reassert + read-back, and separately mode-change confirm
#define PWRCAL_MEASURE_MIN_MS      500  // never call it "settled" before at least this long - see the
                                         // header comment: a bare TX; with no tone reads a flat 0.00 W
                                         // that "stabilises" on the very first samples, which is what a
                                         // fixed 3-in-a-row check alone was fooled by
#define PWRCAL_MEASURE_MAX_MS     6000  // hard cap per step if the reading never settles
#define PWRCAL_STABLE_SAMPLES        3  // consecutive agreeing readings before trusting one
#define PWRCAL_STABLE_TOL_W       0.05f // "agreeing" = within this many watts of the previous sample
#define PWRCAL_TONE_HZ           1500.0f// audio tone fed to DiGi TX - same default WSPR/FT8 use
#define PWRCAL_TONE_RESEND_MS      150  // re-send TA<freq>; at roughly FT8's own 160 ms symbol cadence,
                                         // or the QMX drops the envelope between commands
#define PWRCAL_EXIT_SETTLE_MS      300  // after restoring mode, before the next step's voltage write
// 23 steps x up to ~9.3 s worst case (3 s voltage-confirm + 6 s measure +
// 0.3 s exit) is ~214 s - give real margin over that, not just the old
// 5-step figure. Mirrors tune_modal's TUNE_TIMEOUT_MS idea either way: a
// backstop that should basically never fire in practice.
#define PWRCAL_TOTAL_TIMEOUT_MS 300000

// 1.0-12.0 V in 0.5 V steps - see the PWRCAL_STEPS comment in settings.h for
// why this range and this resolution.
static const uint8_t s_test_voltage_x10[PWRCAL_STEPS] = {
     10,  15,  20,  25,  30,  35,  40,  45,  50,  55,  60,  65,
     70,  75,  80,  85,  90,  95, 100, 105, 110, 115, 120,
};

typedef enum {
    PC_IDLE = 0,
    PC_SET_VOLTAGE,   // writing + confirming this step's Max. PA voltage
    PC_ENTER_DIGI,    // DiGi mode requested, waiting for it to be confirmed before keying
    PC_MEASURE,       // TX; + a repeated TA<freq>; tone, sampling PC; until the reading settles
    PC_EXIT_KEY,      // TA0; + RX; sent, prior mode restored, settling before the next step
    PC_RESTORE,       // writing + confirming the ORIGINAL PA voltage back
    PC_DONE,          // results shown, sweep finished (fully or partially)
} pwrcal_state_t;

static lv_obj_t   *s_modal       = NULL;
static lv_obj_t   *s_panel       = NULL;
static lv_obj_t   *s_status_lbl  = NULL;
static lv_obj_t   *s_results_lbl = NULL;   // left column
static lv_obj_t   *s_results_lbl2 = NULL;  // right column
static lv_obj_t   *s_action_btn  = NULL;
static lv_obj_t   *s_action_lbl  = NULL;
static lv_obj_t   *s_cancel_btn  = NULL;
static lv_timer_t *s_timer       = NULL;

static pwrcal_state_t s_state         = PC_IDLE;
static uint32_t        s_state_enter_ms = 0;
static uint32_t        s_sweep_start_ms = 0;
static int              s_step           = 0;
static uint16_t         s_measured_w_x100[PWRCAL_STEPS];
static float             s_last_pw        = -1.0f;  // most recent valid PC; reading this step
static float              s_prev_pw       = -1.0f;  // the sample before that, for the settle check
static int                s_stable_count  = 0;       // consecutive samples within PWRCAL_STABLE_TOL_W
static uint32_t            s_last_ta_ms   = 0;       // last time TA<freq>; was (re)sent, for the resend cadence
static char              s_prior_mode[8]  = "USB";
static uint16_t          s_orig_pa_x10    = 120;      // restored at the end
static char              s_band[8]        = "";

static void render_results(void);
static void begin_step(int step);
static void advance_or_finish(void);
static void enter_restore(void);

static void enter_state(pwrcal_state_t st)
{
    s_state = st;
    s_state_enter_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t state_elapsed_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000) - s_state_enter_ms;
}

static void status_set_text(const char *s)
{
    if (s_status_lbl) lv_label_set_text(s_status_lbl, s);
}

// ---- results rendering -----------------------------------------------

// Calibration means matching to KNOWN reference levels, not just reporting
// whatever a voltage sweep happened to produce (the operator's own framing).
// The known levels here are WSPR_STD_DBM (wspr_tx.h) - the same standard
// dBm steps the WSPR drawer's "Declared power" dropdown offers - so this
// table answers "what do I set PA voltage to, to actually BE the level I'm
// about to declare", rather than leaving that as a guess the way the
// dropdown always has been.
//
// For each standard dBm value, find the SWEPT point whose measured output
// is closest to that target (10^((dbm-30)/10) W) and report its voltage.
// A gap wider than 3 dB is shown as "--", not a wrong answer dressed up as
// a right one - see the "never fabricate" rule elsewhere in this codebase
// (RST placeholders, chase-reference fallback): a big enough gap means the
// level is not really achievable here, and saying so is more honest than
// naming the nearest thing anyway.
#define PWRCAL_MATCH_MAX_DB 3.0f

// Runtime lookup, independent of the modal (which may never have been
// opened this session): given a band already calibrated by a PAST sweep
// (persisted, not the in-progress s_measured_w_x100 above), find the
// voltage that produced the measured output closest to target_dbm. Exactly
// the same "closest by dB gap, refuse past PWRCAL_MATCH_MAX_DB" rule as
// render_results() above, kept as one rule rather than two so the WSPR
// drawer's "Declared power" dropdown (main/ui/ui.c, power_cal_apply_declared
// there) can never silently disagree with what this modal's own results
// table shows for the same band and dBm.
bool power_cal_voltage_for_dbm(const char *band, int8_t target_dbm, uint16_t *out_v_x10)
{
    uint8_t  v_x10[PWRCAL_STEPS];
    uint16_t w_x100[PWRCAL_STEPS];
    if (!band || !band[0] || !settings_get_pwr_cal_band(band, v_x10, w_x100)) return false;

    float target_w = powf(10.0f, ((float)target_dbm - 30.0f) / 10.0f);
    int   best = -1;
    float best_gap_db = 1e9f;
    for (int i = 0; i < PWRCAL_STEPS; i++) {
        if (w_x100[i] == 0) continue;
        float w = (float)w_x100[i] / 100.0f;
        float gap_db = fabsf(10.0f * log10f(w / target_w));
        if (gap_db < best_gap_db) { best_gap_db = gap_db; best = i; }
    }
    if (best < 0 || best_gap_db > PWRCAL_MATCH_MAX_DB) return false;
    if (out_v_x10) *out_v_x10 = v_x10[best];
    return true;
}

static void render_results(void)
{
    if (!s_results_lbl || !s_results_lbl2) return;
    char left[512], right[512];
    int loff = 0, roff = 0;
    loff += snprintf(left + loff, sizeof(left) - loff, "Band %s\n", s_band[0] ? s_band : "?");
    roff += snprintf(right + roff, sizeof(right) - roff, "\n");

    int half = (WSPR_STD_DBM_N + 1) / 2;
    for (int k = 0; k < WSPR_STD_DBM_N; k++) {
        int dbm = WSPR_STD_DBM[k];
        float target_w = powf(10.0f, ((float)dbm - 30.0f) / 10.0f);

        int best = -1;
        float best_gap_db = 1e9f;
        for (int i = 0; i < PWRCAL_STEPS; i++) {
            if (s_measured_w_x100[i] == 0) continue;
            float w = (float)s_measured_w_x100[i] / 100.0f;
            float gap_db = fabsf(10.0f * log10f(w / target_w));
            if (gap_db < best_gap_db) { best_gap_db = gap_db; best = i; }
        }

        char line[48];
        if (best >= 0 && best_gap_db <= PWRCAL_MATCH_MAX_DB) {
            unsigned vx = s_test_voltage_x10[best];
            snprintf(line, sizeof(line), "%3d dBm  %2u.%uV\n", dbm, vx / 10, vx % 10);
        } else {
            snprintf(line, sizeof(line), "%3d dBm    --\n", dbm);
        }

        if (k < half) loff += snprintf(left + loff, sizeof(left) - loff, "%s", line);
        else          roff += snprintf(right + roff, sizeof(right) - roff, "%s", line);
    }
    lv_label_set_text(s_results_lbl, left);
    lv_label_set_text(s_results_lbl2, right);
}

// ---- sweep state machine ----------------------------------------------

static void begin_step(int step)
{
    s_step = step;
    s_last_pw = -1.0f;
    cat_request_pa_voltage_x10(s_test_voltage_x10[step]);
    enter_state(PC_SET_VOLTAGE);
    char buf[64];
    snprintf(buf, sizeof(buf), "Step %d of %d - setting %u.%uV...",
             step + 1, PWRCAL_STEPS, s_test_voltage_x10[step] / 10, s_test_voltage_x10[step] % 10);
    status_set_text(buf);
}

static void advance_or_finish(void)
{
    int next = s_step + 1;
    if (next < PWRCAL_STEPS) {
        begin_step(next);
    } else {
        enter_restore();
    }
}

static void enter_restore(void)
{
    cat_request_pa_voltage_x10(s_orig_pa_x10);
    enter_state(PC_RESTORE);
    status_set_text("Restoring original PA voltage...");
    if (s_cancel_btn) lv_obj_add_state(s_cancel_btn, LV_STATE_DISABLED);
}

static void finish_done(void)
{
    // Persist whatever was measured, even a partial run (a cancelled sweep
    // still leaves useful, clearly-marked-partial data - "--" rows are
    // honest, not a reason to throw the good ones away).
    settings_set_pwr_cal_band(s_band, s_test_voltage_x10, s_measured_w_x100);
    render_results();
    enter_state(PC_DONE);
    status_set_text("Done.");
    if (s_action_lbl) lv_label_set_text(s_action_lbl, "Start Calibration");
    if (s_action_btn) lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    if (s_cancel_btn) lv_obj_clear_state(s_cancel_btn, LV_STATE_DISABLED);
}

// Stop transmitting RIGHT NOW if we currently are, then head for a
// confirmed voltage restore before the window is allowed to close. Used by
// both the total-timeout backstop and Cancel - never just hide the window
// out from under a radio we have not put back the way we found it.
static void abort_to_restore(const char *toast)
{
    if (s_state == PC_MEASURE) {
        // Actually keyed (PC_ENTER_DIGI hasn't sent TX; yet) - drop the tone
        // and unkey FIRST, then change mode away from DiGi. Wrong order
        // keys a carrier in whatever the prior mode was for however long
        // the mode change takes to land.
        cat_send_raw_cmd("TA0;");
        cat_send_raw_cmd("RX;");
        cat_tune_poll_set_active(false);
    }
    if (s_state == PC_ENTER_DIGI || s_state == PC_MEASURE) {
        cat_request_mode(s_prior_mode);
    }
    enter_restore();
    if (toast) ui_toast(toast);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_state == PC_IDLE || s_state == PC_DONE) return;

    if ((uint32_t)(esp_timer_get_time() / 1000) - s_sweep_start_ms > PWRCAL_TOTAL_TIMEOUT_MS
        && s_state != PC_RESTORE) {
        abort_to_restore(LV_SYMBOL_WARNING " Calibration timed out - restoring and stopping");
        return;
    }

    switch (s_state) {
    case PC_SET_VOLTAGE: {
        int16_t cur = cat_get_pa_voltage_x10();
        if (cur == (int16_t)s_test_voltage_x10[s_step]) {
            cat_request_mode("DiGi");
            enter_state(PC_ENTER_DIGI);
        } else if (state_elapsed_ms() > PWRCAL_WRITE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "step %d: PA voltage %u.%uV never confirmed (radio reads %d) - skipping",
                     s_step, s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10, cur);
            s_measured_w_x100[s_step] = 0;
            advance_or_finish();
        }
        break;
    }
    case PC_ENTER_DIGI: {
        // Wait for the radio to actually CONFIRM DiGi before keying - not a
        // fixed delay. Keying TX; while the mode change is still in flight
        // risks a carrier in the wrong mode.
        const char *m = cat_get_mode_str();
        if (m && strcmp(m, "DiGi") == 0) {
            // TX; alone produces no RF here (found on hardware: every step
            // "settled" on a flat 0.00 W in under a second) - DiGi is
            // audio-modulated, so it needs the same TA<freq>; tone WSPR/FT8
            // feed it, resent on roughly their own 160 ms cadence for as
            // long as the carrier should stay up.
            cat_send_raw_cmd("TX;");
            char ta[24];
            snprintf(ta, sizeof(ta), "TA%.0f;", (double)PWRCAL_TONE_HZ);
            cat_send_raw_cmd(ta);
            s_last_ta_ms = (uint32_t)(esp_timer_get_time() / 1000);
            cat_tune_poll_set_active(true);   // PC;/SW; poll rotation, same mechanism Tune used
            s_prev_pw = -1.0f;
            s_stable_count = 0;
            enter_state(PC_MEASURE);
        } else if (state_elapsed_ms() > PWRCAL_WRITE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "step %d: DiGi mode never confirmed (radio reads '%s') - skipping",
                     s_step, m ? m : "?");
            s_measured_w_x100[s_step] = 0;
            advance_or_finish();
        }
        break;
    }
    case PC_MEASURE: {
        // Keep the tone alive - the QMX drops the envelope if TA<freq>; isn't
        // refreshed, same as an FT8/WSPR symbol.
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_ta_ms >= PWRCAL_TONE_RESEND_MS) {
            char ta[24];
            snprintf(ta, sizeof(ta), "TA%.0f;", (double)PWRCAL_TONE_HZ);
            cat_send_raw_cmd(ta);
            s_last_ta_ms = now_ms;
        }
        // Wait for the reading to actually SETTLE, not just for a fixed time
        // to pass - the PA rail takes a moment to reach the newly-commanded
        // voltage after MU; reloads the config, and a fixed short window was
        // catching it mid-ramp. Same idea as the MM-write confirm-and-retry
        // pattern elsewhere in this codebase: trust a value once it holds.
        // ⚠ PWRCAL_MEASURE_MIN_MS is load-bearing: a bare 3-in-a-row check
        // was fooled by a flat run of zeros right at key-up, before the
        // tone had even been sent once - see the case comment above.
        float pw = -1.0f, swr = -1.0f;
        cat_pwr_swr_async_read(&pw, &swr);
        if (pw >= 0.0f) {
            if (s_prev_pw >= 0.0f && fabsf(pw - s_prev_pw) <= PWRCAL_STABLE_TOL_W) {
                s_stable_count++;
            } else {
                s_stable_count = 0;
            }
            s_prev_pw = pw;
            s_last_pw = pw;
        }
        char buf[80];
        if (s_last_pw >= 0.0f) {
            snprintf(buf, sizeof(buf), "Step %d of %d - %u.%uV - %.2fW, settling (%d/%d)...",
                     s_step + 1, PWRCAL_STEPS, s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10,
                     (double)s_last_pw, s_stable_count, PWRCAL_STABLE_SAMPLES);
        } else {
            snprintf(buf, sizeof(buf), "Step %d of %d - %u.%uV - waiting for a reading...",
                     s_step + 1, PWRCAL_STEPS, s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10);
        }
        status_set_text(buf);
        bool min_time_met = state_elapsed_ms() >= PWRCAL_MEASURE_MIN_MS;
        if ((min_time_met && s_stable_count >= PWRCAL_STABLE_SAMPLES) ||
            state_elapsed_ms() > PWRCAL_MEASURE_MAX_MS) {
            cat_send_raw_cmd("TA0;");   // drop the envelope
            cat_send_raw_cmd("RX;");    // unkey, then change mode away from DiGi
            cat_tune_poll_set_active(false);
            cat_request_mode(s_prior_mode);
            s_measured_w_x100[s_step] = (s_last_pw >= 0.0f) ? (uint16_t)(s_last_pw * 100.0f + 0.5f) : 0;
            ESP_LOGI(TAG, "step %d: %u.%uV -> %.2f W (%s, %d stable samples)", s_step,
                     s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10, (double)s_last_pw,
                     (min_time_met && s_stable_count >= PWRCAL_STABLE_SAMPLES) ? "settled" : "gave up waiting",
                     s_stable_count);
            enter_state(PC_EXIT_KEY);
        }
        break;
    }
    case PC_EXIT_KEY:
        if (state_elapsed_ms() > PWRCAL_EXIT_SETTLE_MS) advance_or_finish();
        break;
    case PC_RESTORE: {
        int16_t cur = cat_get_pa_voltage_x10();
        if (cur == (int16_t)s_orig_pa_x10) {
            finish_done();
        } else if (state_elapsed_ms() > PWRCAL_WRITE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "original PA voltage %u.%uV never confirmed restored (radio reads %d)",
                     s_orig_pa_x10 / 10, s_orig_pa_x10 % 10, cur);
            ui_toast(LV_SYMBOL_WARNING " Could not confirm PA voltage restored - check the radio's Protection menu");
            finish_done();
        }
        break;
    }
    default:
        break;
    }
}

// ---- buttons ------------------------------------------------------------

static void start_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != PC_IDLE && s_state != PC_DONE) return;  // already running

    // No firmware-version gate here: TX;/RX;/CW have existed since 1_03.
    // The drawer BUTTON is still 1_04+-gated only because it shares its
    // section with Antenna Tune - see the file header comment.
    if (cat_user_pause_active()) {
        ui_toast("Radio released - take it back before calibrating");
        return;
    }
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) {
        ui_toast("FT8 is transmitting - try again once it's idle");
        return;
    }
    if (wspr_rx_running()) {
        ui_toast("Stop WSPR first - calibration needs the radio to itself");
        return;
    }

    const char *cur_mode = cat_get_mode_str();
    strncpy(s_prior_mode, (cur_mode && cur_mode[0]) ? cur_mode : "USB", sizeof(s_prior_mode) - 1);
    s_prior_mode[sizeof(s_prior_mode) - 1] = '\0';

    int16_t pa = cat_get_pa_voltage_x10();
    if (pa >= 0) {
        s_orig_pa_x10 = (uint16_t)pa;
    } else {
        s_orig_pa_x10 = 120;  // full power - the QMX's own default/max, safest guess if truly unknown
        ui_toast("Could not read the current PA voltage - will restore to 12.0V when done");
    }

    strncpy(s_band, adif_log_band_for_freq(cat_get_frequency()), sizeof(s_band) - 1);
    s_band[sizeof(s_band) - 1] = '\0';

    memset(s_measured_w_x100, 0, sizeof(s_measured_w_x100));
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_action_lbl) lv_label_set_text(s_action_lbl, "Running...");
    if (s_action_btn) lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(0xB03020), 0);
    if (s_results_lbl) lv_label_set_text(s_results_lbl, "");
    if (s_results_lbl2) lv_label_set_text(s_results_lbl2, "");

    begin_step(0);
    if (!s_timer) s_timer = lv_timer_create(timer_cb, 150, NULL);
    ESP_LOGI(TAG, "calibration started, band=%s prior_mode=%s orig_pa=%u.%uV",
             s_band, s_prior_mode, s_orig_pa_x10 / 10, s_orig_pa_x10 % 10);
}

// Cancel always leaves the radio in a known-safe, confirmed-voltage state
// before closing - same discipline as tune_modal.c's cancel, extended with
// one more step because this modal also changes a value the radio persists
// (Max. PA voltage), not just a transient mode.
static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_state == PC_IDLE || s_state == PC_DONE) {
        if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_state == PC_RESTORE) return;  // already on the way out, disabled below anyway
    abort_to_restore(NULL);  // no toast - the status label already says "Restoring..."
}

// PC_RESTORE resolving while the window is still open (normal completion,
// or after Cancel/timeout) is what actually allows the window to close -
// close_after_restore mirrors that by re-checking state once PC_DONE lands.
// Simplest correct way to do this without a second timer: the Close/Start
// button and the header both already reflect PC_DONE, so once the operator
// sees results (or a plain "Done." with an empty table from an early
// cancel) they close it themselves via Cancel, which now hits the
// IDLE/DONE branch above and hides immediately.

static void modal_build(void)
{
    if (s_modal) return;
    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(s_modal);
    // 560 -> 680: a full 5-row result table (2 header lines + 5 data lines at
    // montserrat_24) ran to ~y=524 inside the padded content box, which the
    // original 560-tall panel couldn't clear before the bottom-aligned Cancel
    // button - seen on hardware, the last two rows drew UNDER Cancel. 680 is
    // the tallest this can go and still centre with real margin inside the
    // 720 px screen height; the rest of the fix is tightening the layout
    // above the results table so there is room to spare, not just enough.
    lv_obj_set_size(s_panel, 680, 680);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Calibrate Power");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *warn = lv_label_create(s_panel);
    lv_label_set_text(warn, LV_SYMBOL_WARNING " Connect a DUMMY LOAD, not the antenna.\n"
                             "Keys a brief full carrier at 5 voltage steps.");
    lv_obj_set_style_text_color(warn, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 60);

    s_status_lbl = lv_label_create(s_panel);
    lv_label_set_text(s_status_lbl, "Ready.");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 130);

    s_action_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_action_btn, 360, 72);
    lv_obj_align(s_action_btn, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(s_action_btn, 8, 0);
    lv_obj_add_event_cb(s_action_btn, start_btn_cb, LV_EVENT_CLICKED, NULL);
    s_action_lbl = lv_label_create(s_action_btn);
    lv_label_set_text(s_action_lbl, "Start Calibration");
    lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_action_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_action_lbl);

    // ⚠ An earlier version here blamed a scrollable container for a boot
    // crash and reverted to a flat, unbounded-height label. Retracted: the
    // real cause (found afterwards) was qmx_settings_t growing large enough
    // to overflow several OTHER tasks' stacks - nothing to do with this
    // object. Moot either way now: the table is indexed by WSPR_STD_DBM (12
    // known levels), not the raw 23-point sweep, so two 6-row columns fit
    // with room to spare and no scrolling is needed.
    s_results_lbl = lv_label_create(s_panel);
    lv_label_set_text(s_results_lbl, "");
    lv_obj_set_style_text_color(s_results_lbl, lv_color_hex(0xC0D0E0), 0);
    lv_obj_set_style_text_font(s_results_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(s_results_lbl, LV_ALIGN_TOP_LEFT, 0, 262);

    s_results_lbl2 = lv_label_create(s_panel);
    lv_label_set_text(s_results_lbl2, "");
    lv_obj_set_style_text_color(s_results_lbl2, lv_color_hex(0xC0D0E0), 0);
    lv_obj_set_style_text_font(s_results_lbl2, &lv_font_montserrat_24, 0);
    lv_obj_align(s_results_lbl2, LV_ALIGN_TOP_LEFT, 320, 262);

    s_cancel_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_cancel_btn, 240, 72);
    lv_obj_align(s_cancel_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(s_cancel_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(s_cancel_btn, 2, 0);
    lv_obj_set_style_radius(s_cancel_btn, 8, 0);
    lv_obj_add_event_cb(s_cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(s_cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    // Esc only, same reasoning as tune_modal.c: this keys a carrier, and the
    // only key worth having live is the one that stops it.
    ui_kbd_set_buttons(NULL, s_cancel_btn);

    ESP_LOGI(TAG, "Calibrate Power modal built");
}

void power_cal_modal_init(void)
{
    modal_build();
}

void power_cal_modal_show(void)
{
    modal_build();
    // Pre-fill from any prior calibration of the CURRENT band, so opening
    // the modal is informative even before the operator taps Start.
    strncpy(s_band, adif_log_band_for_freq(cat_get_frequency()), sizeof(s_band) - 1);
    s_band[sizeof(s_band) - 1] = '\0';
    uint8_t v_x10[PWRCAL_STEPS];
    if (s_state == PC_IDLE && settings_get_pwr_cal_band(s_band, v_x10, s_measured_w_x100)) {
        render_results();
        status_set_text("Previously calibrated - Start to re-measure.");
    } else if (s_state == PC_IDLE) {
        status_set_text("Ready.");
        if (s_results_lbl) lv_label_set_text(s_results_lbl, "");
        if (s_results_lbl2) lv_label_set_text(s_results_lbl2, "");
    }
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ESP_LOGI(TAG, "Calibrate Power modal shown");
}
