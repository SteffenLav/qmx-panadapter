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
// 45 steps (was 23 - see PWRCAL_STEPS in settings.h) x up to ~9.3 s worst
// case (3 s voltage-confirm + 6 s measure + 0.3 s exit) is ~419 s - give
// real margin over that, not just the per-step arithmetic. Mirrors
// tune_modal's TUNE_TIMEOUT_MS idea either way: a backstop that should
// basically never fire in practice.
#define PWRCAL_TOTAL_TIMEOUT_MS 480000

// ---- how far the sweep actually goes (Bruce N9JCV, 2026-09-17) ------------
// The sweep used to run the whole fixed 1.0-12.0 V table on every radio. On a
// 9 V QMX fed from an 8 V supply that is minutes of keying the PA at settings
// it physically cannot reach, and it writes 12.0 V to a radio whose operator
// deliberately set a lower ceiling. Two independent limits now stop it.
//
// 1. THE OPERATOR'S OWN CEILING. Never sweep past the radio's Max. PA voltage.
//    It is shown on the modal before Start, because the value is whatever the
//    radio currently holds and that is not always what the operator intends -
//    a WSPR session legitimately drives it down to 2.3 V, and a bench session
//    once left it at 7.5 V. Capping silently at a leftover would produce a
//    near-empty calibration and say nothing. Showing the number lets them see
//    it first.
//
// 2. A MEASURED PLATEAU. Above the supply the output stops rising, so further
//    steps re-measure the same watts. Detected rather than modelled - the same
//    reason the classifier reads PC; instead of assuming watts from volts, and
//    the reason this works on a 9 V build whose PA transformer is wound
//    differently (RWTST vs WTST) without knowing which radio it is talking to.
/* Below this, the pre-Start line warns instead of stating the range - see
 * idle_status_refresh(). 10.0 V is chosen to sit above a 9 V QMX's legitimate
 * ceiling and well below the 12.0 V a full-power 12 V build calibrates to, so
 * it catches "left turned down" without nagging a correctly-set 9 V radio. */
#define PWRCAL_LOW_CEILING_X10      100

#define PWRCAL_PLATEAU_SAMPLES        3   // consecutive steps that fail to rise
#define PWRCAL_PLATEAU_ARM_W_X100    30   // only look once 0.30 W has been seen
#define PWRCAL_PLATEAU_MIN_V_X10     40   // and never stop below 4.0 V
#define PWRCAL_PLATEAU_TOL_PCT        3   // "not rising" = within this of the best so far
#define PWRCAL_PLATEAU_TOL_MIN_X100   5   // 0.05 W floor - PC; resolution is coarse

// Panel geometry. Every label inside the panel must be sized against the
// CONTENT box, so it is derived here once instead of being written out at each
// call site - that is what stops a label being wider than the panel that
// clips it.
#define PWRCAL_PANEL_W          680
#define PWRCAL_PANEL_PAD         24
#define PWRCAL_PANEL_CONTENT_W  (PWRCAL_PANEL_W - 2 * PWRCAL_PANEL_PAD)

// 1.0-12.0 V, alternating +0.3/+0.2 V (average 0.25 V) - see the
// PWRCAL_STEPS comment in settings.h for why this range, this resolution,
// and why it is not a literal 0.25 V step (the radio only accepts 0.1 V
// granularity).
static const uint8_t s_test_voltage_x10[PWRCAL_STEPS] = {
     10,  13,  15,  18,  20,  23,  25,  28,  30,  33,  35,  38,
     40,  43,  45,  48,  50,  53,  55,  58,  60,  63,  65,  68,
     70,  73,  75,  78,  80,  83,  85,  88,  90,  93,  95,  98,
    100, 103, 105, 108, 110, 113, 115, 118, 120,
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
static lv_obj_t   *s_cancel_lbl  = NULL;   // "Cancel" while running/idle, "Done" once results are in
static lv_timer_t *s_timer       = NULL;

static pwrcal_state_t s_state         = PC_IDLE;
/* True while THIS modal stopped WSPR receive to have the radio to itself -
 * set only in start_btn_cb() right before wspr_rx_stop(), cleared once
 * wspr_rx_start() has been called to resume it. Operator, 2026-09-15: a
 * "Stop WSPR first" refusal with no way to do that from here made
 * calibrating awkward enough to report - swipe out to Panadapter, stop it,
 * come back, calibrate, then swipe back to WSPR. This modal now does the
 * stop/resume itself. */
static bool             s_wspr_was_running = false;
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
static int               s_last_step      = PWRCAL_STEPS - 1; // highest step this sweep will reach
static uint16_t          s_max_w_x100     = 0;        // best output seen this sweep
static int               s_plateau_run    = 0;        // consecutive steps that did not beat it
static bool              s_stopped_early  = false;    // plateau ended the sweep, not the table
static lv_timer_t       *s_idle_timer     = NULL;     // polls the ceiling for the pre-Start line
static int               s_idle_ticks     = 0;        // reset when that timer is created
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

/* The status line is white by default; a warning takes the same amber the rest
 * of this firmware uses. Every caller that sets the text back to an ordinary
 * message calls status_set_plain() first, so a warning cannot stick. */
static void status_set_warn(bool warn)
{
    if (s_status_lbl)
        lv_obj_set_style_text_color(s_status_lbl,
            lv_color_hex(warn ? 0xFFA040 : 0xFFFFFF), 0);
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
// ⛔ CLASSIFY FROM THE MEASUREMENT, NOT TOWARD A TARGET (2026-09-16).
//
// The original rule picked, for each standard dBm's NOMINAL wattage, the
// swept point within PWRCAL_MATCH_MAX_DB (3 dB) of it - a tolerance window
// around a target. That is backwards for a "declare what you actually do"
// feature, and it produced exactly the inconsistency the operator caught:
// a radio calibrated to 3.6 W max (35.6 dBm) fell within 3 dB of BOTH 33 dBm
// (2 W nominal, 2.6 dB away) and 37 dBm (5 W nominal, 1.4 dB away) - so
// "37 dBm" was offered and labelled with its own real 3.6 W, self-
// consistent only by accident of which target happened to be nearest.
// "the whole idea here is to be as precise as can be - this is research...
// not a candy store."
//
// power_cal_dbm_for_watts() below is the other direction: given what the radio
// REALLY measured, which standard step is it CLOSEST to - the same
// nearest-neighbour classification wspr_tx_advised_dbm() (wspr_tx.c) already
// uses to turn a burst's own PC; reading into a declaration. There is no
// "gap too wide, refuse" case any more: every real wattage classifies to
// exactly one step, by construction. What used to be "no match, too far"
// (0/3/7/10/13 dBm below the QMX's ~100 mW PC; floor) is now simply "no
// SWEPT point classifies as this exact step" - power_cal_voltage_for_dbm()
// below still returns false for that, just from a cleaner test.
int8_t power_cal_dbm_for_watts(uint16_t w_x100)
{
    float mw  = (float)w_x100 * 10.0f;         /* w_x100 is hundredths of a watt */
    float dbm = 10.0f * log10f(mw);
    int8_t best = WSPR_STD_DBM[0];
    float  best_gap = 1e9f;
    for (int i = 0; i < WSPR_STD_DBM_N; i++) {
        float gap = fabsf(dbm - (float)WSPR_STD_DBM[i]);
        if (gap < best_gap) { best_gap = gap; best = WSPR_STD_DBM[i]; }
    }
    return best;
}

// Runtime lookup, independent of the modal (which may never have been
// opened this session): given a band already calibrated by a PAST sweep
// (persisted, not the in-progress s_measured_w_x100 above), find a swept
// point that CLASSIFIES as target_dbm - power_cal_dbm_for_watts(its real
// wattage) == target_dbm - preferring the LOWEST voltage among any tied at
// the same step (never use more voltage than the one that already gets
// there, same rule power_cal_list_watts() uses for the Output power
// slider). Exactly the same classification render_results() above uses, so
// the WSPR drawer's "Declared power" dropdown (main/ui/ui.c,
// power_cal_apply_declared there) can never silently disagree with what
// this modal's own results table shows for the same band and dBm.
//
// out_w_x100 (optional) is that swept point's REAL measured wattage - by
// construction the one whose nearest standard step IS target_dbm, so the
// two can never again print as "37 dBm (5 W)" for a radio that only
// reaches 3.6 W (Operator, 2026-09-16). Callers that build a label for the
// operator should use this, not a fixed nominal-wattage string.
bool power_cal_voltage_for_dbm(const char *band, int8_t target_dbm, uint16_t *out_v_x10,
                                uint16_t *out_w_x100)
{
    uint8_t  v_x10[PWRCAL_STEPS];
    uint16_t w_x100[PWRCAL_STEPS];
    bool got = band && band[0] && settings_get_pwr_cal_band(band, v_x10, w_x100);
    /* Logging added 2026-09-15 chasing a bug that turned out to live in
     * ui.c (the WSPR drawer's build-once-per-boot section never re-checked
     * this after a fresh calibration - see wspr_dbm_area_refresh()'s own
     * header), not here. Kept anyway: 0/3/7/10/13 dBm missing is EXPECTED
     * (the QMX's own ~100 mW PC; floor), and a one-line reason beats a
     * silent "no" for anything that looks like it should have matched. */
    if (!got) {
        ESP_LOGW(TAG, "voltage_for_dbm(band=%s, dbm=%d): settings_get_pwr_cal_band found NOTHING",
                 (band && band[0]) ? band : "(empty)", target_dbm);
        return false;
    }

    /* power_cal_dbm_for_watts() gates WHICH rows may even be considered - a row
     * classifying as some OTHER step can never win here regardless of how
     * close its wattage looks, which is what makes this bucket-correct. Once
     * inside the bucket, break ties by closeness to THIS step's own nominal
     * wattage (10^((dbm-30)/10)), not by lowest voltage: "37 dBm" should
     * mean whichever real point best represents 5 W among the ones that
     * classify there, not merely the cheapest one to reach. Operator,
     * 2026-09-16, on a bucket containing both 3.3 W and 3.6 W: "you state
     * 37 dBm (3.3 W) not 3.6 W, why?" - the lowest-voltage rule (right for
     * the Output power slider below, wrong here) had picked 3.3 W. */
    int   best = -1;
    float best_gap_db = 1e9f;
    int   nonzero = 0;
    float target_w = powf(10.0f, ((float)target_dbm - 30.0f) / 10.0f);
    for (int i = 0; i < PWRCAL_STEPS; i++) {
        if (w_x100[i] == 0) continue;
        nonzero++;
        if (power_cal_dbm_for_watts(w_x100[i]) != target_dbm) continue;
        float w = (float)w_x100[i] / 100.0f;
        float gap_db = fabsf(10.0f * log10f(w / target_w));
        if (gap_db < best_gap_db) { best_gap_db = gap_db; best = i; }
    }
    if (best < 0) {
        ESP_LOGW(TAG, "voltage_for_dbm(band=%s, dbm=%d): row found with %d nonzero point(s), "
                      "none classifies as this step - no match",
                 band, target_dbm, nonzero);
        return false;
    }
    if (out_v_x10)  *out_v_x10  = v_x10[best];
    if (out_w_x100) *out_w_x100 = w_x100[best];   /* the REAL measured watts - see callers */
    return true;
}

// For the general "Output power" slider (main/ui/ui.c) - unlike
// power_cal_voltage_for_dbm() above, which matches against WSPR's fixed
// dBm steps, this control has no standard step list to honour: it should
// simply offer every wattage Calibrate Power actually measured on `band`,
// so every position is real (same "never offer a fake level" rule).
//
// The sweep tests voltage ASCENDING, and the QMX's own PC; readback has
// ~100 mW resolution, so several adjacent voltage steps routinely read the
// SAME rounded wattage (see settings.h's PWRCAL_STEPS comment). Listing
// every raw sample would put multiple slider positions a few pixels apart
// all labelled "0.3 W" - keep only the FIRST (i.e. LOWEST-voltage) sample
// at each distinct wattage, which is also the right one to keep: it is
// never worth using more voltage than the lowest that produces a given
// output.
//
// Returns the number of distinct levels found (0 = not calibrated).
// out_w_x100/out_v_x10 must each hold at least PWRCAL_STEPS entries;
// max_n bounds how many are actually written.
int power_cal_list_watts(const char *band, uint16_t *out_w_x100, uint16_t *out_v_x10, int max_n)
{
    uint8_t  v_x10[PWRCAL_STEPS];
    uint16_t w_x100[PWRCAL_STEPS];
    if (!band || !band[0] || !settings_get_pwr_cal_band(band, v_x10, w_x100)) return 0;

    int n = 0;
    uint16_t last_w = 0xFFFF;
    for (int i = 0; i < PWRCAL_STEPS && n < max_n; i++) {
        if (w_x100[i] == 0) continue;          // below the QMX's own measurable floor
        if (w_x100[i] == last_w) continue;     // same reading as a lower voltage already listed
        out_w_x100[n] = w_x100[i];
        out_v_x10[n]  = v_x10[i];
        n++;
        last_w = w_x100[i];
    }
    return n;
}

// Reverse of the two lookups above: given a voltage the RADIO is actually
// reporting (cat_get_pa_voltage_x10(), not something this session chose),
// find the wattage Calibrate Power measured at that exact voltage on `band`.
// For log/status lines that only ever see the voltage - the WSPR TX burst
// header logs "PA=%d.%d V" straight from a CAT readback, with no dBm or
// slider index in scope to look the wattage up the other way. Exact match
// only: the sweep is what it is, and a nearby-voltage guess here would be
// the same "wrong answer dressed up as a right one" this file already
// refuses to give for the dBm-keyed lookups.
bool power_cal_watts_for_voltage(const char *band, uint16_t v_x10, uint16_t *out_w_x100)
{
    uint8_t  vtab[PWRCAL_STEPS];
    uint16_t wtab[PWRCAL_STEPS];
    if (!band || !band[0] || !settings_get_pwr_cal_band(band, vtab, wtab)) return false;
    for (int i = 0; i < PWRCAL_STEPS; i++) {
        if (wtab[i] != 0 && vtab[i] == v_x10) {
            if (out_w_x100) *out_w_x100 = wtab[i];
            return true;
        }
    }
    return false;
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

        /* Same classify-then-pick-closest-to-nominal rule as
         * power_cal_voltage_for_dbm() - see that function's own header. */
        int   best = -1;
        float best_gap_db = 1e9f;
        float target_w = powf(10.0f, ((float)dbm - 30.0f) / 10.0f);
        for (int i = 0; i < PWRCAL_STEPS; i++) {
            if (s_measured_w_x100[i] == 0) continue;
            if (power_cal_dbm_for_watts(s_measured_w_x100[i]) != dbm) continue;
            float w = (float)s_measured_w_x100[i] / 100.0f;
            float gap_db = fabsf(10.0f * log10f(w / target_w));
            if (gap_db < best_gap_db) { best_gap_db = gap_db; best = i; }
        }

        char line[48];
        if (best >= 0) {
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
             step + 1, s_last_step + 1, s_test_voltage_x10[step] / 10, s_test_voltage_x10[step] % 10);
    status_set_text(buf);
}

// Highest table index at or below the operator's ceiling. Always returns a
// valid index: a ceiling below the first step still sweeps one point, which is
// visible in the "of N" count rather than silently doing nothing.
static int step_index_for_ceiling(uint16_t ceiling_x10)
{
    int last = 0;
    for (int i = 0; i < PWRCAL_STEPS; i++) {
        if (s_test_voltage_x10[i] <= ceiling_x10) last = i;
    }
    return last;
}

// Called once per completed step. A step with NO reading (w == 0) is not
// evidence either way and leaves the run untouched - a single failed settle
// mid-sweep must not look like the radio has stopped responding to voltage.
static void note_plateau(uint16_t w_x100)
{
    if (w_x100 == 0) return;
    uint16_t tol = (uint16_t)((uint32_t)s_max_w_x100 * PWRCAL_PLATEAU_TOL_PCT / 100);
    if (tol < PWRCAL_PLATEAU_TOL_MIN_X100) tol = PWRCAL_PLATEAU_TOL_MIN_X100;
    bool armed = (s_max_w_x100 >= PWRCAL_PLATEAU_ARM_W_X100) &&
                 (s_test_voltage_x10[s_step] >= PWRCAL_PLATEAU_MIN_V_X10);
    if (armed && w_x100 <= (uint16_t)(s_max_w_x100 + tol)) s_plateau_run++;
    else                                                   s_plateau_run = 0;
    if (w_x100 > s_max_w_x100) s_max_w_x100 = w_x100;
}

static void advance_or_finish(void)
{
    if (s_plateau_run >= PWRCAL_PLATEAU_SAMPLES) {
        s_stopped_early = true;
        ESP_LOGI(TAG, "plateau at %u.%uV after %d steps with no rise (best %u.%02u W) - stopping",
                 s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10,
                 s_plateau_run, s_max_w_x100 / 100, s_max_w_x100 % 100);
        enter_restore();
        return;
    }
    int next = s_step + 1;
    if (next <= s_last_step) {
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
    // A short sweep is not a failed one, and an operator who expected 45 steps
    // and got 29 needs to be told which of the two limits ended it.
    if (s_stopped_early) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Done - power stopped rising at %u.%uV.",
                 s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10);
        status_set_text(buf);
    } else if (s_last_step < PWRCAL_STEPS - 1) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Done - swept to your %u.%uV limit.",
                 s_orig_pa_x10 / 10, s_orig_pa_x10 % 10);
        status_set_text(buf);
    } else {
        status_set_text("Done.");
    }
    if (s_action_lbl) lv_label_set_text(s_action_lbl, "Start Calibration");
    if (s_action_btn) lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    if (s_cancel_btn) lv_obj_clear_state(s_cancel_btn, LV_STATE_DISABLED);
    /* The bottom button no longer CANCELS anything at this point - results
     * are already in and saved. "Done" says what tapping it now means: close
     * the window. Reverted to "Cancel" in start_btn_cb() when a new sweep
     * begins, since it goes back to being an abort button then. Operator,
     * 2026-09-15: "Cancel should be Done now(!)". */
    if (s_cancel_btn) lv_obj_set_style_bg_color(s_cancel_btn, lv_color_hex(0x2E7D32), 0);
    if (s_cancel_lbl) lv_label_set_text(s_cancel_lbl, "Done");
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
                     s_step + 1, s_last_step + 1, s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10,
                     (double)s_last_pw, s_stable_count, PWRCAL_STABLE_SAMPLES);
        } else {
            snprintf(buf, sizeof(buf), "Step %d of %d - %u.%uV - waiting for a reading...",
                     s_step + 1, s_last_step + 1, s_test_voltage_x10[s_step] / 10, s_test_voltage_x10[s_step] % 10);
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
            note_plateau(s_measured_w_x100[s_step]);
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
    /* Calibration needs the radio to itself - WSPR's own capture/decode
     * loop and its 2-minute CAT/audio cadence would otherwise collide with
     * the sweep's own TX;/TA;/RX; and mode changes. Rather than refuse and
     * send the operator out to the Panadapter to stop it by hand (their own
     * report: awkward, and easy to forget to come back and re-enable it
     * afterward), stop it here and remember to bring it back. wspr_rx_stop()
     * is the SAME shutdown Panadapter/FT8 swipe uses - it aborts anything
     * armed/keyed first, releases the PA-voltage guard, and only then
     * returns, so this never keys the radio out from under WSPR mid-burst. */
    s_wspr_was_running = wspr_rx_running();
    if (s_wspr_was_running) {
        wspr_rx_stop();
        ESP_LOGI(TAG, "WSPR receive stopped for the calibration sweep - "
                      "will resume when this window closes");
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

    // Never sweep past the operator's own Max. PA voltage. See the
    // PWRCAL_PLATEAU_* block for why it is capped here rather than trusted
    // blindly, and why the figure is on the modal before Start.
    if (s_idle_timer) { lv_timer_del(s_idle_timer); s_idle_timer = NULL; }
    s_last_step     = step_index_for_ceiling(s_orig_pa_x10);
    s_max_w_x100    = 0;
    s_plateau_run   = 0;
    s_stopped_early = false;
    ESP_LOGI(TAG, "sweep capped at %u.%uV (radio's Max. PA voltage) - %d of %d steps",
             s_orig_pa_x10 / 10, s_orig_pa_x10 % 10, s_last_step + 1, PWRCAL_STEPS);

    strncpy(s_band, adif_log_band_for_freq(cat_get_frequency()), sizeof(s_band) - 1);
    s_band[sizeof(s_band) - 1] = '\0';

    memset(s_measured_w_x100, 0, sizeof(s_measured_w_x100));
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_action_lbl) lv_label_set_text(s_action_lbl, "Running...");
    if (s_action_btn) lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(0xB03020), 0);
    if (s_results_lbl) lv_label_set_text(s_results_lbl, "");
    if (s_results_lbl2) lv_label_set_text(s_results_lbl2, "");
    /* Back to a real abort button for the sweep that is about to run -
     * finish_done() relabels this "Done" once results are in. */
    if (s_cancel_btn) lv_obj_set_style_bg_color(s_cancel_btn, lv_color_hex(0x962020), 0);
    if (s_cancel_lbl) lv_label_set_text(s_cancel_lbl, "Cancel");

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
        /* Bring WSPR back if this window is what stopped it - covers a
         * normal finish AND a mid-sweep Cancel/timeout, both of which land
         * in PC_DONE and wait for the operator to dismiss the window here
         * (see the comment above this function). Resuming on CLOSE, not on
         * PC_DONE itself, so the operator can still read the results with
         * the radio quiet before deciding. */
        if (s_wspr_was_running) {
            s_wspr_was_running = false;
            wspr_rx_start();
            ESP_LOGI(TAG, "resuming WSPR receive after calibration");
        }
        /* ⛔ WAS STUCK AT PC_DONE UNTIL THE NEXT SWEEP - power_cal_modal_show()'s
         * own pre-fill ("Previously calibrated - Start to re-measure") only
         * runs `if (s_state == PC_IDLE)`, so reopening after a finished run
         * skipped it and just left last time's rendered labels on screen
         * unchanged - a snapshot, not a live re-check. Reset here so the
         * NEXT open always re-queries settings fresh, same as a first-ever
         * open would. */
        s_state = PC_IDLE;
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
    lv_obj_set_size(s_panel, PWRCAL_PANEL_W, 680);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, PWRCAL_PANEL_PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Calibrate Power");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *warn = lv_label_create(s_panel);
    {
        static char warn_txt[128];
        snprintf(warn_txt, sizeof(warn_txt),
                 LV_SYMBOL_WARNING " Connect a DUMMY LOAD, not the antenna.\n"
                 "Keys a real carrier at %d steps - 5 to 8 minutes.",
                 PWRCAL_STEPS);
        lv_label_set_text(warn, warn_txt);
    }
    lv_obj_set_style_text_color(warn, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
    // WIDTH + WRAP, never a content-sized label. Reported by Bruce N9JCV on
    // v1.14.3 with a photo: the second line read "...ys a brief full carrier
    // at 45 voltage steps, about 5-8 minut" - clipped at BOTH ends, because a
    // centred label with no width sizes to its own content and the panel clips
    // whatever hangs over it.
    //
    // THE TEXT MUST STILL FIT TWO LINES AT THIS WIDTH. Everything below is
    // hand-positioned with 12 px of clearance (status at y=130, Start at
    // y=176), so a third line would draw over the status line. The wrap is the
    // backstop that stops a future edit clipping again; it is not room to write
    // more. Shortened from 63 characters to 48 to leave that margin.
    lv_obj_set_width(warn, PWRCAL_PANEL_CONTENT_W);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 60);

    s_status_lbl = lv_label_create(s_panel);
    lv_label_set_text(s_status_lbl, "Ready.");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    // Same class as the warning above, different remedy: this one sits 12 px
    // above the Start button, so it may never grow a line. LONG_DOT truncates
    // with an ellipsis, which cannot change the height - and an ellipsis reads
    // as "there is more", where clipping mid-word reads as a broken screen.
    lv_obj_set_width(s_status_lbl, PWRCAL_PANEL_CONTENT_W);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_DOT);
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
    s_cancel_lbl = lv_label_create(s_cancel_btn);
    lv_label_set_text(s_cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(s_cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_cancel_lbl);

    // Esc only, same reasoning as tune_modal.c: this keys a carrier, and the
    // only key worth having live is the one that stops it.
    ui_kbd_set_buttons(NULL, s_cancel_btn);

    ESP_LOGI(TAG, "Calibrate Power modal built");
}

// The pre-Start line. The ceiling is whatever the radio currently holds, and
// that is exactly the thing worth seeing before committing to a sweep: a WSPR
// session leaves it at 2.3 V quite legitimately, and calibrating 1.0-2.3 V is
// not what anyone pressing Start has in mind.
static void idle_status_refresh(void)
{
    if (s_state != PC_IDLE && s_state != PC_DONE) return;
    int16_t pa   = cat_get_pa_voltage_x10();
    bool    have = settings_get_pwr_cal_band(s_band, NULL, NULL);
    char buf[80];
    // Kept short on purpose: this label is one line inside a 632 px content
    // box and cannot grow one (see its LONG_DOT note), so the longest form
    // here - "Calibrated. Sweeps 1.0-12.0V (radio max)." - is 41 characters.
    if (pa >= 0) {
        /* ⛔ A LOW CEILING IS A WARNING, NOT A FOOTNOTE (Rick W5NR, 2026-09-19).
         *
         * His QMX+ was at 6.00 V when he pressed Start - which is exactly
         * WSPR_PA_TARGET_X10, the old fixed WSPR guard's figure, so a WSPR
         * session had almost certainly left it there. The sweep dutifully
         * capped at 6.0 V, measured a 600 mW ceiling, and the declared-power
         * slider has offered him 100-600 mW ever since. Every reconnect then
         * re-applies a voltage from that table: "if I disconnect my Tab5 ...
         * I can run my QMX+ at 3.7 watts ... But when I reconnect my Tab5 back
         * to the QMX+ the power will drop back to 600mW."
         *
         * The line already SAID "Sweeps 1.0-6.0V (radio max)". In neutral grey
         * that does not read as "you are about to calibrate half your radio",
         * and he had no reason to think it mattered. Our failure, not his.
         *
         * So below PWRCAL_LOW_CEILING_X10 it says what the sweep will actually
         * reach and what to do first, in the warning colour. No threshold can
         * know the operator's supply - a 9 V build legitimately tops out near
         * 9 V - so this INFORMS and never blocks. */
        if (pa < PWRCAL_LOW_CEILING_X10) {
            snprintf(buf, sizeof(buf), "Radio max is only %d.%dV - raise it first!",
                     pa / 10, pa % 10);
            status_set_warn(true);
            status_set_text(buf);
            return;
        }
        status_set_warn(false);
        snprintf(buf, sizeof(buf), "%s. Sweeps 1.0-%d.%dV (radio max).",
                 have ? "Calibrated" : "Ready", pa / 10, pa % 10);
    } else {
        status_set_warn(false);
        snprintf(buf, sizeof(buf), "%s", have ? "Calibrated. Start to re-measure." : "Ready.");
    }
    status_set_text(buf);
}

// Polls only until the radio answers, then deletes itself - the modal has no
// tick of its own while idle and does not need one for anything else.
static void idle_timer_cb(lv_timer_t *t)
{
    bool done = (++s_idle_ticks > 25)                 /* ~5 s, then give up */
             || (cat_get_pa_voltage_x10() >= 0)       /* answered */
             || (s_state != PC_IDLE && s_state != PC_DONE);
    idle_status_refresh();
    if (done) {
        lv_timer_del(t);
        s_idle_timer = NULL;
    }
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
    } else if (s_state == PC_IDLE) {
        if (s_results_lbl) lv_label_set_text(s_results_lbl, "");
        if (s_results_lbl2) lv_label_set_text(s_results_lbl2, "");
    }
    if (s_state == PC_IDLE || s_state == PC_DONE) {
        // Ask the radio for its Max. PA voltage and show it as soon as it
        // answers, so the sweep range is on screen BEFORE Start rather than
        // discovered afterwards from a short results table.
        cat_query_pa_voltage();
        idle_status_refresh();
        if (!s_idle_timer) {
            s_idle_ticks = 0;
            s_idle_timer = lv_timer_create(idle_timer_cb, 200, NULL);
        }
    }
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ESP_LOGI(TAG, "Calibrate Power modal shown");
}
