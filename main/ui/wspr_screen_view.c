/* The WSPR page. See wspr_screen_view.h and docs/wspr-ui-design.md. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"
#include "ui_theme.h"
#include "wspr_screen_view.h"
#include "wspr_spots.h"
#include "wspr_rx.h"
#include "esp_heap_caps.h"
#include "cat.h"
#include "util/dxcc.h"
#include "wspr_tx.h"
#include "storage/settings.h"
#include "wspr_sim.h"

/* JetBrains Mono, already compiled in for the QMX terminal page (#147). The
 * spot list is space-padded columns of short tokens, and in a PROPORTIONAL font
 * those do not line up - the header would sit visibly off its own rows. Reusing
 * the font that is already in the binary costs nothing and is the difference
 * between a table and a mess. */
LV_FONT_DECLARE(qmx_mono_25);

/* Duplicated from ui.c / ft8_screen_view.c, which already each carry their own
 * copy. Following the existing pattern rather than introducing a shared header
 * as a side effect of adding a page - but all three must move together. */
#define TOP_BAR_H     60
#define BOTTOM_BAR_H  36

#define MID_Y   TOP_BAR_H
#define MID_H   (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W   1280
#define LEFT_W  320

/* Rows the list can show at once. The pane is MID_H tall and a mono-25 row plus
 * line spacing is ~31 px, so 18 is a screenful including the cycle headers -
 * this is a screenful with the header - NOT the ring's capacity. Deliberately
 * bounded: the snapshot is copied onto the caller's buffer and this runs on
 * taskLVGL, where CLAUDE.md keeps a list of crashes caused by kB-scale locals
 * (the v0.20.1 pounce crash was an 11 KB array on exactly this task). */
/* Right-hand area, split as the operator asked: the captured window's
 * waterfall on top, the decode log underneath. */
#define RIGHT_X    (LEFT_W + 8)
#define RIGHT_W    (MID_W - RIGHT_X - 8)
#define WF_Y       6
#define WF_H       200
#define AXIS_Y     (WF_Y + WF_H + 2)
#define AXIS_H     22
#define LIST_Y     (AXIS_Y + AXIS_H + 8)

#define AXIS_TICKS 7      /* 1350..1650 every 50 Hz */
#define VIEW_ROWS  12

static lv_obj_t *s_container;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_dial;
static lv_obj_t *s_lbl_cycle;
static lv_obj_t *s_bar_cycle;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_heard;

static lv_obj_t *s_dd_dial;
static lv_obj_t *s_btn_tx;
static lv_obj_t *s_lbl_tx;
static lv_obj_t *s_btn_duty;
static lv_obj_t *s_lbl_duty;

/* THE standard WSPR dial for each band - the whole list, not a range.
 *
 * WSPR lives in a 200 Hz sub-band per band, and a station outside it is heard
 * by nobody. A free-entry keypad would therefore hand the operator a way to be
 * silently wrong, which is the exact error class CLAUDE.md keeps recording; a
 * list of the real ones cannot be. These are USB dial frequencies - the
 * transmission itself sits ~1400-1600 Hz above each. */
typedef struct { const char *label; uint32_t dial_hz; } wspr_band_t;
static const wspr_band_t kBands[] = {
    { "160 m  1.836600", 1836600u },
    { "80 m   3.568600", 3568600u },
    { "60 m   5.287200", 5287200u },
    { "40 m   7.038600", 7038600u },
    { "30 m  10.138700", 10138700u },
    { "20 m  14.095600", 14095600u },
    { "17 m  18.104600", 18104600u },
    { "15 m  21.094600", 21094600u },
    { "12 m  24.924600", 24924600u },
    { "10 m  28.124600", 28124600u },
    { "6 m   50.293000", 50293000u },
};
#define N_BANDS ((int)(sizeof(kBands) / sizeof(kBands[0])))

/* Duty is a CYCLING VALUE, per docs/wspr-ui-design.md: WSPR asks "what
 * fraction of slots", never "transmit now". 0 is a legitimate state - enabled
 * but silent - while setting up. */
static const uint8_t kDuty[] = { 0, 10, 20, 33, 50 };
#define N_DUTY ((int)(sizeof(kDuty) / sizeof(kDuty[0])))

static void dial_changed_cb(lv_event_t *e)
{
    uint16_t i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (i >= N_BANDS) return;
    settings_set_wspr_dial_hz(kBands[i].dial_hz);
    /* Forced: the ordinary setter shares a 200 ms rate limit with the CAT
     * poll, and a band change the operator just asked for must not be the
     * write that gets dropped. */
    cat_set_frequency_forced(kBands[i].dial_hz);
}

static void tx_toggle_cb(lv_event_t *e)
{
    (void)e;
    qmx_settings_t st;
    settings_load_all(&st);
    settings_set_wspr_tx_en(!st.wspr_tx_en);
}

static void duty_cycle_cb(lv_event_t *e)
{
    (void)e;
    qmx_settings_t st;
    settings_load_all(&st);
    int i = 0;
    for (int k = 0; k < N_DUTY; k++) if (kDuty[k] == st.wspr_duty_pct) { i = k; break; }
    settings_set_wspr_duty_pct(kDuty[(i + 1) % N_DUTY]);
}
static lv_obj_t *s_list;           /* right pane, one label per line */
static lv_obj_t *s_lbl_rows;
static lv_obj_t *s_wf_canvas;

static uint8_t  *s_wf_buf;      /* RGB565 canvas pixels */
static uint8_t  *s_wf_data;     /* WSPR_WF_HIST_ROWS x WSPR_WF_COLS, NEWEST ROW FIRST */
static uint32_t  s_wf_seen;

/* First logging in this file: the dial push is the one thing here that
 * silently changes the radio, so it says what it did and why. */
static const char *TAG = "wspr_view";

static int   s_last_spot_count = -1;
static char  s_last_status[48];

/* ---- THE STORED DIAL HAS TO BE PUSHED TO THE RADIO -------------------
 *
 * ⛔ IT USED TO BE PUSHED ONLY BY A TAP ON THE PICKER. Nothing re-applied it on
 * page entry, at boot, after a QMX power cycle, or on leaving simulation - so
 * the device could sit on the WSPR page with 20 m stored while the radio was on
 * 7.074 MHz, quietly decoding a 200 Hz slice of the FT8 calling frequency.
 * Observed exactly that on 2026-08-24, and again when a QMX power cycle brought
 * the radio back on 30 m mid-session.
 *
 * ⛔ AND IT CANNOT SIMPLY BE PUSHED AT PAGE-ENTRY TIME. CAT link-up is ~17 s
 * after boot, so an immediate write often has nowhere to go - this project
 * already shipped that bug once, where the CW-pitch value was written at ~4.5 s
 * and went nowhere on EVERY boot. So the push stays PENDING until
 * cat_is_ready() and then fires once.
 *
 * ⛔ AND IT MUST NOT FIGHT THE OPERATOR. Re-pushing continuously would drag the
 * radio back every time someone deliberately tuned off the sub-band. So this is
 * a BOUNDED ONE-SHOT armed by three discrete events - entering the page, CAT
 * coming back (which is what a QMX power cycle looks like from here), and
 * simulation being switched off - and it gives up rather than surprising
 * anyone minutes later. */
#define DIAL_PUSH_TRIES 60          /* ~60 s: comfortably past CAT link-up */

static int  s_dial_push_left;
static bool s_cat_was_ready;
static bool s_sim_was_on;

static void arm_dial_push(const char *why)
{
    s_dial_push_left = DIAL_PUSH_TRIES;
    ESP_LOGI(TAG, "dial: will push the stored WSPR dial to the radio (%s)", why);
}

/* ONE format string for the header AND every row.
 *
 * These used to be two independent strings - a hand-spaced header and a
 * printf format - and they drifted: PWR's data ended at column 43 where its
 * header started, and KM/BRG were off by one and two. Nothing catches that
 * except looking at the screen, which is how the operator found it.
 *
 * Every field is passed as a STRING, including the numeric ones, so the header
 * can be produced by the same specifiers. Numbers are right-aligned and their
 * headers with them, which is what a numeric column wants.
 *
 * Monospaced by construction (qmx_mono_25) - column arithmetic in characters
 * only means anything in a fixed-advance font. */
/* ---- THE COLUMN BUDGET, because it is exactly full ---------------------
 *
 * qmx_mono_25's advance is 15 px (240 sixteenths - see CELL_W in
 * qmx_term_view.c), and the pane is RIGHT_W = 944 px, so there are exactly
 * 62 characters. Every column below is its own true maximum and the gaps are
 * a single space, which is what "squeeze them together but keep a proper gap"
 * has to mean when the row is already at the edge:
 *
 *   UTC 5 (HH:MM)   CALL 10   GRID 4   COUNTRY 11   SNR 3   DRF 3
 *   HZ 6 (1416.3)   PWR 3     KM 5 (18897)          BRG 3
 *   = 53 + 9 single spaces = 62. Full. Nothing more fits.
 *
 * Consequences worth knowing before editing this:
 *  - GRID is 4 because WSPR_SPOT_GRID_MAX is 5. A 6-char grid cannot arrive.
 *  - KM is 5 because the antipode is ~20000 km.
 *  - DRIFT is headed DRF: the word is 5 characters and the data is 3, and
 *    since ONE format string serves header and rows the column would have to
 *    be 5 to hold the title. Abbreviating the title is cheaper than two wasted
 *    columns on every row.
 *  - CALL gets 10 - the struct's whole capacity - because a truncated
 *    CALLSIGN is a wrong identity, which this project does not print. COUNTRY
 *    is allowed to fall back instead of truncating; see country_field().
 *  - The UTC column is blank on all but the first row of a cycle. That still
 *    costs 6 characters, and it is worth it: it replaced a standalone
 *    timestamp line AND a blank line per group, so a 3-spot cycle went from
 *    5 lines to 3. */
#define ROW_FMT "%-5s %-10s %-4s %-11s %3s %3s %6s %3s %5s %3s"

/* Spelled out if it fits, else the DXCC alpha-3. NEVER truncated: "United
 * Stat" is not a country and a clipped name reads as a bug, while USA is
 * simply the shorter true answer. The full name comes from the callsign via
 * dxcc_lookup(), the same source the web panel uses, so the two screens
 * cannot disagree. */
#define COUNTRY_W 11
static const char *country_field(const wspr_spot_t *sp)
{
    const char *full = dxcc_lookup(sp->call);
    if (full && full[0] && strlen(full) <= COUNTRY_W) return full;
    return sp->cty[0] ? sp->cty : "--";
}

static void fmt_row(char *out, size_t n, const wspr_spot_t *sp, const char *utc)
{
    char snr[16], drift[16], hz[16], pwr[16], km[20], brg[16];

    /* An unmeasured value prints as a dash, never as a number. WSPR_SNR_UNKNOWN
     * and WSPR_DRIFT_UNKNOWN exist precisely so this cannot quietly become a
     * fabricated measurement - the same rule that deleted the ADIF "599". */
    if (sp->snr_db == WSPR_SNR_UNKNOWN) snprintf(snr, sizeof(snr), "--");
    else snprintf(snr, sizeof(snr), "%+d", sp->snr_db);

    if (sp->drift_hz == WSPR_DRIFT_UNKNOWN) snprintf(drift, sizeof(drift), "--");
    else snprintf(drift, sizeof(drift), "%+d", sp->drift_hz);

    snprintf(hz,  sizeof(hz),  "%.1f", (double)sp->freq_hz);
    snprintf(pwr, sizeof(pwr), "%d", (int)sp->power_dbm);

    if (sp->km < 0) snprintf(km, sizeof(km), "--");
    else snprintf(km, sizeof(km), "%d", (int)sp->km);

    if (sp->bearing_deg < 0) snprintf(brg, sizeof(brg), "--");
    else snprintf(brg, sizeof(brg), "%d", (int)sp->bearing_deg);

    snprintf(out, n, ROW_FMT, utc, sp->call, sp->grid,
             country_field(sp), snr, drift, hz, pwr, km, brg);
}

static void fmt_header(char *out, size_t n)
{
    snprintf(out, n, ROW_FMT, "UTC", "CALL", "GRID", "COUNTRY", "SNR", "DRF",
             "HZ", "PWR", "KM", "BRG");
}

static void cycle_label(char *out, size_t n, int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    /* HH:MM only - 5 characters, which is the column width. The word UTC is
     * in the column HEADER now, so repeating it on every group wasted 4. */
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

void wspr_screen_view_init(lv_obj_t *parent)
{
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    /* A backdrop, not a control - the pointer must not go green over it. */
    lv_obj_add_flag(s_container, UI_FLAG_NOT_HOT);

    /* ---------------- left pane ---------------- */
    s_lbl_title = lv_label_create(s_container);
    lv_label_set_text(s_lbl_title, "MODE: WSPR");
    /* 28, not 48. "MODE: FT8" fits the 320 px panel at 48; "MODE: WSPR" is one
     * character longer and overran it into the decode list's CALL column. */
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_pos(s_lbl_title, 16, 10);

    /* The dial, boxed like the FT8 page's preset. Read-only for now: the
     * standard-dial picker is the next piece (see docs/wspr-ui-design.md - a
     * free-entry keypad is deliberately NOT wanted, because every band has one
     * canonical WSPR frequency and anything else is simply not in the
     * sub-band). */
    lv_obj_t *box = lv_obj_create(s_container);
    lv_obj_set_size(box, LEFT_W - 32, 56);
    lv_obj_set_pos(box, 16, 48);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, UI_FLAG_NOT_HOT);

    s_dd_dial = lv_dropdown_create(box);
    {
        char opts[N_BANDS * 20];
        size_t used = 0;
        opts[0] = '\0';
        for (int i = 0; i < N_BANDS; i++) {
            used += (size_t)snprintf(opts + used, sizeof(opts) - used, "%s%s",
                                     i ? "\n" : "", kBands[i].label);
        }
        lv_dropdown_set_options(s_dd_dial, opts);
    }
    lv_obj_set_width(s_dd_dial, LEFT_W - 56);
    lv_obj_center(s_dd_dial);
    lv_obj_set_style_text_font(s_dd_dial, &lv_font_montserrat_20, 0);
    /* Dark like everything else on this page; the stock dropdown is white. */
    lv_obj_set_style_bg_color(s_dd_dial, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_text_color(s_dd_dial, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_dd_dial, lv_color_hex(UI_COLOR_BORDER), 0);
    {
        lv_obj_t *list = lv_dropdown_get_list(s_dd_dial);
        if (list) {
            lv_obj_set_style_bg_color(list, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
            lv_obj_set_style_text_color(list, lv_color_hex(UI_COLOR_TEXT), 0);
            lv_obj_set_style_text_font(list, &lv_font_montserrat_20, 0);
        }
    }
    /* Start on the STORED dial, not on entry 0.
     *
     * A screenshot caught this: with the radio wedged cat_get_frequency()
     * returns 0, the tick's sync never runs, and the picker sat on "160 m"
     * while the stored dial was 20 m. A control that displays a band it is not
     * set to is worse than one that displays nothing. */
    {
        qmx_settings_t ds;
        settings_load_all(&ds);
        for (int i = 0; i < N_BANDS; i++) {
            if (kBands[i].dial_hz == ds.wspr_dial_hz) {
                lv_dropdown_set_selected(s_dd_dial, (uint16_t)i);
                break;
            }
        }
    }
    lv_obj_add_event_cb(s_dd_dial, dial_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_lbl_dial = NULL;   /* the dropdown IS the dial readout now */

    /* The cycle: plain language above, one 120 s bar below. It orients - "am I
     * receiving, how long left" - rather than urging, because nothing in WSPR
     * needs a decision inside the cycle. */
    s_lbl_cycle = lv_label_create(s_container);
    lv_label_set_text(s_lbl_cycle, "starting...");
    lv_obj_set_style_text_font(s_lbl_cycle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_cycle, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_pos(s_lbl_cycle, 16, 118);

    s_bar_cycle = lv_bar_create(s_container);
    lv_obj_set_size(s_bar_cycle, LEFT_W - 32, 10);
    lv_obj_set_pos(s_bar_cycle, 16, 152);
    lv_bar_set_range(s_bar_cycle, 0, 120);
    lv_bar_set_value(s_bar_cycle, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_PRIMARY), LV_PART_INDICATOR);

    s_lbl_status = lv_label_create(s_container);
    lv_label_set_text(s_lbl_status, "");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_pos(s_lbl_status, 16, 172);

    s_lbl_heard = lv_label_create(s_container);
    lv_label_set_text(s_lbl_heard, "Heard nothing yet");
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_lbl_heard, 16, 202);

    /* TX and Duty, side by side.
     *
     * A WSPR transmission keys the radio for 110 SECONDS - eight times an FT8
     * burst. This project's rule for controls that key the radio (written for
     * SWR Tune) is that they must be impossible to trigger by accident and
     * visibly ACTIVE while engaged, which is why TX is a labelled toggle
     * reading OFF/ON rather than a one-tap "transmit". */
    const int half = (LEFT_W - 32 - 8) / 2;

    s_btn_tx = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_tx, half, 56);
    lv_obj_set_pos(s_btn_tx, 16, 240);
    lv_obj_set_style_radius(s_btn_tx, 8, 0);
    lv_obj_add_event_cb(s_btn_tx, tx_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_tx = lv_label_create(s_btn_tx);
    lv_label_set_text(s_lbl_tx, "TX  OFF");
    lv_obj_set_style_text_font(s_lbl_tx, &lv_font_montserrat_20, 0);
    lv_obj_center(s_lbl_tx);

    s_btn_duty = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_duty, half, 56);
    lv_obj_set_pos(s_btn_duty, 16 + half + 8, 240);
    lv_obj_set_style_radius(s_btn_duty, 8, 0);
    lv_obj_add_event_cb(s_btn_duty, duty_cycle_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_duty = lv_label_create(s_btn_duty);
    lv_label_set_text(s_lbl_duty, "Duty 20%");
    lv_obj_set_style_text_font(s_lbl_duty, &lv_font_montserrat_20, 0);
    lv_obj_center(s_lbl_duty);

    /* ---------------- right pane, upper: the captured window ---------------- */
    /* RGB565 at display resolution rather than a 205x176 image scaled up:
     * lv_canvas has no scaling, and drawing straight into display pixels keeps
     * the frequency axis below it exactly aligned with the columns. */
    s_wf_buf = heap_caps_malloc(RIGHT_W * WF_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_wf_buf) {
        s_wf_canvas = lv_canvas_create(s_container);
        lv_canvas_set_buffer(s_wf_canvas, s_wf_buf, RIGHT_W, WF_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_wf_canvas, RIGHT_X, WF_Y);
        lv_canvas_fill_bg(s_wf_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
        lv_obj_add_flag(s_wf_canvas, UI_FLAG_NOT_HOT);
    }

    /* The frequency scale. Evenly spaced ticks with numbers, because a
     * waterfall without them cannot answer "where is that signal?" - which is
     * the only question it is there to answer. */
    /* ONE LABEL PER TICK, positioned absolutely.
     *
     * The first version was a single space-padded string, which needs the
     * font's space width to be known - I assumed ~10 px for montserrat_18 and
     * it is about half that, so the scale ended at x~730 of a 944 px waterfall
     * and every label pointed at the wrong column. Absolute positions cannot be
     * wrong: each label is placed by the SAME arithmetic that maps a frequency
     * to a waterfall column, then centred on it. */
    for (int i = 0; i < AXIS_TICKS; i++) {
        int hz = 1350 + i * 50;
        int x  = (hz - (int)WSPR_WF_LO_HZ) * RIGHT_W /
                 (int)(WSPR_WF_HI_HZ - WSPR_WF_LO_HZ);
        lv_obj_t *t = lv_label_create(s_container);
        lv_label_set_text_fmt(t, "%d", hz);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_update_layout(t);
        int w = lv_obj_get_width(t);
        int px = RIGHT_X + x - w / 2;                /* centre on its column */
        if (px < RIGHT_X) px = RIGHT_X;              /* keep the ends on-screen */
        if (px + w > RIGHT_X + RIGHT_W) px = RIGHT_X + RIGHT_W - w;
        lv_obj_set_pos(t, px, AXIS_Y);
        /* A 1 px tick above the number, so the eye can follow it into the
         * waterfall rather than estimating. */
        lv_obj_t *tick = lv_obj_create(s_container);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 1, 4);
        lv_obj_set_pos(tick, RIGHT_X + x, AXIS_Y - 4);
        lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_add_flag(tick, UI_FLAG_NOT_HOT);
    }

    /* ---------------- right pane, lower: the log ---------------- */
    lv_obj_t *hdr = lv_label_create(s_container);
    { char h[160]; fmt_header(h, sizeof(h)); lv_label_set_text(hdr, h); }
    lv_obj_set_style_text_font(hdr, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(hdr, RIGHT_X, LIST_Y);

    s_list = lv_obj_create(s_container);
    lv_obj_set_size(s_list, RIGHT_W, MID_H - LIST_Y - 34);
    lv_obj_set_pos(s_list, RIGHT_X, LIST_Y + 30);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_list, UI_FLAG_NOT_HOT);

    /* ONE label holding every line, not one object per row.
     *
     * The FT8 list needs per-row objects because rows are touch targets - you
     * tap a station to work it. Nothing here is tappable: a WSPR spot is a
     * measurement, there is nobody to reply to. So a single multi-line label is
     * both simpler and much cheaper on an LVGL object budget this board has
     * repeatedly run into. */
    s_lbl_rows = lv_label_create(s_list);
    lv_label_set_text(s_lbl_rows, "Listening...");
    lv_obj_set_style_text_font(s_lbl_rows, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(s_lbl_rows, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_rows, 2, 0);
    lv_obj_set_pos(s_lbl_rows, 0, 0);
}

void wspr_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_container);
    s_last_spot_count = -1;      /* force a repaint on entry */
    s_last_status[0]  = '\0';
    /* Entering the page is the operator saying "receive WSPR", and that is
     * only true if the radio is actually on a WSPR dial. */
    arm_dial_push("page entry");
    s_cat_was_ready = cat_is_ready();
    s_sim_was_on    = wspr_sim_enabled();
}

void wspr_screen_view_hide(void)
{
    if (!s_container) return;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *wspr_screen_view_get_container(void) { return s_container; }

/* SDR-ish ramp: black -> blue -> cyan -> yellow -> red, same family the
 * panadapter's waterfall uses so the two pages read alike. Returns RGB565
 * directly - see repaint_waterfall() for why this does not go through
 * lv_color_t. */
static inline uint16_t wf_rgb565(uint8_t v)
{
    uint8_t r, g, b;
    /* Cycle-boundary marker: a light green (144,238,144) the signal ramp cannot
     * produce, because wspr_rx.c clamps real intensities to 254. Deliberately
     * NOT a value picked out of the ramp - a strong signal passes through green
     * on its way to red, so a palette green would still be ambiguous. */
    if (v == WSPR_WF_MARK) return (uint16_t)(((144 >> 3) << 11) | ((238 >> 2) << 5) | (144 >> 3));
    if (v < 64)        { r = 0; g = 0;                      b = (uint8_t)(v * 3); }
    else if (v < 128)  { r = 0; g = (uint8_t)((v - 64) * 4); b = 255; }
    else if (v < 192)  { r = (uint8_t)((v - 128) * 4); g = 255; b = (uint8_t)(255 - (v - 128) * 4); }
    else               { r = 255; g = (uint8_t)(255 - (v - 192) * 4); b = 0; }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Repaint the captured window.
 *
 * ⛔ Writes STRAIGHT INTO THE RGB565 BUFFER, not via lv_canvas_set_px().
 * This is 944 x 200 = 188,800 pixels, and set_px() goes through LVGL's draw
 * layer for every one of them - enough to block taskLVGL long enough to starve
 * the HTTP server. The symptom was the operator's browser disconnecting every
 * time a cycle finished, and /ss.bmp truncating at 135 KB of 1.84 MB. A direct
 * buffer fill plus one invalidate does the same job without holding the task.
 *
 * Called whenever the sequence number moves - which since the carpet became
 * row-by-row is roughly once per WSPR symbol (~1.5 Hz) while a capture is
 * filling, NOT once per cycle as this comment used to claim. That is the
 * reason the direct-buffer rule above is load-bearing rather than a nicety. */
static void repaint_waterfall(void)
{
    if (!s_wf_canvas || !s_wf_data || !s_wf_buf) return;
    uint16_t *px = (uint16_t *)s_wf_buf;

    /* Row and column maps precomputed once instead of a divide per pixel. */
    static uint16_t colmap[RIGHT_W];
    static uint16_t rowmap[WF_H];
    for (int x = 0; x < RIGHT_W; x++) colmap[x] = (uint16_t)(x * WSPR_WF_COLS / RIGHT_W);
    /* out row 0 is the NEWEST row, so display y maps straight through and the
     * newest data lands at the top - the panadapter's convention. */
    for (int y = 0; y < WF_H;    y++) rowmap[y] = (uint16_t)(y * WSPR_WF_HIST_ROWS / WF_H);

    for (int y = 0; y < WF_H; y++) {
        const uint8_t *src = &s_wf_data[rowmap[y] * WSPR_WF_COLS];
        uint16_t *dst = &px[y * RIGHT_W];
        for (int x = 0; x < RIGHT_W; x++) dst[x] = wf_rgb565(src[colmap[x]]);
    }
    lv_obj_invalidate(s_wf_canvas);
}

void wspr_screen_view_tick(void)
{
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    /* ---- re-arm triggers, then the pending push ---------------------- */
    {
        const bool cat_now = cat_is_ready();
        const bool sim_now = wspr_sim_enabled();
        /* CAT coming back is what a QMX power cycle looks like from here, and a
         * power cycle reloads the radio's own band config - measured, it came
         * back on 30 m while 20 m was stored. */
        if (cat_now && !s_cat_was_ready) arm_dial_push("CAT came back");
        /* Simulation never touches the radio, so switching it OFF is the first
         * moment the radio's actual frequency starts to matter again. */
        if (!sim_now && s_sim_was_on)    arm_dial_push("simulation off");
        s_cat_was_ready = cat_now;
        s_sim_was_on    = sim_now;

        if (s_dial_push_left > 0) {
            if (!cat_now) {
                s_dial_push_left--;      /* wait for the link, do not give up yet */
                if (s_dial_push_left == 0)
                    ESP_LOGW(TAG, "dial: CAT never became ready - the radio keeps "
                                  "whatever frequency it is on");
            } else {
                qmx_settings_t ds;
                settings_load_all(&ds);
                const uint32_t want = ds.wspr_dial_hz;
                const uint32_t have = cat_get_frequency();
                if (want && have != want) {
                    /* Forced: the ordinary setter shares a 200 ms rate limit
                     * with the CAT poll, and the one write that decides whether
                     * this page hears anything at all must not be the one that
                     * gets dropped. */
                    cat_set_frequency_forced(want);
                    ESP_LOGW(TAG, "dial: pushed %lu Hz to the radio (was %lu)",
                             (unsigned long)want, (unsigned long)have);
                } else if (want) {
                    ESP_LOGI(TAG, "dial: radio already on %lu Hz", (unsigned long)want);
                }
                s_dial_push_left = 0;    /* one shot - never fight manual tuning */
            }
        }
    }

    /* Dial: select the standard entry matching the radio, so the picker shows
     * where we actually are rather than what was last tapped. A dial that is
     * not a standard WSPR frequency leaves the selection alone - the operator
     * has tuned off the sub-band and the picker should not pretend otherwise. */
    uint32_t f = cat_get_frequency();
    if (f && s_dd_dial) {
        for (int i = 0; i < N_BANDS; i++) {
            if (kBands[i].dial_hz == f) {
                if (lv_dropdown_get_selected(s_dd_dial) != (uint16_t)i)
                    lv_dropdown_set_selected(s_dd_dial, (uint16_t)i);
                break;
            }
        }
    }

    /* TX and Duty, from settings so the web UI and the buttons cannot drift.
     *
     * While a burst is running the TX block goes UI_COLOR_TX_ACTIVE orange and
     * counts down: this project's rule for anything that keys the radio is
     * that the operator should never have to wonder whether it is
     * transmitting. 110 s is a long time to be unsure. */
    {
        qmx_settings_t st;
        settings_load_all(&st);

        char txt[48];
        int secs = 0;
        wspr_tx_state_t tst = wspr_tx_get_status(NULL, 0, &secs);

        if (tst == WSPR_TX_ACTIVE) {
            snprintf(txt, sizeof(txt), "TX  ON AIR");
            lv_obj_set_style_bg_color(s_btn_tx, lv_color_hex(UI_COLOR_TX_ACTIVE), 0);
        } else if (tst == WSPR_TX_ARMED) {
            snprintf(txt, sizeof(txt), "TX  in %d:%02d", secs / 60, secs % 60);
            lv_obj_set_style_bg_color(s_btn_tx, lv_color_hex(UI_COLOR_PRIMARY), 0);
        } else {
            snprintf(txt, sizeof(txt), "TX  %s", st.wspr_tx_en ? "ON" : "OFF");
            lv_obj_set_style_bg_color(s_btn_tx,
                lv_color_hex(st.wspr_tx_en ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_RAISED), 0);
        }
        if (strcmp(lv_label_get_text(s_lbl_tx), txt) != 0)
            lv_label_set_text(s_lbl_tx, txt);

        char dt[32];
        snprintf(dt, sizeof(dt), "Duty %u%%", (unsigned)st.wspr_duty_pct);
        if (strcmp(lv_label_get_text(s_lbl_duty), dt) != 0)
            lv_label_set_text(s_lbl_duty, dt);
        /* Duty is subordinate to TX and says nothing while TX is off, so it
         * only takes colour when it can actually act. A screenshot had the
         * bright button on Duty and the dull one on TX, which pulls the eye to
         * the control that matters less. */
        lv_obj_set_style_bg_color(s_btn_duty,
            lv_color_hex(st.wspr_tx_en ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_text_color(s_lbl_duty,
            lv_color_hex(st.wspr_tx_en ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED), 0);
    }

    /* cycle position: the bar is the 120 s window, so it is a real clock
     * position rather than a progress guess. */
    time_t now = time(NULL);
    int into = (int)(now % 120);
    lv_bar_set_value(s_bar_cycle, into, LV_ANIM_OFF);

    char c[48];
    snprintf(c, sizeof(c), "cycle  %d:%02d / 2:00", into / 60, into % 60);
    lv_label_set_text(s_lbl_cycle, c);

    /* status straight from the slot loop - change-detected, because writing an
     * identical string still costs LVGL an invalidate. */
    const char *st = wspr_rx_running() ? wspr_rx_status() : "receiver stopped";
    if (strncmp(st, s_last_status, sizeof(s_last_status)) != 0) {
        snprintf(s_last_status, sizeof(s_last_status), "%s", st);
        lv_label_set_text(s_lbl_status, s_last_status);
    }

    /* the captured window, repainted only when a new one has landed */
    uint32_t seq = wspr_rx_waterfall_seq();
    if (seq != s_wf_seen && s_wf_canvas) {
        if (!s_wf_data)
            s_wf_data = heap_caps_malloc(WSPR_WF_HIST_ROWS * WSPR_WF_COLS,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_wf_data && wspr_rx_get_waterfall(s_wf_data)) {
            s_wf_seen = seq;
            repaint_waterfall();
        }
    }

    /* the log, repainted only when the spot count actually changed */
    int n = wspr_spots_count();
    if (n == s_last_spot_count) return;
    s_last_spot_count = n;

    int uniq = wspr_spots_unique_calls();
    char h[48];
    if (uniq == 0) snprintf(h, sizeof(h), "Heard nothing yet");
    else snprintf(h, sizeof(h), "Heard %d station%s", uniq, uniq == 1 ? "" : "s");
    lv_label_set_text(s_lbl_heard, h);

    if (n == 0) {
        lv_label_set_text(s_lbl_rows, "Listening...");
        return;
    }

    static wspr_spot_t snap[VIEW_ROWS];   /* file-scope static, never the stack */
    int got = wspr_spots_get(snap, VIEW_ROWS);

    /* Grouped under the cycle each burst was heard in - the whole reason this
     * is a log and not a live list. */
    char buf[VIEW_ROWS * 110 + 256];
    size_t off = 0;
    int64_t last_cycle = 0;
    for (int i = 0; i < got && off < sizeof(buf) - 96; i++) {
        /* The cycle time is the row FIRST COLUMN, printed once per cycle and
         * blank for the rest. It used to be a line of its own preceded by a
         * blank line - two lines per cycle to carry five characters, which on
         * a pane this size was most of the log. A 3-spot cycle went 5 -> 3. */
        char utc[8] = "";
        if (snap[i].cycle_utc != last_cycle) {
            last_cycle = snap[i].cycle_utc;
            cycle_label(utc, sizeof(utc), last_cycle);
        }
        char row[160];
        fmt_row(row, sizeof(row), &snap[i], utc);
        off += snprintf(buf + off, sizeof(buf) - off, "%s\n", row);
    }
    lv_label_set_text(s_lbl_rows, buf);
}
