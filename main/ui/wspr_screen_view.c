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
static lv_obj_t *s_list;           /* right pane, one label per line */
static lv_obj_t *s_lbl_rows;
static lv_obj_t *s_wf_canvas;

static uint8_t  *s_wf_buf;      /* RGB565 canvas pixels */
static uint8_t  *s_wf_data;     /* WSPR_WF_ROWS x WSPR_WF_COLS intensities */
static uint32_t  s_wf_seen;

static int   s_last_spot_count = -1;
static char  s_last_status[48];

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
#define ROW_FMT "%-9s %-5s %-4s %5s %5s %7s %5s %7s %4s"

static void fmt_row(char *out, size_t n, const wspr_spot_t *sp)
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

    snprintf(out, n, ROW_FMT, sp->call, sp->grid,
             sp->cty[0] ? sp->cty : "--", snr, drift, hz, pwr, km, brg);
}

static void fmt_header(char *out, size_t n)
{
    snprintf(out, n, ROW_FMT, "CALL", "GRID", "CTY", "SNR", "DRIFT",
             "HZ", "PWR", "KM", "BRG");
}

static void cycle_label(char *out, size_t n, int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d:%02d UTC", tmv.tm_hour, tmv.tm_min);
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

    s_lbl_dial = lv_label_create(box);
    lv_label_set_text(s_lbl_dial, "Dial: --.------ MHz");
    lv_obj_set_style_text_font(s_lbl_dial, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_dial, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(s_lbl_dial);

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

    /* No TX control on this page yet, and saying so beats an inert button.
     * WSPR TX exists and has been on the air, but its duty-cycle scheduler is
     * not built - see docs/wspr-ui-design.md. */
    lv_obj_t *note = lv_label_create(s_container);
    lv_label_set_text(note, "Receive only.\nTX is not wired to\nthis page yet.");
    lv_obj_set_style_text_font(note, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(note, 16, 240);

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
 * Only called when the sequence number moves, i.e. once per cycle. */
static void repaint_waterfall(void)
{
    if (!s_wf_canvas || !s_wf_data || !s_wf_buf) return;
    uint16_t *px = (uint16_t *)s_wf_buf;

    /* Row and column maps precomputed once instead of a divide per pixel. */
    static uint16_t colmap[RIGHT_W];
    static uint16_t rowmap[WF_H];
    for (int x = 0; x < RIGHT_W; x++) colmap[x] = (uint16_t)(x * WSPR_WF_COLS / RIGHT_W);
    for (int y = 0; y < WF_H;    y++) rowmap[y] = (uint16_t)(y * WSPR_WF_ROWS / WF_H);

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

    /* dial */
    uint32_t f = cat_get_frequency();
    if (f) {
        char d[40];
        snprintf(d, sizeof(d), "Dial: %lu.%03lu.%03lu MHz",
                 (unsigned long)(f / 1000000),
                 (unsigned long)((f / 1000) % 1000),
                 (unsigned long)(f % 1000));
        lv_label_set_text(s_lbl_dial, d);
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
            s_wf_data = heap_caps_malloc(WSPR_WF_ROWS * WSPR_WF_COLS,
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
        if (snap[i].cycle_utc != last_cycle) {
            last_cycle = snap[i].cycle_utc;
            char cl[24];
            cycle_label(cl, sizeof(cl), last_cycle);
            off += snprintf(buf + off, sizeof(buf) - off, "%s%s\n",
                            i ? "\n" : "", cl);
        }
        char row[160];
        fmt_row(row, sizeof(row), &snap[i]);
        off += snprintf(buf + off, sizeof(buf) - off, "%s\n", row);
    }
    lv_label_set_text(s_lbl_rows, buf);
}
