/* The WSPR page. See wspr_screen_view.h and docs/wspr-ui-design.md. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "util/format_freq.h"   // #302
#include "lvgl.h"
#include "esp_attr.h"      /* EXT_RAM_BSS_ATTR on the row snapshot */

#include "ui.h"
#include "ui_theme.h"
#include "wspr_screen_view.h"
#include "wspr_spots.h"
#include "net/wsprnet.h"
#include "wspr_rx.h"
#include "cat.h"
#include "esp_heap_caps.h"
#include "util/dxcc.h"
#include "wspr_tx.h"
#include <math.h>
#include "esp_timer.h"
#include "storage/settings.h"

/* One narrow read per call, never settings_load_all() - this runs once per row
   on taskLVGL and that struct is kilobytes (CLAUDE.md lists four crashes from
   exactly that). Defined here, above every user: the best-DX panel needs it
   long before the row formatter does. */
static inline bool wspr_dist_in_miles(void) { return settings_get_distance_in_miles(); }
#include "wspr_sim.h"

/* JetBrains Mono, already compiled in for the QMX terminal page (#147). The
 * spot list is space-padded columns of short tokens, and in a PROPORTIONAL font
 * those do not line up - the header would sit visibly off its own rows. Reusing
 * the font that is already in the binary costs nothing and is the difference
 * between a table and a mess. It is also what the waterfall letter markers use
 * (#360), so a letter on the carpet and its letter in the S column below are
 * the same glyph at the same size. */
LV_FONT_DECLARE(qmx_mono_25);

/* Duplicated from ui.c / ft8_screen_view.c, which already each carry their own
 * copy. Following the existing pattern rather than introducing a shared header
 * as a side effect of adding a page - but all three must move together. */
#define TOP_BAR_H     60
#define BOTTOM_BAR_H  36

#define MID_Y   TOP_BAR_H
#define MID_H   (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W   1280
/* ⭐ 372, NOT 320, TO FIT "MODE: WSPR" AT 48 pt. The header is ~336 px wide
 * and starts at x=16, so 320 left it running out of the panel and over the
 * waterfall. The operator accepted that overlap once ("the wf can run under
 * it") but only because the alternative offered then was a smaller font; given
 * a wider panel he would rather it simply fit. What the panel takes, the
 * decode table gives back - see ROW_FMT. */
#define LEFT_W  372

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

/* ⭐ THE DECODE TABLE STARTS FURTHER LEFT THAN THE WATERFALL, and that is the
 * point rather than an oversight (operator, 2026-09-07: "there is plenty of
 * space left of the utc - not above, but leave that as is with the wf").
 *
 * LEFT_W cannot shrink: it is 372 so "MODE: WSPR" fits at 48 pt, and moving
 * RIGHT_X would drag the waterfall left with it. But NOTHING in the left pane
 * below the waterfall needs its full width, so the TABLE alone reclaims 60 px
 * - four characters at qmx_mono_25's exact 15.0 px advance - which is what
 * paid for the DT column. The lower-left widgets are narrowed to match
 * (EX_W_LOW) so nothing collides.
 *
 * ⛔ The waterfall and its axis keep RIGHT_X/RIGHT_W. Do not "tidy" these into
 * one pair of macros - they describe two different columns on purpose. */
/* 90, not 60: the extra 30 px is the `S` column and its separator (#360) - two
 * characters at qmx_mono_25's exact 15.0 px advance. The row was 63 of 63, so
 * there was nowhere else it could come from, and the operator named this as the
 * place: "there is still dead space enough on the right side of the WSPR
 * panel. We could easily cut out 30px or more and then just narrow the TX
 * button." EX_W_LOW is derived from this, and the TX button from EX_W_LOW, so
 * this one number moves all three. */
#define LIST_SHIFT 90
#define LIST_X     (RIGHT_X - LIST_SHIFT)
#define LIST_W     (RIGHT_W + LIST_SHIFT)
#define WF_Y       6
#define WF_H       200
#define AXIS_Y     (WF_Y + WF_H + 2)
#define AXIS_H     22
#define LIST_Y     (AXIS_Y + AXIS_H + 8)

#define AXIS_TICKS 7      /* 1350..1650 every 50 Hz */
/* Rows RENDERED, not rows visible - the pane shows about a dozen and scrolls
 * through the rest, which is what the operator asked for ("like FT8/4"). 64 is
 * a quarter of the 256-entry ring: several screenfuls to scroll back through
 * without rendering a log nobody will reach. */
#define VIEW_ROWS  64

static lv_obj_t *s_container;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_dial;
static lv_obj_t *s_lbl_cycle;
static lv_obj_t *s_bar_cycle;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_heard;

static lv_obj_t *s_btn_dial;       /* opens the band picker; carries s_lbl_dial */
static lv_obj_t *s_btn_tx;
static lv_obj_t *s_lbl_tx;

/* THE standard WSPR dial for each band - the whole list, not a range.
 *
 * WSPR lives in a 200 Hz sub-band per band, and a station outside it is heard
 * by nobody. A free-entry keypad would therefore hand the operator a way to be
 * silently wrong, which is the exact error class CLAUDE.md keeps recording; a
 * list of the real ones cannot be. These are USB dial frequencies - the
 * transmission itself sits ~1400-1600 Hz above each. */
/* The type and the radio-availability accessor live in the header now, because
 * the band-hop tick list needs the same table and the same filtering. */
static const wspr_band_t kBands[] = {
    { "160", 1836600u },
    { "80", 3568600u },
    { "60", 5287200u },
    { "40", 7038600u },
    { "30", 10138700u },
    { "20", 14095600u },
    { "17", 18104600u },
    { "15", 21094600u },
    { "12", 24924600u },
    { "10", 28124600u },
    { "6", 50293000u },
};
#define N_BANDS ((int)(sizeof(kBands) / sizeof(kBands[0])))

const char *wspr_band_name_for_dial(uint32_t dial_hz)
{
    if (!dial_hz) return NULL;                    /* recorded before we kept it */
    for (int i = 0; i < N_BANDS; i++)
        if (kBands[i].dial_hz == dial_hz) return kBands[i].name;
    return NULL;                                  /* off-table dial - say nothing */
}

const wspr_band_t *wspr_bands(int *out_count)
{
    if (out_count) *out_count = N_BANDS;
    return kBands;
}

int wspr_bands_available(uint8_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int nradio = 0;
    const cat_band_entry_t *radio = cat_get_band_list(&nradio);
    int n = 0;
    for (int i = 0; i < N_BANDS && n < max; i++) {
        if (nradio > 0) {
            int have = 0;
            for (int r = 0; r < nradio; r++)
                if (!strcmp(radio[r].name, kBands[i].name)) { have = 1; break; }
            if (!have) continue;
        }
        out[n++] = (uint8_t)i;
    }
    return n;
}

/* Up here rather than beside the first ESP_LOGI that used it: band hopping logs
 * the dial change it makes, and that code sits well above the old site. */
static const char *TAG = "wspr_view";

/* Duty is a CYCLING VALUE, per docs/wspr-ui-design.md: WSPR asks "what
 * fraction of slots", never "transmit now". 0 is a legitimate state - enabled
 * but silent - while setting up. */
/* The ONLY legal duty values, and now shared with the settings drawer so the
 * two cannot offer different sets. Exported through wspr_screen_view.h. */
const uint8_t kDuty[] = { 0, 10, 20, 33, 50 };
#define N_DUTY ((int)(sizeof(kDuty) / sizeof(kDuty[0])))

/* Which kBands entries this radio can reach, in table order. Built when the
 * page is constructed and refreshed whenever CAT reports a band list, because
 * at boot the page can be built before the radio has answered. */
static uint8_t s_avail[16];
static int     s_navail;

/* The picker lists only the bands the radio has, so its selection index is into
 * s_avail[], never into kBands[] directly. Getting that wrong would silently
 * tune the wrong band. */
/* rebuild_dial_options() is GONE with the dropdown it filled. The band picker
 * composes its rows in bp_open() from the same kBands table and the same
 * format_freq_hz(), so a frequency-format change is picked up the next time it
 * is opened rather than needing the list rewritten in place (#302). */

/* #302: the band picker's option list is composed when the dropdown is built
   and never again, so a frequency-format change leaves it showing the
   punctuation it was created with - reported from the bench as "changing it
   does not change WSPR". Rebuilding the options is all it takes; the selected
   index is preserved because the order is unchanged. */
/* Declared here because the frequency-format hook below is the FIRST user and
   sits well above the picker's own code. */
static void bp_button_refresh(void);

void wspr_screen_view_freq_style_changed(void)
{
    /* The BUTTON carries the frequency now, and the picker's rows are composed
       fresh every time it opens - so a format change needs only the button
       repainting, and the list looks after itself. */
    bp_button_refresh();
}


/* ---- THE LOWER HALF OF THE LEFT PANEL ------------------------------------
 *
 * Everything below the TX buttons answers a question the decode list cannot:
 * how is the band DOING, rather than what did it just say.
 *
 *   BEST DX      - the furthest station this session. WSPR's whole point is
 *                  how far a few milliwatts got, and that answer otherwise
 *                  scrolls off the list within a few cycles.
 *   HISTORY      - stations per cycle, oldest left. A snapshot cannot tell an
 *                  opening band from a closing one; a row of bars can.
 *   WSPRNET      - what would be published, and whether it can be.
 *   BAND HOP     - which bands to rotate through, ticked off.
 *
 * All four read state that already exists. None of them measures anything new,
 * which is deliberate: this is presentation, and the measuring belongs in the
 * decoder where it can be validated against wsprd.
 */
#define EX_X      16
#define EX_W      (LEFT_W - 32)
/* ⛔ WIDGETS BELOW THE WATERFALL MUST BE NARROWER, because the decode table
 * reaches LIST_SHIFT px further left than the waterfall does (see LIST_X). The
 * table's left edge is LIST_X, so anything in this pane at that height has to
 * end before it. Everything ABOVE the table - the MODE header, the dial
 * dropdown, the cycle bar - keeps the full EX_W. */
#define EX_W_LOW  (EX_W - LIST_SHIFT)
/* ⭐ TX SITS AT THE BOTTOM AND EVERYTHING ELSE MOVED UP (Roy KI0ER, 2026-09-01:
 * the TX button "is still where you have to touch to switch to the Panadapter";
 * operator's call: "lets move it to the bottom then - and free up the space in
 * the middle of the panel").
 *
 * v1.10.5 shifted the button right, out of the 30 px edge-swipe strip, which
 * fixed the horizontal overlap Randy reported. It did not fix this one, because
 * the problem is VERTICAL: the button sat at y=258 in a 624 px panel, i.e.
 * across the middle, and the middle of the left edge is exactly where a hand
 * goes for the page-swipe grip. Being clear of the strip in x does not help if
 * the thumb lands there on the way past.
 *
 * At the bottom it is nowhere near the grip, and the three read-only extras -
 * BEST DX, the cycle history and the wsprnet line - move up into the space it
 * vacated, so the panel has no hole in the middle.
 *
 * ⚠ These three and the TX button's y must move TOGETHER. This file's own
 * history is a section height and its y drifting apart; keep the arithmetic
 * here, where all four are visible at once. */
#define EX_DX_Y   258
#define EX_HIST_Y 340
#define EX_NET_Y  406
/* Bottom of the panel, one button height plus a margin. */
#define EX_TX_Y   (MID_H - 72)
/* ⚠ BAND HOP SITS AT THE BOTTOM, and the gap above it is not slack.
 * The wsprnet line above wraps to TWO lines once the counts reach two digits
 * ("wsprnet: off - 12 of 25 calls confirmed"), and at 506 the second line
 * printed straight through the BAND HOP heading. Anchored to the panel's
 * bottom instead of stacked below its neighbour, so a line that grows can
 * never reach it. */
#define EX_HOP_Y  (MID_H - 90)

#define HIST_BARS  WSPR_CYCLE_HISTORY
#define HIST_BAR_W 6
#define HIST_GAP   1
#define HIST_H     30

static lv_obj_t *s_lbl_dx;
static lv_obj_t *s_hist_bar[HIST_BARS];
static lv_obj_t *s_lbl_net;
static lv_obj_t *s_lbl_hdr;        /* the column headings over the decode list */
static bool      s_hdr_miles;      /* the unit the headings were built for */
static bool      s_hdr_built;      /* have WE written the headings yet */
static lv_obj_t *s_btn_clr;        /* clear the decode list (Samuel W7STF) */
static lv_obj_t *s_lbl_clr;
static lv_obj_t *s_btn_hold;       /* freeze the carpet (#370) */
static lv_obj_t *s_lbl_hold;
static lv_obj_t *s_lbl_held;       /* the caption ON the pane while frozen */
static bool      s_wf_hold;
static uint32_t  s_wf_hold_rows0;  /* wspr_rx_wf_rows_total() when Hold began */
static int       s_wf_top;         /* first visible row of the display copy */
static void repaint_waterfall(void);
static int64_t   s_clr_armed_us;   /* two-tap arming, 0 = not armed */
static lv_obj_t *s_hop_cb[16];
static uint8_t   s_hop_band[16];   /* kBands index behind each checkbox */
static int       s_hop_n;
static lv_obj_t *s_btn_hop;        /* opens the picker */
static lv_obj_t *s_lbl_hop;        /* says which bands are ticked */
static lv_obj_t *s_hop_modal;      /* NULL when closed */

static void hop_button_refresh(void);
/* ---- HOVER THE WATERFALL TO NAME A TRACE ----------------------------------
 *
 * Samuel W7STF asked for hover readouts; the operator picked the version worth
 * having: point at a trace and be told WHOSE it is. On a WSPR waterfall the
 * traces are the whole picture and the list underneath is the answer key, and
 * matching one to the other by eye means reading a tone off the axis and then
 * hunting the TONE column.
 *
 * ⛔ MOUSE ONLY, AND THAT IS FINE HERE. A touchscreen has no hover - a finger
 * is either not there or is a press - so this can only ever be an extra. It
 * adds nothing that is not already in the list, which is what makes it
 * acceptable for it to be unavailable to most operators. Nothing may become
 * reachable ONLY this way.
 *
 * The match is by tone, within half a WSPR signal's width either side. A WSPR
 * transmission is about 6 Hz wide, so +/-4 Hz is "the trace under the pointer"
 * without claiming the neighbour 20 Hz away. The NEAREST spot wins when two
 * are in range, and a repeat station shows its most recent hearing. */
#define HOVER_TOL_HZ   4.0f
#define HOVER_PERIOD   100      /* ms - a tooltip does not need 30 Hz */

static lv_obj_t *s_hover_lbl;

static void hover_hide(void)
{
    if (s_hover_lbl && !lv_obj_has_flag(s_hover_lbl, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(s_hover_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void hover_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_hover_lbl || !s_container ||
        lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) { hover_hide(); return; }

    lv_point_t p;
    if (!ui_mouse_pointer(&p)) { hover_hide(); return; }
    if (p.x < RIGHT_X || p.x >= RIGHT_X + RIGHT_W ||
        p.y < WF_Y    || p.y >= WF_Y + WF_H) { hover_hide(); return; }

    /* x -> tone, the exact inverse of the tick placement above. */
    const float hz = WSPR_WF_LO_HZ +
        (float)(p.x - RIGHT_X) * (WSPR_WF_HI_HZ - WSPR_WF_LO_HZ) / (float)RIGHT_W;

    /* ⛔ A BOUNDED SNAPSHOT ON THIS TASK'S STACK IS NOT AN OPTION - the ring
       holds 256 spots and taskLVGL has crashed this project on kB-scale locals
       more than once. wspr_spots_get() copies into a caller buffer, so this
       walks a SMALL window of the newest entries instead: a trace on screen was
       decoded in the last cycle or two, so the newest handful is all that can
       possibly match what is being pointed at. */
    wspr_spot_t recent[12];
    const int n = wspr_spots_get(recent, (int)(sizeof(recent) / sizeof(recent[0])));
    int best = -1;
    float bestd = HOVER_TOL_HZ;
    for (int i = 0; i < n; i++) {
        const float d = fabsf(recent[i].freq_hz - hz);
        if (d <= bestd) { bestd = d; best = i; }
    }
    if (best < 0) { hover_hide(); return; }

    const wspr_spot_t *sp = &recent[best];
    char t[64];
    if (sp->snr_db == WSPR_SNR_UNKNOWN)
        snprintf(t, sizeof(t), "%s  %.1f Hz", sp->call, (double)sp->freq_hz);
    else
        snprintf(t, sizeof(t), "%s  %.1f Hz  %+d dB",
                 sp->call, (double)sp->freq_hz, sp->snr_db);
    lv_label_set_text(s_hover_lbl, t);
    lv_obj_clear_flag(s_hover_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_hover_lbl);

    /* Placed BESIDE the pointer, never under it, and flipped to the left near
       the right-hand edge so the text can never run off the screen. */
    lv_obj_update_layout(s_hover_lbl);
    const int w = lv_obj_get_width(s_hover_lbl);
    int x = p.x + 16;
    if (x + w > MID_W - 4) x = p.x - 16 - w;
    if (x < RIGHT_X) x = RIGHT_X;
    int y = p.y - 34;
    if (y < WF_Y) y = p.y + 20;
    lv_obj_set_pos(s_hover_lbl, x, y);
}

/* ---- BAND PICKER: a dense drag-to-pick list, not a dropdown ---------------
 *
 * Operator, 2026-09-07: "make the band selector dropdown list like the other
 * dense lists we have - so you touch and the line you hit lights up, and if it
 * was the wrong one then drag up and down till you hit it."
 *
 * That is the Reader Contents panel's gesture and the FT8 decode list's, and it
 * is the right one for a touchscreen: an lv_dropdown commits on the cell your
 * finger happens to LIFT over, with no way to see what you are about to choose
 * and no way to change your mind without reopening it. Here the highlight
 * follows the finger and only the RELEASE commits, so a mis-landing costs a
 * drag rather than a wrong band and a CAT write.
 *
 * ⛔ THE ROWS ARE THE HIT TEST, and the panel owns the gesture - individual
 * rows are NOT clickable. LVGL delivers a press to one object and then sends
 * PRESSING to that same object wherever the finger goes, so a per-row handler
 * would light the row you started on and never follow you off it. Same reason
 * reader_view.c does it this way. */
#define BP_ROW_H   52

/* Quiet window after this page pushes the dial, so the mismatch check below
 * does not fire on our own write while the FA poll is still catching up. */
static int64_t   s_dial_settle_us = 0;
/* Which mark set the rows on screen were rendered against - see the guard in
 * the tick. The S column is drawn from wspr_rx_mark_for_freq(), so a new set of
 * marks makes every row stale. */
static uint32_t  s_rows_marks_seq = 0;
static lv_obj_t *s_bp_panel;                 /* NULL when closed */
static lv_obj_t *s_bp_row[N_BANDS];
static int       s_bp_n;
static int       s_bp_hi = -1;               /* highlighted row, -1 = none */

static void bp_highlight(int k)
{
    if (k == s_bp_hi) return;
    if (s_bp_hi >= 0 && s_bp_hi < s_bp_n && lv_obj_is_valid(s_bp_row[s_bp_hi]))
        lv_obj_set_style_bg_opa(s_bp_row[s_bp_hi], LV_OPA_TRANSP, 0);
    s_bp_hi = k;
    if (k >= 0 && k < s_bp_n && lv_obj_is_valid(s_bp_row[k])) {
        lv_obj_set_style_bg_color(s_bp_row[k], lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_bg_opa(s_bp_row[k], LV_OPA_40, 0);
    }
}

static void bp_close(void)
{
    if (!s_bp_panel) return;
    lv_obj_del(s_bp_panel);
    s_bp_panel = NULL;
    s_bp_hi = -1;
    s_bp_n = 0;
}

static void bp_apply(int k);

static void bp_drag_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int hit = -1;
        for (int k = 0; k < s_bp_n; k++) {
            lv_area_t ar;
            lv_obj_get_coords(s_bp_row[k], &ar);
            if (p.x >= ar.x1 && p.x <= ar.x2 && p.y >= ar.y1 && p.y <= ar.y2) { hit = k; break; }
        }
        bp_highlight(hit);
    } else if (code == LV_EVENT_RELEASED) {
        const int k = s_bp_hi;
        /* ⛔ READ THE CHOICE BEFORE CLOSING - bp_close() clears s_bp_hi, and
           applying afterwards would always read -1. */
        bp_close();
        if (k >= 0) bp_apply(k);
    } else if (code == LV_EVENT_PRESS_LOST) {
        /* A finger that leaves the panel entirely chooses nothing. Releasing
           OUTSIDE is how you cancel, which is what a list like this should
           mean by it. */
        bp_close();
    }
}

static void bp_open(void)
{
    if (s_bp_panel) { bp_close(); return; }   /* a second tap on the button closes it */
    s_navail = wspr_bands_available(s_avail, (int)sizeof(s_avail));
    if (s_navail <= 0) return;
    s_bp_n = s_navail;

    const int h = BP_ROW_H * s_bp_n + 8;
    s_bp_panel = lv_obj_create(s_container);
    lv_obj_set_size(s_bp_panel, EX_W, h);
    /* Directly under the button, and clamped so a long list cannot run off the
       bottom of the panel. */
    int y = 70 + 56 + 4;
    if (y + h > MID_H - 8) y = MID_H - 8 - h;
    if (y < 8) y = 8;
    lv_obj_set_pos(s_bp_panel, EX_X, y);
    lv_obj_set_style_bg_color(s_bp_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_bp_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_bp_panel, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(s_bp_panel, 1, 0);
    lv_obj_set_style_radius(s_bp_panel, 8, 0);
    lv_obj_set_style_pad_all(s_bp_panel, 4, 0);
    lv_obj_clear_flag(s_bp_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_bp_panel);

    qmx_settings_t cs;
    settings_load_all(&cs);
    const uint32_t cur = cs.wspr_dial_hz;
    for (int k = 0; k < s_bp_n; k++) {
        lv_obj_t *r = lv_obj_create(s_bp_panel);
        lv_obj_set_size(r, EX_W - 16, BP_ROW_H - 2);
        lv_obj_set_pos(r, 0, k * BP_ROW_H);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        /* NOT clickable - the panel owns the gesture, see the note above. */
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);

        char fs[20];
        format_freq_hz(kBands[s_avail[k]].dial_hz, g_freq_style, fs, sizeof(fs));
        char txt[40];
        snprintf(txt, sizeof(txt), "%s m  %s", kBands[s_avail[k]].name, fs);
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        /* The band in force is named in the accent colour, so the list says
           where you ARE as well as offering where to go. */
        lv_obj_set_style_text_color(l,
            lv_color_hex(kBands[s_avail[k]].dial_hz == cur ? UI_COLOR_PRIMARY : 0xFFFFFF), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        s_bp_row[k] = r;
    }

    lv_obj_add_event_cb(s_bp_panel, bp_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_bp_panel, bp_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_bp_panel, bp_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_bp_panel, bp_drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* The button's own label, so it always names the band in force - including
   after a band HOP, which changes the dial without anyone touching this. */
static void bp_button_refresh(void)
{
    if (!s_lbl_dial) return;
    qmx_settings_t bs;
    settings_load_all(&bs);
    const uint32_t hz = bs.wspr_dial_hz;
    const char *bn = wspr_band_name_for_dial(hz);
    char fs[20], t[44];
    format_freq_hz(hz, g_freq_style, fs, sizeof(fs));
    snprintf(t, sizeof(t), "%s m  %s", bn ? bn : "--", fs);
    lv_label_set_text(s_lbl_dial, t);
}

static void bp_apply(int k)
{
    if (k < 0 || k >= s_navail) return;
    const int i = s_avail[k];
    settings_set_wspr_dial_hz(kBands[i].dial_hz);
    /* Forced: the ordinary setter shares a 200 ms rate limit with the CAT
     * poll, and a band change the operator just asked for must not be the
     * write that gets dropped. */
    wspr_rx_wf_floor_reset();   /* the new band has its own noise floor */
    cat_set_frequency_forced(kBands[i].dial_hz);
    bp_button_refresh();
}

static void bp_button_cb(lv_event_t *e) { (void)e; bp_open(); }


/* ⭐ THE HEADINGS ARE NOT CONSTANT - one of them names a UNIT. Built once at
   page construction, the KM/MI heading froze at whatever the setting was then,
   so ticking miles converted every VALUE and left the title saying KM. Rebuilt
   whenever the unit changes, and only then - the string is 63 characters and
   nothing else in it can move. */
static void fmt_header(char *out, size_t n);
static void wspr_header_refresh(void)
{
    if (!s_lbl_hdr) return;
    const bool mi = wspr_dist_in_miles();
    /* ⛔ AN EXPLICIT FLAG, NOT "is the label empty yet". A fresh
       lv_label_create() starts with LVGL's own placeholder text "Text", so
       testing the label for content answered "already built" the very first
       time and skipped the only build that mattered - the headings never
       appeared at all until a unit change forced a rebuild, which is precisely
       what the operator saw ("the header labels are gone and only come up
       after a km/mi change"). Never ask a widget whether YOU have written to
       it; remember that yourself. */
    if (s_hdr_built && s_hdr_miles == mi) return;
    s_hdr_built = true;
    s_hdr_miles = mi;
    char h[224];
    fmt_header(h, sizeof(h));
    lv_label_set_text(s_lbl_hdr, h);
}

/* Two taps, because this discards spots that may not have been published yet -
   see the note beside the button. The armed state expires so a stray first tap
   cannot leave it primed for the rest of the session. */
#define CLR_ARM_WINDOW_US  4000000
/* Hold is edge-triggered on the row count, so pressing it twice in a row
 * cannot accumulate an offset, and releasing always returns to live. */
static void wf_hold_cb(lv_event_t *e)
{
    (void)e;
    s_wf_hold = !s_wf_hold;
    if (s_wf_hold) s_wf_hold_rows0 = wspr_rx_wf_rows_total();
    if (s_lbl_hold) lv_label_set_text(s_lbl_hold, s_wf_hold ? "Live" : "Hold");
    if (s_btn_hold)
        lv_obj_set_style_bg_color(s_btn_hold,
            lv_color_hex(s_wf_hold ? UI_COLOR_ACCENT_GOLD : UI_COLOR_SURFACE), 0);
    if (s_lbl_hold)
        lv_obj_set_style_text_color(s_lbl_hold,
            lv_color_hex(s_wf_hold ? 0x000000 : 0xFFFFFF), 0);
    if (s_lbl_held) {
        if (s_wf_hold) lv_obj_clear_flag(s_lbl_held, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_lbl_held, LV_OBJ_FLAG_HIDDEN);
    }
    /* Repaint at once rather than waiting for the next row: on Hold nothing
     * moves, so without this the button would appear to do nothing until the
     * carpet next advanced. */
    repaint_waterfall();
}

static void clear_spots_cb(lv_event_t *e)
{
    (void)e;
    const int64_t now = esp_timer_get_time();
    if (s_clr_armed_us && (now - s_clr_armed_us) < CLR_ARM_WINDOW_US) {
        s_clr_armed_us = 0;
        wspr_spots_clear();
        if (s_lbl_clr) lv_label_set_text(s_lbl_clr, "Clear");
        ESP_LOGI(TAG, "WSPR decode list cleared by the operator");
        ui_toast("Decodes cleared");
        return;
    }
    s_clr_armed_us = now;
    if (s_lbl_clr) lv_label_set_text(s_lbl_clr, "Sure?");
}

static void hop_toggled_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    uint16_t mask = 0;
    for (int i = 0; i < s_hop_n; i++) {
        if (s_hop_cb[i] && lv_obj_has_state(s_hop_cb[i], LV_STATE_CHECKED))
            mask |= (uint16_t)(1u << s_hop_band[i]);
    }
    (void)cb;
    settings_set_wspr_hop_mask(mask);
    /* Hopping is ON exactly when more than one band is ticked. A separate
     * enable switch would be a second thing to get wrong, and "one band ticked"
     * already means "stay there" - which is the same as off. */
    settings_set_wspr_hop_en(__builtin_popcount(mask) > 1);
    hop_button_refresh();
}

/* ---- THE BAND-HOP PICKER ----------------------------------------------
 *
 * A full-screen window with finger-sized rows, opened from the panel button.
 * The panel itself only ever shows WHICH bands are ticked; choosing them is a
 * deliberate act that gets room to happen in.
 *
 * ⚠ THE LIST IS BUILT HERE, ON OPEN, NOT AT PAGE INIT. That is not tidiness:
 * wspr_bands_available() filters against cat_get_band_list(), and CAT does not
 * answer until ~17 s after boot. Built during init the filter always saw an
 * empty radio list and silently offered every band in the table on every
 * radio. Built on open, the radio has long since answered.
 */
static void hop_modal_close(void)
{
    if (!s_hop_modal) return;
    lv_obj_del(s_hop_modal);
    s_hop_modal = NULL;
    for (int i = 0; i < 16; i++) s_hop_cb[i] = NULL;
    s_hop_n = 0;
    hop_button_refresh();
}

static void hop_close_cb(lv_event_t *e) { (void)e; hop_modal_close(); }

static void hop_modal_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_hop_modal) return;

    s_hop_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_hop_modal, 1280, 720);
    lv_obj_set_pos(s_hop_modal, 0, 0);
    lv_obj_set_style_bg_color(s_hop_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_hop_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_hop_modal, 0, 0);
    lv_obj_clear_flag(s_hop_modal, LV_OBJ_FLAG_SCROLLABLE);
    /* A scrim you dismiss, not a control you press - so the mouse pointer
     * stays white over it (ui_theme.h). */
    lv_obj_add_flag(s_hop_modal, UI_FLAG_NOT_HOT);
    lv_obj_add_flag(s_hop_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hop_modal, hop_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_hop_modal);
    lv_obj_set_size(panel, 780, 660);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Presses inside the panel must not reach the scrim's dismiss handler. */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Band hop");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);   /* 36 and 40 are not built into this image; 32 is */
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(panel);
    /* Says how it works, in the order the questions actually arise. The
     * operator asked all three of these, which is the sign the window was
     * showing controls without explaining them:
     *   - what does a tick DO?       one cycle each, so two minutes per band
     *   - what if I tick only one?   nothing hops; the band selector wins
     *   - so what should I do?       pick at least two - say it plainly */
    lv_label_set_text(hint,
        "Pick at least two bands.\n"
        "The radio moves to the next ticked band every cycle -\n"
        "two minutes on each - in the order listed below, then wraps.\n"
        "\n"
        "Fewer than two ticked means no hopping at all: the band\n"
        "selector on the page decides, and the radio stays there.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 62);

    qmx_settings_t hs;
    settings_load_all(&hs);
    s_hop_n = wspr_bands_available(s_hop_band, (int)sizeof(s_hop_band));

    if (s_hop_n == 0) {
        /* Says which of the two it is. "No bands" with the radio off reads as
         * a broken feature; it is a disconnected radio. */
        lv_obj_t *none = lv_label_create(panel);
        lv_label_set_text(none, cat_is_ready()
            ? "The radio reported no bands."
            : "Waiting for the radio - connect the QMX and reopen this.");
        lv_obj_set_style_text_font(none, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(none, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_align(none, LV_ALIGN_TOP_LEFT, 0, 238);
    }

    /* Two columns of finger-sized rows. 64 px pitch and a 32 px tick box: the
     * grid this replaces used 30 px rows and a 20 px box, which is what made
     * it unusable with a finger. */
    for (int i = 0; i < s_hop_n; i++) {
        lv_obj_t *cb = lv_checkbox_create(panel);
        lv_checkbox_set_text(cb, kBands[s_hop_band[i]].name);
        lv_obj_set_style_text_font(cb, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_pad_all(cb, 8, 0);
        lv_obj_set_style_width(cb, 32, LV_PART_INDICATOR);
        lv_obj_set_style_height(cb, 32, LV_PART_INDICATOR);
        lv_obj_align(cb, LV_ALIGN_TOP_LEFT, (i % 2) * 340, 238 + (i / 2) * 64);
        if (hs.wspr_hop_mask & (1u << s_hop_band[i]))
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_add_event_cb(cb, hop_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);
        s_hop_cb[i] = cb;
    }

    lv_obj_t *done = lv_btn_create(panel);
    lv_obj_set_size(done, 200, 64);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(done, 8, 0);
    lv_obj_add_event_cb(done, hop_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done");
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_28, 0);
    lv_obj_center(dl);
}

/* Opened from the settings drawer now that the button has moved off this page.
 * The modal parents to lv_layer_top(), so it is indifferent to whether the
 * WSPR page is visible - and it rebuilds its band list every time it opens,
 * which is what makes it correct when the radio answered late. */
void wspr_screen_view_open_hop_picker(void)
{
    hop_modal_open_cb(NULL);
}

/* The panel button says what is ticked, so the picker never has to be opened
 * just to find out. */
static void hop_button_refresh(void)
{
    if (!s_lbl_hop) return;
    qmx_settings_t hs;
    settings_load_all(&hs);

    uint8_t bands[16];
    int n = wspr_bands_available(bands, (int)sizeof(bands));
    char t[64];
    size_t off = 0;
    int ticked = 0;
    for (int i = 0; i < n && off < sizeof(t) - 8; i++) {
        if (!(hs.wspr_hop_mask & (1u << bands[i]))) continue;
        ticked++;
        off += (size_t)snprintf(t + off, sizeof(t) - off, "%s%s",
                                ticked > 1 ? " " : "", kBands[bands[i]].name);
    }
    /* ⚠ The names only fit while there are few of them. EX_W is 340 px and
     * montserrat_28 averages ~15 px a character, so about 22 characters -
     * "160 80 60 40 30 20" is 18 and fits, but a QMX+ with eleven bands ticked
     * would be 33 and run off the button. Past the limit it says how many
     * instead, which is the useful summary anyway; the picker has the detail. */
    if (ticked == 0)        snprintf(t, sizeof(t), "Band hop: off");
    else if (off > 22)      snprintf(t, sizeof(t), "%d bands", ticked);
    lv_label_set_text(s_lbl_hop, t);
}


static lv_obj_t *ex_heading(const char *text, int y)
{
    lv_obj_t *l = lv_label_create(s_container);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(l, EX_X, y);
    return l;
}

static void build_left_extras(void)
{
    /* ---- best DX ---- */
    ex_heading("BEST DX", EX_DX_Y);
    s_lbl_dx = lv_label_create(s_container);
    lv_label_set_text(s_lbl_dx, "-");
    lv_obj_set_style_text_font(s_lbl_dx, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_dx, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_width(s_lbl_dx, EX_W_LOW);
    lv_obj_set_pos(s_lbl_dx, EX_X, EX_DX_Y + 22);

    /* ---- cycle history ---- */
    ex_heading("STATIONS PER CYCLE", EX_HIST_Y);
    for (int i = 0; i < HIST_BARS; i++) {
        lv_obj_t *b = lv_obj_create(s_container);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, HIST_BAR_W, 2);
        lv_obj_set_pos(b, EX_X + i * (HIST_BAR_W + HIST_GAP),
                       EX_HIST_Y + 22 + HIST_H - 2);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_add_flag(b, UI_FLAG_NOT_HOT);
        s_hist_bar[i] = b;
    }

    /* ---- wsprnet ---- */
    s_lbl_net = lv_label_create(s_container);
    lv_label_set_text(s_lbl_net, "wsprnet: -");
    /* ⛔ NOT 18. This project settled long ago that 18 is below what is
     * readable on this screen at arm's length, and it went in here anyway. */
    lv_obj_set_style_text_font(s_lbl_net, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_width(s_lbl_net, EX_W_LOW - 100);
    lv_obj_set_pos(s_lbl_net, EX_X, EX_NET_Y);

    /* ---- Clear, beside the confirmed line ----
     *
     * Samuel W7STF asked for it exactly here: "a button, perhaps below the
     * stations per cycle and to the right of xx/yy confirmed, for clearing the
     * decodes".
     *
     * ⛔ IT CLEARS THE RING, WHICH IS ALSO THE UPLOAD QUEUE. Anything not yet
     * published to wsprnet goes with it, and the heard-more-than-once gate is
     * computed from the same ring - so clearing resets which stations are
     * confirmed, not just what is on screen. That is why it asks first: a
     * mis-tap should not silently discard spots the operator was waiting to
     * publish. Same two-tap arming the ADIF delete-all uses. */
    /* ---- Hold ----
     *
     * ⛔ IT MUST ANNOUNCE ITSELF. A frozen carpet is pixel-identical to a hung
     * one - that is the whole reason wf_mark_boundary() exists at all - so the
     * button goes amber and a HELD caption sits on the pane while it is on.
     * Nothing here may be inferred from the picture alone.
     *
     * Deliberately a BUTTON and not a drag on the carpet: touch sampling on
     * this display was measured at 98-233 ms between passes, so a drag reads
     * as stick-and-slip, and it would repaint 944x200 px per move against the
     * ~1.5 Hz the carpet does now - on the core that is already the wall. */
    s_btn_hold = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_hold, 92, 40);
    lv_obj_set_pos(s_btn_hold, EX_X + EX_W_LOW - 92 - 100, EX_NET_Y - 4);
    lv_obj_set_style_radius(s_btn_hold, 8, 0);
    lv_obj_set_style_bg_color(s_btn_hold, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(s_btn_hold, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_hold, 1, 0);
    lv_obj_add_event_cb(s_btn_hold, wf_hold_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_hold = lv_label_create(s_btn_hold);
    lv_label_set_text(s_lbl_hold, "Hold");
    lv_obj_set_style_text_font(s_lbl_hold, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_hold, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_lbl_hold);

    s_btn_clr = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_clr, 92, 40);
    lv_obj_set_pos(s_btn_clr, EX_X + EX_W_LOW - 92, EX_NET_Y - 4);
    lv_obj_set_style_radius(s_btn_clr, 8, 0);
    lv_obj_set_style_bg_color(s_btn_clr, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(s_btn_clr, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_clr, 1, 0);
    lv_obj_add_event_cb(s_btn_clr, clear_spots_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_clr = lv_label_create(s_btn_clr);
    lv_label_set_text(s_lbl_clr, "Clear");
    lv_obj_set_style_text_font(s_lbl_clr, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_clr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_lbl_clr);

    /* ---- band hop ----
     *
     * ⭐ A BUTTON, NOT A GRID OF CHECKBOXES IN THE PANEL, AND FOR TWO REASONS.
     *
     * The obvious one is the operator's: eleven 20 px checkboxes crammed into
     * whatever height was left at the bottom of the panel cannot be hit with a
     * finger. It was a list you could read and not use.
     *
     * The one that would have gone unnoticed is worse. The tick list is
     * filtered to the bands the RADIO reports (wspr_bands_available ->
     * cat_get_band_list), but it was built in this init function, which runs
     * during boot - and CAT does not come up until about 17 s. So nradio was
     * always 0, the filter never applied, and every band in the table was
     * offered on every radio. Exactly the shape of the CW-pitch bug CLAUDE.md
     * records: a value read once, too early, and never revisited.
     *
     * Building the list when the WINDOW OPENS fixes both at once - by then the
     * radio has long since answered. */
/* BAND HOP moved to the settings drawer (operator, 2026-08-28), for the same
     * reason Duty did: choosing WHICH BANDS to rotate through is a decision made
     * once for a session, not a control reached while watching spots arrive. The
     * picker itself is unchanged and still parents to lv_layer_top(), so it
     * opens correctly from the drawer with this page hidden. */
}

/* ---- BAND HOPPING --------------------------------------------------------
 *
 * ⛔ THERE IS NO SAFE MOMENT INSIDE A CYCLE TO CHANGE BAND. The capture arms on
 * the even minute and runs the FULL 120 s, so a dial change at any point during
 * it corrupts that window - the first half would be one band and the rest
 * another, and the decoder would be handed something no station transmitted.
 *
 * So the hop happens in the last few seconds BEFORE a boundary, which is the
 * only gap there is: the previous capture has finished and the next has not
 * armed. Three seconds is comfortably more than a QMX takes to retune and
 * comfortably less than the gap.
 *
 * Hopping is ON exactly when more than one band is ticked - "one band ticked"
 * already means "stay there", so a separate enable switch would only be a
 * second thing to get wrong.
 */
/* ⭐ SIX, NOT THREE. The hop is attempted from a 1 Hz tick, so a 3 s window
 * gave it about three chances per cycle - and a MISS is not neutral, it leaves
 * the radio on the band it was already on, so misses accumulate into "it nearly
 * always transmits on one band" (Dirk DK7CVD, 2026-09-08). Core 0 on this board
 * runs at 0-7 % idle and the worst measured taskLVGL pass gap is 233 ms, so a
 * tick landing late is ordinary rather than exotic. Six costs nothing - the
 * s_hop_done_cycle guard makes a second attempt in the same window a no-op. */
#define HOP_LEAD_SEC 6

static int64_t s_hop_done_cycle = -1;

static void hop_maybe(void)
{
    qmx_settings_t hs;
    settings_load_all(&hs);
    if (!hs.wspr_hop_en) return;

    const uint16_t mask = hs.wspr_hop_mask;
    if (__builtin_popcount(mask) < 2) return;

    /* The reachability test below reads s_avail, which is filled when the page
       is BUILT. Now that hopping runs with the page hidden - and the page is
       built lazily - it can be empty here, which would silently reject every
       band and stop hopping altogether. Fill it on demand. */
    if (s_navail <= 0) s_navail = wspr_bands_available(s_avail, (int)sizeof(s_avail));
    if (s_navail <= 0) return;

    const time_t now = time(NULL);
    if (now < 1600000000) return;                /* clock not set yet */
    const int64_t next_cycle = (int64_t)(now / 120) + 1;
    if ((now % 120) < (120 - HOP_LEAD_SEC)) return;
    if (s_hop_done_cycle == next_cycle) return;  /* already hopped for it */

    /* Next ticked band AFTER the current one, wrapping - so the rotation is the
     * table's order and an operator can predict where it goes next. */
    int cur = -1;
    for (int i = 0; i < N_BANDS; i++)
        if (kBands[i].dial_hz == hs.wspr_dial_hz) { cur = i; break; }

    int pick = -1;
    for (int step = 1; step <= N_BANDS; step++) {
        const int i = (cur < 0 ? 0 : (cur + step) % N_BANDS);
        if (!(mask & (1u << i))) continue;
        /* ⚠ A stored mask can name a band this RADIO does not have - the mask
         * outlives a change of radio, and settings.h says why it is not
         * silently pruned. Skip it here rather than tuning somewhere the
         * hardware cannot filter. */
        int reachable = 0;
        for (int k = 0; k < s_navail; k++) if (s_avail[k] == i) { reachable = 1; break; }
        if (!reachable) continue;
        pick = i;
        break;
    }
    if (pick < 0 || kBands[pick].dial_hz == hs.wspr_dial_hz) {
        s_hop_done_cycle = next_cycle;
        return;
    }

    s_hop_done_cycle = next_cycle;
    settings_set_wspr_dial_hz(kBands[pick].dial_hz);
    wspr_rx_wf_floor_reset();   /* the new band has its own noise floor */
    cat_set_frequency_forced(kBands[pick].dial_hz);
    bp_button_refresh();   /* the hop changed the dial - say so on the button */
    ESP_LOGI(TAG, "band hop -> %s m (%lu Hz) for the cycle starting in %llds",
             kBands[pick].name, (unsigned long)kBands[pick].dial_hz,
             (long long)(120 - (now % 120)));
}

static void refresh_left_extras(void)
{
    /* Best DX. An ACCESSOR, not a snapshot - see wspr_spots.h for why a 10 KB
     * copy must not land on taskLVGL. */
    if (s_lbl_dx) {
        wspr_spot_t dx;
        char t[64];
        if (wspr_spots_best_dx(&dx) && dx.km >= 0) {
            /* SPELLED OUT here, unlike the table's COUNTRY column. That
             * column is one of ten on a fixed-width line and has to fall back
             * to the alpha-3; this line has the whole panel width to itself,
             * so "Germany" beats "DEU" with nothing to trade for it. Falls
             * back the same way when the callsign is not in the DXCC table. */
            const char *full = dxcc_lookup(dx.call);
            const char *where = (full && full[0]) ? full
                              : (dx.cty[0] ? dx.cty : dx.grid);
            /* Miles if that is what the operator asked for - the same switch
             * the table's KM/MI column follows. This line said "km"
             * unconditionally, which is half of Samuel W7STF's report. */
            const bool mi = wspr_dist_in_miles();
            snprintf(t, sizeof(t), "%s  %s\n%ld %s  %d dBm",
                     dx.call, where,
                     mi ? lround(dx.km * 0.621371) : (long)dx.km,
                     mi ? "mi" : "km", (int)dx.power_dbm);
        } else {
            snprintf(t, sizeof(t), "-");
        }
        lv_label_set_text(s_lbl_dx, t);
    }

    /* Stations per cycle. Scaled to the busiest cycle held rather than to a
     * fixed ceiling: what matters is the SHAPE - rising or falling - and a fixed
     * scale would flatten a quiet band into nothing. */
    {
        uint8_t h[HIST_BARS];
        int n = wspr_rx_cycle_history(h, HIST_BARS);
        int peak = 1;
        for (int i = 0; i < n; i++) if (h[i] > peak) peak = h[i];
        for (int i = 0; i < HIST_BARS; i++) {
            if (!s_hist_bar[i]) continue;
            /* Oldest at the left, so a partly-filled history grows rightwards
             * the way the decode list does. */
            int v = (i < n) ? h[i] : -1;
            int px = (v <= 0) ? 2 : 2 + (v * (HIST_H - 2)) / peak;
            lv_obj_set_size(s_hist_bar[i], HIST_BAR_W, px);
            lv_obj_set_pos(s_hist_bar[i], EX_X + i * (HIST_BAR_W + HIST_GAP),
                           EX_HIST_Y + 22 + HIST_H - px);
            uint32_t c = (v < 0)  ? UI_COLOR_BORDER          /* no cycle yet */
                       : (v == 0) ? 0x553333                 /* heard nothing */
                                  : UI_COLOR_SUCCESS_BORDER;
            lv_obj_set_style_bg_color(s_hist_bar[i], lv_color_hex(c), 0);
        }
    }

    /* ⛔ THIS SAID "off" AS A STRING LITERAL, AND WENT ON SAYING IT AFTER THE
     * UPLOADER WAS BUILT AND PUBLISHING. The operator watched his own spots
     * appear on wsprnet.org while this line told him it was switched off. It
     * was written when upload genuinely did not exist and was honest then;
     * nothing tied it to the truth afterwards.
     *
     * It now asks the uploader. Third time in one day that a thing was added
     * and its surface left behind (wspr_en on the wrong endpoint, wspr_net_en
     * missing from /api/settings, and this) - a status line must READ state,
     * never restate what someone believed when they typed it.
     *
     * The confirmed count stays, because it is the part the operator cannot
     * get anywhere else: how many of the calls heard are eligible under the
     * heard-more-than-once rule that gates publication. */
    /* The KM/MI heading follows the setting, which can change from the web
       while this page is open. Cheap: it returns at once unless the unit
       actually moved. */
    wspr_header_refresh();

    /* Let a forgotten "Sure?" fall back to "Clear" on its own, so the button
       never sits armed waiting for a tap the operator stopped intending. */
    if (s_clr_armed_us &&
        (esp_timer_get_time() - s_clr_armed_us) >= CLR_ARM_WINDOW_US) {
        s_clr_armed_us = 0;
        if (s_lbl_clr) lv_label_set_text(s_lbl_clr, "Clear");
    }

    if (s_lbl_net) {
        char t[96];
        const int rpt = wspr_spots_repeat_calls();
        const int all = wspr_spots_unique_calls();
        /* ⚠ TWO LINES, AND THE WIDTH IS PART OF THE CONTRACT. The panel is
         * 340 px at 22 pt - about 28 characters - and this label sits directly
         * above the BAND HOP heading. "wsprnet: on - 8 sent, 3 waiting" over
         * "18 of 25 calls confirmed" ran to THREE wrapped lines and printed
         * through the heading below, which is the same collision the wsprnet
         * line already caused once when its counts reached double figures.
         * Both halves are kept short at the source rather than trimmed here:
         * see the note beside s_status in wsprnet.c. */
        /* ⭐ "confirmed" ALONE MEANT NOTHING - Samuel W7STF had to ask what it
         * was ("what is the meaning of the display for xx/yy confirmed?").
         * It is the publication gate: a call is only sent to wsprnet once it
         * has been heard more than once, so this is how many of the calls
         * heard are eligible. "publishable" names the consequence rather than
         * the internal state - and it also answers his OTHER question, why
         * wspr.rocks shows fewer unique calls than this screen says we heard.
         * The two numbers are the two ends of this one line. */
        snprintf(t, sizeof(t), "wsprnet: %s\n%d of %d publishable",
                 wsprnet_status(), rpt, all);
        lv_label_set_text(s_lbl_net, t);
    }
}

static void tx_toggle_cb(lv_event_t *e)
{
    (void)e;
    qmx_settings_t st;
    settings_load_all(&st);
    const bool turning_off = st.wspr_tx_en;
    settings_set_wspr_tx_en(!st.wspr_tx_en);
    /* Re-roll which cycle transmits next, so the countdown on this very button
     * is right the moment it is pressed rather than at the next boundary. */
    wspr_rx_tx_schedule_reset(!turning_off, st.wspr_duty_pct);

    /* ⭐ SWITCHING OFF STOPS A BURST THAT IS ON THE AIR (Roy KI0ER, 2026-09-01:
     * "if that button is tapped while actively transmitting, nothing happens and
     * TX continues until the end of the 2 minute cycle... an immediate change to
     * TX OFF is a more expected behaviour").
     *
     * He is right, and it was only ever a wiring gap: run_burst() has checked
     * s_abort_requested at every symbol since WSPR TX was written, and
     * wspr_tx_request_abort() has been public the whole time - nothing called
     * it. Tapping the button set a flag that took effect at the NEXT slot, so a
     * burst already keyed ran its full ~110 s with the button reading ON AIR.
     *
     * The abort keys up through run_burst's own tail (TA0; then RX;), which
     * always runs, so the radio is left receiving rather than stuck keyed.
     *
     * ⛔ The PA voltage is deliberately NOT restored here. It is restored when
     * WSPR is left, and only after the burst has actually stopped - raising the
     * finals' voltage while the radio is still keyed is the exact thing the
     * guard exists to prevent. See wspr_rx_stop(). */
    if (turning_off) {
        char t[64];
        wspr_tx_state_t tst = wspr_tx_get_status(t, sizeof(t), NULL);
        if (tst == WSPR_TX_ACTIVE) {
            ESP_LOGW(TAG, "TX switched off while ON AIR - aborting the burst now");
            wspr_tx_request_abort();
        } else if (tst == WSPR_TX_ARMED) {
            wspr_tx_disarm();
        }
    }
}

/* duty_cycle_cb moved to the settings drawer with its button (2026-08-28). */
static lv_obj_t *s_list;           /* right pane, one label per line */
static lv_obj_t *s_lbl_rows;
static lv_obj_t *s_wf_canvas;

static uint8_t  *s_wf_buf;      /* RGB565 canvas pixels */
static uint8_t  *s_wf_data;     /* WSPR_WF_HIST_ROWS x WSPR_WF_COLS, NEWEST ROW FIRST */
static uint32_t  s_wf_seen;

/* First logging in this file: the dial push is the one thing here that
 * silently changes the radio, so it says what it did and why. */


static int   s_last_spot_count = -1;
static bool  s_rows_miles;        /* the unit the visible rows were formatted in */
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
/* ⭐ THE CALL COLUMN PAYS FOR THE WIDER LEFT PANEL, AND SEVEN IS WHAT FITS.
 *
 * qmx_mono_25 advances exactly 15.0 px per character, so the pane holds
 * RIGHT_W / 15 = 59 characters; the row was 62 and CALL gives up the three.
 * (I first took them from COUNTRY instead, on the grounds that CALL has no
 * graceful fallback. The operator asked twice for CALL, so CALL it is - and
 * MEASURING the font rather than estimating it is what made 7 possible where
 * a guess had said 6.)
 *
 * ⚠ Seven is not arbitrary and it is not free. Every callsign in a live 25-
 * station sample from this bench is six characters or fewer, so the table
 * aligns in practice - but a COMPOUND call (BH4RRG/QRP is ten) still prints in
 * full and pushes that row's later columns right. printf does not truncate,
 * and it must not: a clipped callsign is a different station.
 *
 * ⛔ THE WIDTHS ARE DEFINED ONCE AND THE FORMAT IS BUILT FROM THEM. Every
 * column width used to appear twice - in ROW_FMT and again wherever the field
 * was prepared - and that drift has already caused two bugs in one evening
 * (COUNTRY_W left at 11 when the format went to 7, then the reverse). A
 * stringified constant cannot disagree with itself. */
/* The pane holds RIGHT_W / 15 characters (qmx_mono_25 advances exactly 15 px).
 * Checked against the real widths at first paint - see fmt_header(). Two stale
 * comments in this file claimed 62 and 59 while the row had grown to 63. */
/* LIST_W / 15 = 63 characters (qmx_mono_25 advances exactly 15.0 px). It was
 * 59 against RIGHT_W; the table's 60 px shift left bought four, and BAND
 * giving up its unused fourth column bought the fifth - which is exactly what
 * DT costs including its separating space. */
#define WSPR_ROW_MAX_CHARS  (LIST_W / 15)
/* ⭐ THE JOIN BETWEEN A ROW AND A TRACE (#360). One letter, matching the mark
 * drawn over that station on the waterfall - A is the leftmost mark, B the next
 * and so on, so no legend is needed. Blank for a spot from an earlier cycle:
 * the carpet only holds one cycle, so an older row has no trace to point at and
 * an invented letter would point at the wrong one. First column, because the
 * letters are read left to right on the carpet too. */
#define W_S     1
#define W_UTC   5
/* Which band the spot was HEARD on. Beside UTC because it answers the same kind
 * of question - the circumstances of the hearing, not a property of the station.
 * Blank for spots recorded before the dial was kept (Roy KI0ER, 2026-08-31). */
/* THREE, not four: the longest name in kBands is "160". The fourth column was
 * blank on every row ever printed, and it is one of the five characters the DT
 * column needed - the heading goes to "BND" for it, the same trade DRF already
 * made. */
#define W_BAND  3
#define W_CALL  7
#define W_GRID  4
#define W_CTY   7
#define W_SNR   3
#define W_DRF   2
#define W_TONE  6
#define W_PWR   3
#define W_KM    5
#define W_BRG   3
/* DT in seconds to one decimal, signed: "+1.0", "-0.4". Four is exactly enough
 * for the range WSPR produces and one more than the heading needs. */
#define W_DT    4

#define STRINGIFY2(x) #x
#define STRINGIFY(x)  STRINGIFY2(x)

#define ROW_FMT "%-" STRINGIFY(W_S) "s %-" STRINGIFY(W_UTC)  "s %"  STRINGIFY(W_BAND) "s %-" STRINGIFY(W_CALL) "s %-"                      STRINGIFY(W_GRID) "s %-" STRINGIFY(W_CTY)  "s %"                       STRINGIFY(W_SNR)  "s %"  STRINGIFY(W_DRF)  "s %"                       STRINGIFY(W_TONE) "s %"  STRINGIFY(W_PWR)  "s %"                       STRINGIFY(W_KM)   "s %"  STRINGIFY(W_BRG)  "s %"                       STRINGIFY(W_DT)   "s"

/* Spelled out if it fits, else the DXCC alpha-3. NEVER truncated: "United
 * Stat" is not a country and a clipped name reads as a bug, while USA is
 * simply the shorter true answer. The full name comes from the callsign via
 * dxcc_lookup(), the same source the web panel uses, so the two screens
 * cannot disagree. */
#define COUNTRY_W W_CTY   /* one number, see the widths above */
/* ⛔ TRUNCATE THE NAME, do not fall back to the code (operator, 2026-09-01:
 * "if country names extend over 7 then just cut them off - dont go back to 3
 * letter that can be difficult to decipher").
 *
 * This used to return sp->cty - a 2-3 letter prefix code - whenever the full
 * name did not fit, so narrowing the column to 7 would have turned most rows
 * into codes. A clipped "United " still reads as a place; "K" does not.
 *
 * ⚠ The opposite rule still holds one column to the left, and for a different
 * reason: a truncated CALLSIGN is a DIFFERENT STATION, so CALL is never cut.
 * A country name is a label, not an identity - which is why it may be. */
static const char *country_field(const wspr_spot_t *sp)
{
    static char buf[COUNTRY_W + 1];   /* one row is formatted at a time */
    const char *full = dxcc_lookup(sp->call);
    if (!full || !full[0]) full = sp->cty[0] ? sp->cty : "--";
    snprintf(buf, sizeof(buf), "%.*s", COUNTRY_W, full);
    return buf;
}

static void fmt_row(char *out, size_t n, const wspr_spot_t *sp, const char *utc)
{
    char snr[16], drift[16], hz[16], pwr[16], km[20], brg[16], dt[16];

    /* An unmeasured value prints as a dash, never as a number. WSPR_SNR_UNKNOWN
     * and WSPR_DRIFT_UNKNOWN exist precisely so this cannot quietly become a
     * fabricated measurement - the same rule that deleted the ADIF "599". */
    if (sp->snr_db == WSPR_SNR_UNKNOWN) snprintf(snr, sizeof(snr), "--");
    else snprintf(snr, sizeof(snr), "%+d", sp->snr_db);

    if (sp->drift_hz == WSPR_DRIFT_UNKNOWN) snprintf(drift, sizeof(drift), "--");
    else snprintf(drift, sizeof(drift), "%+d", sp->drift_hz);

    snprintf(hz,  sizeof(hz),  "%.1f", (double)sp->freq_hz);
    snprintf(pwr, sizeof(pwr), "%d", (int)sp->power_dbm);

    /* ⭐ MILES IF THE OPERATOR ASKED FOR MILES (Samuel W7STF: "I have miles
     * selected, but it is showing KM"). The setting has existed since v0.18.6
     * and the FT8 list has honoured it all along; this list simply never
     * looked. The heading follows the same switch - see fmt_header() - because
     * a number in the wrong unit under the right label is worse than either. */
    if (sp->km < 0) snprintf(km, sizeof(km), "--");
    else if (wspr_dist_in_miles())
        snprintf(km, sizeof(km), "%d", (int)lround(sp->km * 0.621371));
    else snprintf(km, sizeof(km), "%d", (int)sp->km);

    if (sp->bearing_deg < 0) snprintf(brg, sizeof(brg), "--");
    else snprintf(brg, sizeof(brg), "%d", (int)sp->bearing_deg);

    /* An unmeasured DT prints as a dash, never as 0.0 - a spot recorded before
       this field existed has no alignment to report, and a fabricated zero
       would read as a perfectly-timed station. Same rule as SNR and drift. */
    if (sp->dt_tenths == WSPR_DT_UNKNOWN) snprintf(dt, sizeof(dt), "--");
    else snprintf(dt, sizeof(dt), "%+.1f", sp->dt_tenths / 10.0);

    const char *bnd = wspr_band_name_for_dial(sp->dial_hz);

    /* The waterfall letter for this station, asked of wspr_rx.c rather than
     * worked out here - the marks are assigned on the device precisely so the
     * Tab5 and the browser cannot number the same cycle differently. It answers
     * only for the cycle currently on the carpet, so an older row gets a space
     * rather than a letter belonging to somebody else. */
    char sch[2] = { wspr_rx_mark_for_freq(sp->freq_hz, sp->cycle_utc), 0 };
    if (!sch[0]) sch[0] = ' ';

    snprintf(out, n, ROW_FMT, sch, utc, bnd ? bnd : "", sp->call, sp->grid,
             country_field(sp), snr, drift, hz, pwr, km, brg, dt);
}

static void fmt_header(char *out, size_t n)
{
    /* CENTRED over each column. printf has no centring conversion, so each
     * heading is padded into a buffer of exactly its column width first - and
     * because it then arrives at ROW_FMT already the right length, the format's
     * own left/right alignment cannot move it again.
     *
     * "TONE" rather than "HZ": every column here is a number in some unit, so
     * "HZ" named the unit while the others name the quantity. What the column
     * holds is the station's audio tone within the 200 Hz window. */
    char h[13][16];   /* 13 columns since S was added - keep in step with raw[]/w[] */
    /* "M" for metres - the values are bare band numbers (160, 40, 20, 17, 10),
     * so the unit belongs in the heading and not repeated on every row. */
    const char *raw[13] = { "S", "UTC", "BND", "CALL", "GRID", "COUNTRY", "SNR",
                            "DR", "TONE", "PWR", wspr_dist_in_miles() ? "MI" : "KM", "BRG", "DT" };
    const int   w[13]   = { W_S, W_UTC, W_BAND, W_CALL, W_GRID, W_CTY, W_SNR,
                            W_DRF, W_TONE, W_PWR, W_KM, W_BRG, W_DT };
    /* ⭐ BIAS THE HEADING THE WAY ITS DATA IS ALIGNED (operator, 2026-09-01:
     * "KM header should be moved one character right to centre properly above
     * the column").
     *
     * With an ODD amount of padding a heading cannot sit dead centre, so it
     * leans one way - and `pad / 2` rounded DOWN, leaning every heading LEFT.
     * Over a RIGHT-aligned numeric column that is the wrong way: the digits
     * gather at the right edge while the title drifts left of them. KM is the
     * clearest case (5 wide, 2 letters, 3 to share) but SNR, DRF, TONE, PWR and
     * BRG all lean the same wrong way.
     *
     * So the lean follows the data: left-aligned text columns keep the left
     * bias, right-aligned numeric ones take the right. Nothing is nudged by
     * hand - which matters here, because the hand-spaced header is exactly what
     * drifted out of step with the rows before ROW_FMT was made to serve both. */
    /* BAND is right-aligned with the other numbers. */
    const bool right_aligned[13] = { false, false, true, false, false, false,
                                     true, true, true, true, true, true, true };
    /* ⛔ A HEADING LONGER THAN ITS COLUMN SILENTLY WIDENS THE ROW. printf does
     * not truncate, so an over-long title pushes every later column right and
     * the last one off the pane - invisible in code review, obvious only on
     * glass. It bit TWICE inside ten minutes on 2026-09-01 ("COUNTRY" over a
     * 6-wide column, then "DRF" over a 2-wide one), which is twice more than a
     * check this cheap should have allowed. Once per boot, not per row. */
    {
        static bool checked = false;
        if (!checked) {
            checked = true;
            int total = 12;   /* the single spaces between 13 columns */
            for (int i = 0; i < 13; i++) {
                total += w[i];
                if ((int)strlen(raw[i]) > w[i])
                    ESP_LOGE(TAG, "column %d: heading '%s' is %d chars in a %d "
                                  "wide column - the row will overflow",
                             i, raw[i], (int)strlen(raw[i]), w[i]);
            }
            if (total > WSPR_ROW_MAX_CHARS)
                ESP_LOGE(TAG, "spot row is %d chars but the pane holds %d - the "
                              "right-hand column(s) are off screen",
                         total, WSPR_ROW_MAX_CHARS);
        }
    }
    for (int i = 0; i < 13; i++) {
        const int len  = (int)strlen(raw[i]);
        const int pad  = w[i] > len ? w[i] - len : 0;
        /* ⭐ THE HEADING IS ALIGNED THE SAME WAY ITS DATA IS - not centred.
         *
         * Centring was wrong and the operator saw it at once: over a LEFT-
         * aligned column the data starts at the left edge while a centred
         * title floats in the middle, so the two never line up. CALL was the
         * worst - " CALL  " sitting over "OZ1LAV ". Leaning the centring one
         * way or the other (what this did first) only chooses which mismatch
         * you get.
         *
         * Aligning the heading exactly as the column aligns its values makes
         * the title sit ON the data by construction, whatever the widths
         * later become. */
        const int left = right_aligned[i] ? pad : 0;
        int k = 0;
        for (int j = 0; j < left && k < (int)sizeof(h[i]) - 1; j++) h[i][k++] = ' ';
        for (int j = 0; raw[i][j] && k < (int)sizeof(h[i]) - 1; j++) h[i][k++] = raw[i][j];
        for (int j = 0; j < pad - left && k < (int)sizeof(h[i]) - 1; j++) h[i][k++] = ' ';
        h[i][k] = '\0';
    }
    snprintf(out, n, ROW_FMT, h[0], h[1], h[2], h[3], h[4],
             h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12]);
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
    /* ⛔ THE SIZE IS MEASURED, NOT GUESSED - and the guess was wrong.
     *
     * This started at 28 under a comment asserting that "MODE: WSPR" could not
     * fit at 48 because it is one character longer than the FT8 page's
     * "MODE: FT8". That was an ESTIMATE (~296 px against 288 available) written
     * as if it were a fact, and it cost the page a header two sizes smaller
     * than every other mode's for no established reason. The operator asked why
     * it looked odd next to the other pages, which was the right question.
     *
     * It also compared against the wrong budget. The controls below use a 16 px
     * margin, but a title is not a control and need not share it: the decode
     * list starts at RIGHT_X (LEFT_W + 8), so the header can have the panel's
     * full width and still clear it.
     *
     * So: try the big font, ASK LVGL how wide the text actually is, and step
     * down only if it genuinely does not fit. Same pattern as the bottom bar's
     * version label in ui.c. That way this page matches the others whenever it
     * can, and can never spill into the CALL column when it cannot. */
    /* 48, the same as every other mode page, and it DOES NOT FIT in the panel.
     *
     * Measured at runtime rather than guessed: "MODE: WSPR" renders about
     * 336 px at 48 pt against this panel's 320. "MODE: FT8" is one character
     * shorter, ~302 px, which is exactly why the other pages fit and this one
     * cannot. Two alternatives were built and rejected by the operator - a
     * smaller font, and splitting "MODE:" off at 20 so only the name was large.
     * His call, made with the constraint in front of him: consistency with the
     * other pages matters more than the overlap, "if it then lap over the wf
     * window then so be it".
     *
     * So it is foregrounded deliberately. The waterfall canvas is created after
     * this label and LVGL draws siblings in creation order, so without this the
     * title would be drawn UNDER the waterfall and simply disappear - the
     * opposite of what was asked for. The overlap is ~24 px into the top-left
     * corner of the waterfall, which carries the 1350 Hz edge of the window. */
    lv_label_set_text(s_lbl_title, "MODE: WSPR");
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_pos(s_lbl_title, 16, 4);
    lv_obj_move_foreground(s_lbl_title);

    /* The dial, boxed like the FT8 page's preset. Read-only for now: the
     * standard-dial picker is the next piece (see docs/wspr-ui-design.md - a
     * free-entry keypad is deliberately NOT wanted, because every band has one
     * canonical WSPR frequency and anything else is simply not in the
     * sub-band). */
    /* ⛔ NO WRAPPER BOX. The dropdown used to be centred inside an lv_obj of the
     * SAME colour that also had its own 1 px border and its own default
     * padding - so the control rendered as a rounded box inset inside a second
     * rounded box, with the chevron floating in the gap between them. That is
     * the "strange" band button: two frames where the design has one, and an
     * inner control narrower than everything below it.
     *
     * A dropdown is already a styleable box. Styling it directly gives one
     * frame, full panel width, and the same left edge as the cycle bar and the
     * TX buttons underneath. */
    /* ⭐ A BUTTON THAT OPENS A DRAG-TO-PICK LIST, not an lv_dropdown
     * (operator, 2026-09-07). See bp_open() for why the gesture matters: a
     * dropdown commits on whatever cell your finger lifts over, with nothing
     * shown first and no way to change your mind. Here the highlight follows
     * the finger and only the release commits.
     *
     * Left edge is EX_W at EX_X, aligned with everything else in the panel. It
     * was shifted right in v1.10.5 to clear the 30 px edge-swipe strip, but the
     * control genuinely at risk there was the TX BUTTON, sitting across the
     * middle of the left edge where a hand reaches for the page-swipe grip -
     * and that moved to the bottom. A control near the TOP is not on the path
     * of that gesture. */
    s_navail = wspr_bands_available(s_avail, (int)sizeof(s_avail));
    s_btn_dial = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_dial, EX_W, 56);
    lv_obj_set_pos(s_btn_dial, EX_X, 70);
    lv_obj_set_style_radius(s_btn_dial, 8, 0);
    lv_obj_set_style_border_width(s_btn_dial, 1, 0);
    lv_obj_set_style_bg_color(s_btn_dial, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_border_color(s_btn_dial, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_add_event_cb(s_btn_dial, bp_button_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_dial = lv_label_create(s_btn_dial);
    lv_obj_set_style_text_font(s_lbl_dial, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_dial, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(s_lbl_dial, LV_ALIGN_LEFT_MID, 8, 0);
    /* Names the STORED dial, not entry 0. A screenshot caught the old control
       sitting on "160 m" while the stored dial was 20 m, because a wedged radio
       makes cat_get_frequency() return 0 and the tick's sync never runs. A
       control that displays a band it is not set to is worse than a blank one. */
    bp_button_refresh();

    /* The cycle: plain language above, one 120 s bar below. It orients - "am I
     * receiving, how long left" - rather than urging, because nothing in WSPR
     * needs a decision inside the cycle. */
    s_lbl_cycle = lv_label_create(s_container);
    lv_label_set_text(s_lbl_cycle, "starting...");
    lv_obj_set_style_text_font(s_lbl_cycle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_cycle, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_pos(s_lbl_cycle, 16, 136);

    s_bar_cycle = lv_bar_create(s_container);
    lv_obj_set_size(s_bar_cycle, LEFT_W - 32, 10);
    lv_obj_set_pos(s_bar_cycle, 16, 170);
    lv_bar_set_range(s_bar_cycle, 0, 120);
    lv_bar_set_value(s_bar_cycle, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_PRIMARY), LV_PART_INDICATOR);

    s_lbl_status = lv_label_create(s_container);
    lv_label_set_text(s_lbl_status, "");
    /* 22, not 18 - this line carries the capture and decode progress and was
     * the smallest text on the page. */
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_pos(s_lbl_status, 16, 192);

    s_lbl_heard = lv_label_create(s_container);
    lv_label_set_text(s_lbl_heard, "Heard nothing yet");
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_lbl_heard, 16, 224);

    /* TX and Duty, side by side.
     *
     * A WSPR transmission keys the radio for 110 SECONDS - eight times an FT8
     * burst. This project's rule for controls that key the radio (written for
     * SWR Tune) is that they must be impossible to trigger by accident and
     * visibly ACTIVE while engaged, which is why TX is a labelled toggle
     * reading OFF/ON rather than a one-tap "transmit". */
    /* FULL WIDTH: the Duty button used to share this row, and moved to the
     * settings drawer (operator, 2026-08-28). Duty is a policy chosen once for
     * a session - "how much of the time may this thing transmit" - while TX
     * ON/OFF is the control reached during one. Splitting them puts the
     * decision where it belongs and gives the one live control the whole row. */
    /* ⛔ CLEAR OF THE PAGE-SWIPE STRIP. At x=16 this button's first 14 px sat
     * INSIDE the left edge-swipe zone (EDGE_SWIPE_ZONE_PX = 30, x 0..30), the
     * gesture used to reach the panadapter - so a swipe that began a little
     * high could land on a control that keys the radio for 110 seconds.
     * Reported by Randy N4OPI, 2026-08-31: "it is somewhat easy to accidentally
     * turn on transmit when trying to switch pages to the Pandapter."
     *
     * That is not the operator being careless, it is two hit areas overlapping.
     * Starting at 40 puts the whole button outside the strip with 10 px to
     * spare, and the RIGHT edge moves out to match so the control keeps its
     * width - the row simply begins where the swipe zone ends. Moving the left
     * edge without the right just made the button smaller and left 24 px of
     * dead space against the right pane, which the operator spotted at once.
     *
     * ⚠ Any control added to this pane must clear 30 px too. The rule this
     * project already has for radio-keying controls - impossible to trigger by
     * accident - is not satisfied by a confirmation label if the thing can be
     * hit by a gesture aimed at something else entirely.
     *
     * ⭐ AND CLEARING THE STRIP IN X WAS NOT ENOUGH. Roy reported the same
     * accident after that fix shipped: the button was at y=258 of a 624 px
     * panel - across the middle - and the middle of the left edge is where the
     * hand goes for the swipe grip. It now sits at the bottom (EX_TX_Y); see
     * the layout note beside EX_DX_Y. */
    s_btn_tx = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_tx, EX_W_LOW - 24, 56);
    lv_obj_set_pos(s_btn_tx, 40, EX_TX_Y);
    lv_obj_set_style_radius(s_btn_tx, 8, 0);
    lv_obj_add_event_cb(s_btn_tx, tx_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_tx = lv_label_create(s_btn_tx);
    lv_label_set_text(s_lbl_tx, "TX  OFF");
    /* 28, not 20: these are 56 px buttons and the label was sitting in the
     * middle of one looking like a caption. The panel is 372 px wide now, so
     * each half is ~166 px - "TX  OFF" at 28 pt is ~110 px and still fits. */
    lv_obj_set_style_text_font(s_lbl_tx, &lv_font_montserrat_28, 0);
    lv_obj_center(s_lbl_tx);

    build_left_extras();

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

        /* ⛔ A FROZEN CARPET LOOKS EXACTLY LIKE A HUNG ONE. The dashed boundary
         * exists for that same reason (see WSPR_WF_MARK), so a state that stops
         * the whole pane advancing gets its own caption rather than relying on
         * the operator remembering which button they last pressed. Top-right,
         * clear of the boundary time at the left end. */
        s_lbl_held = lv_label_create(s_container);
        lv_label_set_text(s_lbl_held, "HELD");
        lv_obj_set_style_text_font(s_lbl_held, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(s_lbl_held, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_color(s_lbl_held, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
        lv_obj_set_style_bg_opa(s_lbl_held, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_lbl_held, 3, 0);
        lv_obj_set_style_radius(s_lbl_held, 3, 0);
        lv_obj_set_pos(s_lbl_held, RIGHT_X + RIGHT_W - 70, WF_Y + 4);
        lv_obj_clear_flag(s_lbl_held, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_lbl_held, UI_FLAG_NOT_HOT);
        lv_obj_add_flag(s_lbl_held, LV_OBJ_FLAG_HIDDEN);
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
    /* ⛔ LIST_X, NOT RIGHT_X. The header sits over the rows, so it moves with
       the TABLE and not with the waterfall. Left at RIGHT_X it was indented
       60 px past its own columns and its last heading fell off the pane - which
       is exactly how DT arrived with data in every row and no title over it. */
    /* The hover readout, and its own timer. Created last so nothing built after
       it can end up on top; re-foregrounded on each show in any case. */
    s_hover_lbl = lv_label_create(s_container);
    lv_label_set_text(s_hover_lbl, "");
    lv_obj_add_flag(s_hover_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hover_lbl, UI_FLAG_NOT_HOT);      /* a readout, not a control */
    lv_obj_clear_flag(s_hover_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_hover_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_hover_lbl, LV_OPA_80, 0);
    lv_obj_set_style_border_color(s_hover_lbl, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(s_hover_lbl, 1, 0);
    lv_obj_set_style_radius(s_hover_lbl, 6, 0);
    lv_obj_set_style_pad_all(s_hover_lbl, 6, 0);
    lv_obj_set_style_text_font(s_hover_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_hover_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_timer_create(hover_tick_cb, HOVER_PERIOD, NULL);

    s_lbl_hdr = lv_label_create(s_container);
    lv_obj_set_style_text_font(s_lbl_hdr, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(s_lbl_hdr, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(s_lbl_hdr, LIST_X, LIST_Y);
    s_hdr_built = false;          /* a new label - ours has not been written yet */
    wspr_header_refresh();

    s_list = lv_obj_create(s_container);
    lv_obj_set_size(s_list, LIST_W, MID_H - LIST_Y - 34);
    lv_obj_set_pos(s_list, LIST_X, LIST_Y + 30);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    /* SCROLLABLE, vertically only. The ring holds far more than a screenful
     * and the operator wants to reach all of it, the way the FT8 list works.
     * Horizontal scrolling is off: the table is sized to the pane, so sideways
     * travel would only ever be a way to lose the columns off the edge.
     * NOT_HOT because this is a surface you drag, not a control you press -
     * nothing here is tappable (a WSPR spot is a measurement, not a station to
     * work). */
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
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

    /* ⛔ RE-FOREGROUNDED HERE, AT THE END, AND THAT IS THE WHOLE POINT.
     * lv_obj_move_foreground() only lifts a child above the siblings that
     * exist WHEN IT RUNS - and the title is created near the top of this
     * function while the waterfall canvas is created near the bottom. So the
     * call beside the title was undone by every object built after it, and the
     * operator saw the "R" of WSPR disappear behind the waterfall. Raising it
     * once more, after the last sibling exists, is what actually puts it in
     * front. */
    lv_obj_move_foreground(s_lbl_title);
}

void wspr_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_container);
    /* ⛔ AND THEN PUT THE EDGE STRIPS BACK ON TOP. This container is a
     * near-full-screen opaque pane, so foregrounding it buries them - which is
     * exactly why the swipe out of WSPR did nothing. CLAUDE.md already carried
     * this warning for the FT8 view. */
    ui_raise_edge_strips();
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
/* ---- Waterfall letter markers (#360): LVGL LABELS, not a bitmap -------
 *
 * The first version blitted a hand-drawn 5x7 font straight into the pixel
 * buffer, to honour the direct-buffer rule at the top of repaint_waterfall().
 * The operator's verdict was fair: "the font used is very coarse... can you
 * just use the same font as in the decoded lines?"
 *
 * ⭐ THE RULE FORBIDS DRAW CALLS INSIDE THE PIXEL LOOP, WHICH IS NOT THE SAME
 * AS FORBIDDING OBJECTS. What it protects against is per-pixel work on
 * taskLVGL - lv_canvas_set_px() over 188,800 pixels. A FIXED set of at most 21
 * labels, created only when the mark set changes (once every two minutes) and
 * repositioned as the carpet scrolls (~1.5 Hz), is a different order of cost
 * entirely: roughly 30 lv_obj_set_pos() calls a second, against the 4.7 Mpx/s
 * the waterfall already invalidates.
 *
 * ⚠ It is also NOT the vertical-callsign idea that was rejected. That needed
 * one label PER CHARACTER, rebuilt continuously, ~48 objects; this is one
 * label per mark, reused until the cycle changes.
 *
 * They use qmx_mono_25 - the SAME font as the decode list directly below,
 * so a letter on the carpet and its letter in the S column are visibly the
 * same character. Each carries a black background plate, which reads better
 * over a bright trace than the hand-drawn outline it replaces. */
#define MARK_LBL_MAX WSPR_MARKS_MAX
static lv_obj_t *s_mark_lbl[MARK_LBL_MAX];
static int       s_mark_lbl_x[MARK_LBL_MAX];
static int       s_mark_lbl_n;
static lv_obj_t *s_mark_time_lbl;
/* What a mark occupies for the de-crowding test: qmx_mono_25 advances exactly
 * 15.0 px, plus 2 px of plate padding either side. */
#define MARK_W 19
/* Half the width a WSPR transmission occupies: 4-FSK, 1.4648 Hz between tones,
 * so 3 spacings from the lowest tone to the highest and the centre is 1.5 of
 * them above the base tone the decoder reports. */
#define WSPR_TX_HALF_WIDTH_HZ 2.2f

/* ⛔ THE TIME AND THE LETTERS HAVE DIFFERENT LIFETIMES, so they are cleared
 * separately. The time is a property of the LINE and is known the instant the
 * line is drawn; the letters are a property of the DECODE, which lands most of
 * a cycle later. Clearing both together is what made the time wait for a
 * decode it does not depend on. */
static void mark_letters_clear(void)
{
    for (int i = 0; i < s_mark_lbl_n; i++) {
        if (s_mark_lbl[i] && lv_obj_is_valid(s_mark_lbl[i])) lv_obj_del(s_mark_lbl[i]);
        s_mark_lbl[i] = NULL;
    }
    s_mark_lbl_n = 0;
}

static void mark_time_clear(void)
{
    if (s_mark_time_lbl && lv_obj_is_valid(s_mark_time_lbl)) lv_obj_del(s_mark_time_lbl);
    s_mark_time_lbl = NULL;
}

static lv_obj_t *mark_label_new(const char *txt, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(s_container);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    /* The plate. Not fully opaque: it must stay readable over a bright trace
     * without hiding the trace it is pointing at. */
    lv_obj_set_style_bg_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(l, 2, 0);
    lv_obj_set_style_radius(l, 3, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    /* ⛔ NOT_HOT: these sit over the waterfall and a mouse must not turn green
     * on them - they are a caption, not a control (see ui_theme.h). */
    lv_obj_add_flag(l, UI_FLAG_NOT_HOT);
    return l;
}

/* ---- Hold (#370) ----
 *
 * The pane shows WSPR_WF_VIEW_ROWS of a WSPR_WF_HIST_ROWS ring, so it can be
 * pointed at rows other than the newest. Hold freezes it on whatever was
 * newest when it was pressed and lets the capture keep writing behind it.
 *
 * The offset is derived from a ROW COUNT, not accumulated per repaint: a
 * repaint that is skipped, or run twice, must not shift the view, and a
 * difference of two counter readings cannot drift. */
/* ⚠ CLAMPED, and the clamp is a real limit rather than a formality. Past
 * WSPR_WF_HIST_ROWS - WSPR_WF_VIEW_ROWS the held rows have been overwritten by
 * the capture, so there is nothing left to hold; the view then slides forward
 * on its own rather than presenting rows that are quietly no longer the ones
 * that were frozen. At two cycles that is one full extra cycle of dwell. */
static int wf_hold_top(void)
{
    if (!s_wf_hold) return 0;
    const int max_back = WSPR_WF_HIST_ROWS - WSPR_WF_VIEW_ROWS;
    int back = (int)(wspr_rx_wf_rows_total() - s_wf_hold_rows0);
    if (back < 0) back = 0;
    if (back > max_back) back = max_back;
    return back;
}

static void repaint_waterfall(void)
{
    if (!s_wf_canvas || !s_wf_data || !s_wf_buf) return;
    uint16_t *px = (uint16_t *)s_wf_buf;

    s_wf_top = wf_hold_top();

    /* Column map precomputed once instead of a divide per pixel. The row map
     * is rebuilt only when the view actually moves, which in the normal live
     * case is never. */
    static uint16_t colmap[RIGHT_W];
    static uint16_t rowmap[WF_H];
    static int      rowmap_top = -1;
    for (int x = 0; x < RIGHT_W; x++) colmap[x] = (uint16_t)(x * WSPR_WF_COLS / RIGHT_W);
    /* out row 0 is the NEWEST row, so display y maps straight through and the
     * newest data lands at the top - the panadapter's convention.
     *
     * ⛔ VIEW_ROWS, NOT HIST_ROWS. The pane shows one cycle at 1.14 px/row and
     * that must not change because the ring got deeper - mapping the whole
     * ring into 200 px is the 0.57 px/row crawl wspr_rx.h warns about. */
    if (rowmap_top != s_wf_top) {
        rowmap_top = s_wf_top;
        for (int y = 0; y < WF_H; y++)
            rowmap[y] = (uint16_t)(s_wf_top + y * WSPR_WF_VIEW_ROWS / WF_H);
    }

    for (int y = 0; y < WF_H; y++) {
        const uint8_t *src = &s_wf_data[rowmap[y] * WSPR_WF_COLS];
        uint16_t *dst = &px[y * RIGHT_W];
        for (int x = 0; x < RIGHT_W; x++) dst[x] = wf_rgb565(src[colmap[x]]);
    }

    /* ---- letter markers (#360) ----
     *
     * ⭐ POSITIONED FROM THE HISTORY, NEVER BURNED INTO IT. The pixel loop
     * above rewrites the whole canvas from s_wf_data every time, so anything
     * written into that buffer is erased on the next repaint. The marks live in
     * wspr_rx.c; their y is derived here from the same row map the carpet uses,
     * so they scroll with their own cycle and age off the bottom for free.
     *
     * The anchor is the cycle-boundary line, found by scanning the display
     * buffer for the dashed marker rather than by counting rows.
     *
     * ⛔ BELOW THE LINE, NEVER ON IT (operator, 2026-09-08: "right now you
     * write on top of the dashed line and one can be in doubt what to assign
     * them to"). A boundary is BETWEEN two cycles, so a glyph straddling it
     * belongs to neither. The marks describe the cycle BELOW:
     * wf_mark_boundary() runs after a cycle's rows are published and row 0 is
     * the newest, so the line closes off the data beneath it. The row also
     * carries that cycle's own UTC time at its left end, so it can be matched
     * to a UTC group in the list without counting boundaries.
     *
     * ⚠ If the boundary has scrolled far enough down that the row will not fit
     * beneath it, the labels are HIDDEN. The alternative is clamping, which
     * puts them back above the line - onto the wrong cycle, silently, exactly
     * when they are hardest to check.
     *
     * ⛔⛔ AND THE SAME LIE ARRIVED BY A SECOND ROUTE, WHICH THE PARAGRAPH
     * ABOVE DID NOT COVER (operator screenshots, 2026-09-09). The scan below
     * finds the FIRST mark row from the top, which is always the NEWEST
     * boundary - but the marks in hand are whatever last finished DECODING,
     * and a window is decoded while the next one is already recording. So for
     * roughly the first 80 s of every cycle the letters and the time sat under
     * a line belonging to a different cycle, then jumped when the decode
     * landed. Three consecutive frames caught the whole sequence: 14:36 marks
     * under the line closing 14:38, then 14:38 correctly, then 14:38 again
     * under the line closing 14:40 with its own rows already scrolled off.
     * WSPR_WF_CYCLES is 1, so there is only ever ONE line on screen and there
     * is no older one to move them to - the honest answer is to draw them only
     * while the line on screen is their own, and otherwise not at all.
     *
     * ⭐ THE TIME IS NOT SUBJECT TO ANY OF THAT and is now drawn from
     * wspr_rx_boundary_cycle() the moment the line appears (operator: "the
     * timestamp can be printed as soon as the dashed line is visible ... and
     * do not need to wait for the stations to be decoded"). It labels the
     * LINE, which knows its own cycle at the instant it is drawn; only the
     * letters wait for the decoder. Hence two rebuild triggers and two clear
     * helpers, not one. */
    {
        static wspr_mark_t marks[WSPR_MARKS_MAX];
        static int         nmarks = 0;
        static int64_t     marks_cycle_seen = -1;
        static int64_t     line_cycle_seen  = -1;

        /* ⛔ THE LINE IN THE PANE IS NOT NECESSARILY THE NEWEST ONE. Under Hold
         * the view is looking further back, so which cycle the visible line
         * closes has to be worked out rather than asked for.
         *
         * Boundaries are laid down one per cycle in order, so the k-th from the
         * newest closes wspr_rx_boundary_cycle() - k * 120. Counting them is
         * exact and needs no extra state; deriving k from a row number would
         * not be, because a cycle occupies WSPR_WF_ROWS + WSPR_WF_MARK_ROWS
         * rows, not WSPR_WF_ROWS. A marker is WSPR_WF_MARK_ROWS thick, so a run
         * of marked rows counts once. */
        int mark_row = -1, mark_ord = -1;
        {
            int k = -1;
            bool prev_mark = false;
            for (int r = 0; r < WSPR_WF_HIST_ROWS; r++) {
                const bool m = (s_wf_data[(size_t)r * WSPR_WF_COLS] == WSPR_WF_MARK);
                if (m && !prev_mark) k++;
                prev_mark = m;
                if (m && r >= s_wf_top) { mark_row = r; mark_ord = k; break; }
            }
        }
        const int line_y = (mark_row >= 0)
                         ? (mark_row - s_wf_top) * WF_H / WSPR_WF_VIEW_ROWS : -1;
        const int64_t newest_cycle = wspr_rx_boundary_cycle();
        const int64_t line_cycle   = (mark_ord >= 0 && newest_cycle > 0)
                                   ? newest_cycle - (int64_t)mark_ord * 120 : 0;

        /* The letters follow the LINE, and BOTH triggers are needed.
         *
         * The line changing is the obvious one - Hold can leave it on an older
         * cycle while newer decodes arrive for cycles that are not on screen.
         * The other is a decode landing for the cycle ALREADY shown, which is
         * the ordinary case: the line is drawn at the boundary and its marks
         * turn up ~40 s later, with line_cycle unchanged throughout. Keying off
         * the line alone would mean the letters never appeared at all. */
        static uint32_t marks_seq_seen = 0xFFFFFFFFu;
        const uint32_t  marks_seq = wspr_rx_marks_seq();
        if (line_cycle != marks_cycle_seen || marks_seq != marks_seq_seen) {
            marks_cycle_seen = line_cycle;
            marks_seq_seen   = marks_seq;
            const int prev = nmarks;
            nmarks = wspr_rx_get_marks_for_cycle(line_cycle, marks, WSPR_MARKS_MAX);
            /* Only tear the labels down when the set actually changed - this
             * runs on every decode publish, including ones for other cycles. */
            if (nmarks != prev || nmarks == 0) mark_letters_clear();
        }
        const bool fresh = (s_mark_lbl_n == 0 && nmarks > 0);
        const bool line_is_theirs = (nmarks > 0);
        const int row_h  = lv_font_get_line_height(&qmx_mono_25);
        const bool room  = (line_y >= 0) && (line_y + 3 + row_h <= WF_H);

        /* Rebuild only when the cycle changes - every two minutes, not every
         * repaint. Positions are updated below on every repaint, which is the
         * cheap half. */
        if (fresh) {
            mark_letters_clear();
            /* ⛔ A MARK MUST NOT BE MOVED AWAY FROM ITS OWN TONE. An earlier
             * version pushed a crowded mark right until it fitted, which on a
             * quiet band produced exactly what the operator saw: the candidate
             * cap is 20 and it saturates, so twenty '?' were shoved into one
             * continuous run of punctuation pointing at nothing in particular.
             * A marker whose position is a lie is worse than a missing one.
             *
             * DECODES ARE PLACED FIRST and never dropped - they are the join to
             * the S column and there are only ever a handful. The '?' marks
             * then fill whatever room is left, each at its true tone or not at
             * all. */
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < nmarks && s_mark_lbl_n < MARK_LBL_MAX; i++) {
                    const bool decoded = (marks[i].ch != '?');
                    if (decoded != (pass == 0)) continue;
                    /* ⭐ CENTRE THE MARK ON THE TRANSMISSION, NOT ON ITS
                     * LOWEST TONE. A WSPR signal is 4-FSK at 1.4648 Hz
                     * spacing, so it occupies 3 x 1.4648 = 4.4 Hz and the
                     * decoder reports the BASE tone - which put every mark on
                     * the left-hand edge of a trace about 20 px wide rather
                     * than on it. Operator, 2026-09-08: "even the real signals
                     * are marked strange places compared to the signals you can
                     * truly see."
                     *
                     * Measured before changing anything, by profiling the
                     * column energy of a real screenshot against the decoded
                     * tones: IK6ZEW decoded 1480.2 and peaked at 1484.2,
                     * E79Q decoded 1548.0 and peaked at 1544.7. Scatter either
                     * way and no systematic bias, i.e. the x mapping itself was
                     * right and the width was the whole story.
                     *
                     * ⚠ Applied to the DRAWING only. marks[i].freq_hz stays the
                     * decoder's own figure, because that is what the S column
                     * matches a spot by and what the log reports. */
                    const float centre_hz = marks[i].freq_hz + WSPR_TX_HALF_WIDTH_HZ;
                    int x = (int)((centre_hz - WSPR_WF_LO_HZ) * (float)RIGHT_W /
                                  (WSPR_WF_HI_HZ - WSPR_WF_LO_HZ)) - MARK_W / 2;
                    bool clash = false;
                    for (int k = 0; k < s_mark_lbl_n; k++)
                        if (x < s_mark_lbl_x[k] + MARK_W && s_mark_lbl_x[k] < x + MARK_W)
                            { clash = true; break; }
                    /* A decode is drawn regardless - two letters overlapping is
                     * ugly, losing one breaks the join to the list. */
                    if (clash && !decoded) continue;
                    if (x < 0) x = 0;
                    if (x + MARK_W > RIGHT_W) x = RIGHT_W - MARK_W;
                    char t[2] = { marks[i].ch, 0 };
                    /* Light green for a decode, matching the boundary line it
                     * belongs to; dim grey for a candidate that did not decode,
                     * which is a question rather than a result. */
                    lv_obj_t *l = mark_label_new(t, decoded ? 0x90EE90 : 0x969696);
                    if (!l) break;
                    s_mark_lbl_x[s_mark_lbl_n] = x;
                    s_mark_lbl[s_mark_lbl_n++] = l;
                }
            }
        }

        /* Rebuilt when the LINE changes, which is once every two minutes and
         * independent of the decoder. */
        /* Also rebuilt if the label went away underneath us: it is now the only
         * thing keyed off the cycle number, so a lost object would otherwise
         * never come back - the condition that created it would stay false. */
        const bool time_gone = (line_cycle > 0) &&
                               (!s_mark_time_lbl || !lv_obj_is_valid(s_mark_time_lbl));
        if (line_cycle != line_cycle_seen || time_gone) {
            line_cycle_seen = line_cycle;
            mark_time_clear();
            if (line_cycle > 0) {
                time_t tt = (time_t)line_cycle;
                struct tm tmv;
                gmtime_r(&tt, &tmv);
                char ts[8];
                snprintf(ts, sizeof(ts), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
                s_mark_time_lbl = mark_label_new(ts, 0xC8C8C8);
            }
        }

        /* ⛔ line_is_theirs, not just room: a letter under someone else's cycle
         * is a false statement about which two minutes heard that station, and
         * the whole point of putting it on the carpet is that its position
         * means something. Kept alive and merely hidden, so it reappears the
         * moment its own line is the current one rather than waiting for the
         * next decode. */
        for (int i = 0; i < s_mark_lbl_n; i++) {
            if (!s_mark_lbl[i] || !lv_obj_is_valid(s_mark_lbl[i])) continue;
            if (!room || !line_is_theirs) {
                lv_obj_add_flag(s_mark_lbl[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(s_mark_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_mark_lbl[i], RIGHT_X + s_mark_lbl_x[i], WF_Y + line_y + 3);
        }
        if (s_mark_time_lbl && lv_obj_is_valid(s_mark_time_lbl)) {
            if (!room) lv_obj_add_flag(s_mark_time_lbl, LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_clear_flag(s_mark_time_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_mark_time_lbl, RIGHT_X + 2, WF_Y + line_y + 3);
            }
        }
    }
    lv_obj_invalidate(s_wf_canvas);
}

void wspr_screen_view_tick(void)
{
    /* ⭐ BEFORE THE VISIBILITY GUARD, AND THAT IS THE WHOLE FIX (Dirk DK7CVD,
     * 2026-09-08: "it nearly always transmits on 40m and seldom on the other
     * two").
     *
     * Band hopping RETUNES THE RADIO. It was being driven from
     * refresh_left_extras(), i.e. from a screen repaint - so it only happened
     * while the WSPR page was the one on display. Swipe to the panadapter and
     * hopping silently stopped, while WSPR itself carried on transmitting on
     * whatever band it was left on. A radio action must not depend on which
     * screen the operator is looking at.
     *
     * ⚠ Everything below this line still belongs to the page and stays behind
     * the guard - hop_maybe() is the only thing here that acts on the world
     * rather than on pixels. It is safe on a hidden page: its own writes are
     * settings + CAT, and the one UI call it makes (bp_button_refresh) returns
     * immediately when the page has not been built. */
    hop_maybe();

    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    /* After the visibility guard: these read the spot store under its mutex and
     * walk a 256-entry ring, which is pure cost on a page nobody is looking at. */
    refresh_left_extras();

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
                    s_dial_settle_us = esp_timer_get_time() + 3000000;
                    ESP_LOGW(TAG, "dial: pushed %lu Hz to the radio (was %lu)",
                             (unsigned long)want, (unsigned long)have);
                } else if (want) {
                    ESP_LOGI(TAG, "dial: radio already on %lu Hz", (unsigned long)want);
                }
                s_dial_push_left = 0;    /* one shot - never fight manual tuning */
            }
        }

        /* ⛔ AND SAY SO IF THEY EVER DISAGREE AGAIN.
         *
         * The push above is deliberately one-shot, so that this page never
         * fights a deliberate manual tune. The consequence is that if the radio
         * moves afterwards, WSPR carries on decoding whatever is arriving and
         * files every spot against `wspr_dial_hz` - the SETTING, which
         * wspr_rx_cycle_dial_hz() reads, not the radio. So the band on each
         * spot is simply wrong, and it is wrong silently.
         *
         * Found the hard way on 2026-09-08: the WSPR band button had drifted
         * under the top bar's Band hit zone, one tap opened the PANADAPTER's
         * band list, and the radio went to 1.840 MHz while this page kept
         * capturing and labelling everything 40 m. That cause is fixed in
         * ui.c, but the silence was the part that made it hard to see.
         *
         * Change-detected, so it says it once per disagreement rather than
         * every second. Deliberately a warning and not a correction: which of
         * the two is right is the operator's to decide, and this page must not
         * start yanking the dial back from under them. */
        {
            static uint32_t s_last_mismatch = 0;
            qmx_settings_t ms;
            settings_load_all(&ms);
            const uint32_t want = ms.wspr_dial_hz;
            const uint32_t have = cat_get_frequency();
            /* ⚠ NOT WHILE OUR OWN PUSH IS STILL IN FLIGHT. cat_get_frequency()
             * reports the last FA POLL, which lags a write by up to ~150 ms, so
             * without this the check fires on the push it was triggered by and
             * cries mismatch on every entry to this page - observed doing
             * exactly that, 2 ms after the push line. */
            if (cat_now && want && have && have != want &&
                esp_timer_get_time() > s_dial_settle_us) {
                if (have != s_last_mismatch) {
                    s_last_mismatch = have;
                    ESP_LOGW(TAG, "dial MISMATCH: the radio is on %lu Hz but WSPR "
                                  "is set to %lu Hz - every spot this cycle will be "
                                  "filed against the WSPR setting, so its band is "
                                  "wrong. Re-pick the band on this page to agree.",
                             (unsigned long)have, (unsigned long)want);
                }
            } else {
                s_last_mismatch = 0;
            }
        }
    }

    /* Dial: select the standard entry matching the radio, so the picker shows
     * where we actually are rather than what was last tapped. A dial that is
     * not a standard WSPR frequency leaves the selection alone - the operator
     * has tuned off the sub-band and the picker should not pretend otherwise. */
    /* The radio may only have answered its band list AFTER the page was built,
     * so re-take it here - the picker is otherwise stuck with whatever was
     * known at construction (every band, if CAT was down). */
    {
        int n = wspr_bands_available(s_avail, (int)sizeof(s_avail));
        if (n != s_navail) s_navail = n;
    }
    /* And the button names whatever dial is in force, however it got there -
     * a hop, the web, or the radio's own knob. It is a label, so re-writing an
     * unchanged string costs nothing; lv_label_set_text early-outs on equal. */
    bp_button_refresh();

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

        /* ⚠ UNPROTECTED is said ON THIS PAGE, not only in the drawer.
         *
         * WSPR keys the PA for ~110 s out of every 120 and the finals overheat
         * at full power on that cycle. The guard defaults ON and switching it
         * off is deliberate and toasted - but someone who switched it off and
         * walked away had nothing in front of them saying so, and this is the
         * screen they are actually looking at. A protection whose absence is
         * invisible is a protection you cannot trust.
         *
         * Deliberately NOT a block. It is the operator's radio, and there are
         * legitimate reasons (a low supply, a dummy load, a QMX already turned
         * down). It just may not be silent. */
        bool unprotected = st.wspr_tx_en && !st.wspr_pa_reduce;

        if (tst == WSPR_TX_ACTIVE) {
            snprintf(txt, sizeof(txt), unprotected ? "TX  ON AIR  FULL PWR" : "TX  ON AIR");
            lv_obj_set_style_bg_color(s_btn_tx, lv_color_hex(UI_COLOR_TX_ACTIVE), 0);
        } else if (tst == WSPR_TX_ARMED) {
            snprintf(txt, sizeof(txt), "TX  in %d:%02d%s", secs / 60, secs % 60,
                     unprotected ? "  FULL PWR" : "");
            lv_obj_set_style_bg_color(s_btn_tx, lv_color_hex(UI_COLOR_PRIMARY), 0);
        } else if (st.wspr_tx_en) {
            /* ⭐ COUNT DOWN WHENEVER TRANSMIT IS ON, not only while ARMED
             * (operator, 2026-09-02: "TX ON button never count down any more?
             * This was an important info").
             *
             * ⚠ I removed this without meaning to. The countdown used to be
             * visible because of a BUG: an arm that missed its own even minute
             * was scheduled for the NEXT one, so ARMED lasted nearly two
             * minutes and the button counted through it. Fixing that (this
             * release) made ARMED last about a second, and the countdown
             * vanished with it.
             *
             * So it comes back from the honest source: the time to the next
             * even minute, which is when a burst may start. It says "next"
             * rather than promising one, because the duty cycle is a random
             * roll taken at the boundary - at 50 % roughly every other slot
             * transmits, and claiming a burst that then does not happen would
             * be worse than saying nothing. */
            /* ⭐ TIME TO THE NEXT REAL BURST, not to the next opportunity.
             *
             * This asked wspr_tx_seconds_until_next_slot() for one release, and
             * the operator caught it immediately (2026-09-02): "when it reached
             * 00:00 then it started counting down again 01:20(!) I need to see a
             * REAL count down to the next TX." Quite right - the duty-cycle roll
             * was taken AT the boundary, so at zero there was still only a
             * duty_pct chance of anything happening, and most of the time the
             * counter simply restarted. It was counting down to a coin toss.
             *
             * The roll is now taken in advance (roll_next_tx_cycle in
             * wspr_rx.c), so there is a real answer to give.
             *
             * -1 means nothing is scheduled - duty 0, or the schedule was just
             * overtaken and the RX loop has not re-rolled yet. Say nothing then
             * rather than print 0:00, which would be the same lie in a
             * different shape. */
            int nxt = wspr_rx_seconds_to_next_tx();
            if (nxt >= 0)
                snprintf(txt, sizeof(txt), "TX  ON  next %d:%02d%s",
                         nxt / 60, nxt % 60, unprotected ? "  FULL PWR" : "");
            else
                snprintf(txt, sizeof(txt), "TX  ON%s",
                         unprotected ? "  FULL PWR" : "");
        } else {
            snprintf(txt, sizeof(txt), "TX  OFF%s",
                     unprotected ? "  FULL PWR" : "");
            lv_obj_set_style_bg_color(s_btn_tx,
                lv_color_hex(st.wspr_tx_en ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_RAISED), 0);
        }
        /* Red text on the TX block whenever the finals are unprotected, in every
         * one of the three states above - the risk does not pause between
         * bursts, because the next one is coming in under two minutes. */
        lv_obj_set_style_text_color(s_lbl_tx,
            lv_color_hex(unprotected ? 0xFF4010 : 0xFFFFFF), 0);
        if (strcmp(lv_label_get_text(s_lbl_tx), txt) != 0)
            lv_label_set_text(s_lbl_tx, txt);

        /* The Duty readout that used to live here went to the drawer with its
         * button (2026-08-28). Nothing is left to update: TX above still shows
         * whether transmitting is armed at all, which is the part that changes
         * during a session. */
    }

    /* cycle position: the bar is the 120 s window, so it is a real clock
     * position rather than a progress guess. */
    time_t now = time(NULL);
    int into = (int)(now % 120);
    lv_bar_set_value(s_bar_cycle, into, LV_ANIM_OFF);

    char c[48];
    /* ⭐ THE BAND, beside the cycle clock. Roy KI0ER, 2026-08-31: "Band could be
     * indicated in the section banner along with UTC." It goes here rather than
     * in the MODE title because this line already refreshes every second, and
     * with band hopping on the answer CHANGES - a band printed once when the
     * page was built would be wrong for most of the session, which is worse
     * than absent. Blank if the dial matches no WSPR band. */
    qmx_settings_t bs; settings_load_all(&bs);
    const char *bn = wspr_band_name_for_dial(bs.wspr_dial_hz);
    if (bn) snprintf(c, sizeof(c), "%s m   cycle  %d:%02d / 2:00", bn, into / 60, into % 60);
    else    snprintf(c, sizeof(c), "cycle  %d:%02d / 2:00", into / 60, into % 60);
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
    /* ⭐ NO SEPARATE "the hold is ageing" REPAINT IS NEEDED, and adding one
     * would be dead code: wf_publish() bumps the row counter and the waterfall
     * seq together, so every row that shifts the hold offset also lands in the
     * branch above, which recomputes the offset before painting. The only
     * other way the view moves is the button, which repaints itself. */

    /* Repainted when a spot was ADDED - not when the COUNT changed. The count
     * saturates at the ring size and then never moves again, which froze this
     * list and the header below for five hours of the 2026-08-24 overnight run
     * while the receiver decoded normally throughout. */
    int n = (int)wspr_spots_seq();
    /* ⭐ A UNIT CHANGE IS A REASON TO REPAINT, and the sequence number is not
       the only thing that makes the rows wrong. This guard exists so a quiet
       band does not rebuild an unchanged list every second - but it also meant
       that switching to miles converted nothing already on screen until the
       next decode arrived, which on WSPR can be two minutes away or, on a dead
       band, never. Reported straight after the heading was fixed: "decoded data
       does not change either".

       Generalise it: a change-detected repaint must key on everything the
       render READS, not just on the data it lists. */
    const bool mi_now = wspr_dist_in_miles();
    /* ⛔ AND THE MARKS ARE A THIRD THING THE ROWS READ (#360), which the rule
     * stated immediately above would have caught if I had applied it to my own
     * new column. Spots are filed DURING the decode loop, so wspr_spots_seq()
     * moves and the list rebuilds - and marks_publish() only happens at the END
     * of that cycle's decode, a moment later. So the rows were built before any
     * letter existed and the S column came out blank on every row, while the
     * carpet above showed A and B perfectly. Caught on the bench 2026-09-08 in
     * the first cycle that decoded anything. */
    const uint32_t mk_now = wspr_rx_marks_seq();
    if (n == s_last_spot_count && mi_now == s_rows_miles && mk_now == s_rows_marks_seq)
        return;
    s_rows_miles = mi_now;
    s_rows_marks_seq = mk_now;
    s_last_spot_count = n;

    /* BOTH numbers, because one of them alone is misread. "Heard 12 stations"
     * over a list showing 18 rows reads as a bug - the operator asked whether
     * the header was wrong within minutes of the list first working. It was
     * not: the header counts DISTINCT CALLSIGNS and the list has one row per
     * DECODE, so a station heard in four cycles is four rows and one station.
     *
     * "spots" is the WSPR word for a decode, so this is also the vocabulary
     * every other WSPR tool and wsprnet itself uses - saying both makes the
     * relationship obvious instead of leaving it to be worked out.
     *
     * ⚠ Neither figure is a session total. The ring holds WSPR_SPOT_RING (256)
     * entries, roughly eight cycles of a busy band, and older spots fall off
     * the end - so this is a rolling window, hours on a quiet band and about a
     * quarter of an hour on a crowded one. */
    int uniq = wspr_spots_unique_calls();
    int held = wspr_spots_count();
    char h[64];
    if (uniq == 0) snprintf(h, sizeof(h), "Heard nothing yet");
    else snprintf(h, sizeof(h), "%d station%s / %d spot%s",
                  uniq, uniq == 1 ? "" : "s", held, held == 1 ? "" : "s");
    lv_label_set_text(s_lbl_heard, h);

    if (n == 0) {
        /* ⭐ NOT "Listening..." WHILE THE RADIO IS KEYED (Roy KI0ER, 2026-09-01:
         * "while TX ON AIR is showing, over in the empty decodes list, it still
         * says listening ...").
         *
         * He filed it as cosmetic. It is not quite: during a transmit cycle the
         * receiver really is stood down - wspr_rx.c skips the capture entirely
         * and the status line says "transmitting" - so "Listening" was the one
         * part of the screen making a false statement, and it was doing it next
         * to a button reading TX ON AIR. Say what the radio is actually doing. */
        char txt[64];
        wspr_tx_state_t tst = wspr_tx_get_status(txt, sizeof(txt), NULL);
        lv_label_set_text(s_lbl_rows,
            tst == WSPR_TX_ACTIVE ? "Transmitting - not receiving this cycle"
                                  : "Listening...");
        return;
    }

    /* Static, never the stack - and in PSRAM, because it is read once per
     * second by a list rebuild and internal RAM is what the OTA verify runs
     * out of. colmap/rowmap above stay internal deliberately: colmap is read
     * once per PIXEL of a repaint. */
    EXT_RAM_BSS_ATTR static wspr_spot_t snap[VIEW_ROWS];
    int got = wspr_spots_get(snap, VIEW_ROWS);

    /* Grouped under the cycle each burst was heard in - the whole reason this
     * is a log and not a live list. */
    /* ⛔ STATIC AND IN PSRAM, NOT A STACK LOCAL. At 12 rows this was 1.5 KB on
     * the stack and got away with it; at 64 it is ~7.3 KB on taskLVGL, whose
     * stack is about 8 KB - and CLAUDE.md carries a list of crashes from
     * exactly this (the v0.20.1 pounce crash was an 11 KB array on this very
     * task, and the compiler reserves the frame at the prologue whether the
     * code path is taken or not). Safe as a static because this runs only on
     * taskLVGL, the same reasoning snap[] above uses. */
    EXT_RAM_BSS_ATTR static char buf[VIEW_ROWS * 120 + 256];
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
        char row[224];   /* grew with the BND column - -Werror=format-truncation */
        fmt_row(row, sizeof(row), &snap[i], utc);
        off += snprintf(buf + off, sizeof(buf) - off, "%s\n", row);
    }
    lv_label_set_text(s_lbl_rows, buf);
}
