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
#include "cat.h"

/* Duplicated from ui.c / ft8_screen_view.c, which already each carry their own
 * copy. Following the existing pattern rather than introducing a shared header
 * as a side effect of adding a page - but all three must move together. */
#define TOP_BAR_H     60
#define BOTTOM_BAR_H  36

#define MID_Y   TOP_BAR_H
#define MID_H   (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W   1280
#define LEFT_W  320

/* Rows the list can show at once. The pane is MID_H tall and a row is 26 px, so
 * this is a screenful with the header - NOT the ring's capacity. Deliberately
 * bounded: the snapshot is copied onto the caller's buffer and this runs on
 * taskLVGL, where CLAUDE.md keeps a list of crashes caused by kB-scale locals
 * (the v0.20.1 pounce crash was an 11 KB array on exactly this task). */
#define VIEW_ROWS 20

static lv_obj_t *s_container;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_dial;
static lv_obj_t *s_lbl_cycle;
static lv_obj_t *s_bar_cycle;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_heard;
static lv_obj_t *s_list;           /* right pane, one label per line */
static lv_obj_t *s_lbl_rows;

static int   s_last_spot_count = -1;
static char  s_last_status[48];

/* Column layout for the spot list. Monospaced by construction - the values are
 * short tokens and a proportional font makes them ragged where columns should
 * line up. Same reasoning as the browser table and the FT8 list. */
static const char *HEADER =
    "CALL      GRID  CTY   SNR  DRIFT     HZ    PWR      KM  BRG";

static void fmt_row(char *out, size_t n, const wspr_spot_t *s)
{
    /* Generous, because -Werror=format-truncation counts the worst case an int
     * can print, not the values WSPR can actually carry. */
    char snr[16], drift[16], km[20], brg[16];

    /* An unmeasured value prints as a dash, never as a number. WSPR_SNR_UNKNOWN
     * and WSPR_DRIFT_UNKNOWN exist precisely so this cannot quietly become a
     * fabricated measurement - the same rule that deleted the ADIF "599". */
    if (s->snr_db == WSPR_SNR_UNKNOWN) snprintf(snr, sizeof(snr), "%s", "--");
    else snprintf(snr, sizeof(snr), "%+d", s->snr_db);

    if (s->drift_hz == WSPR_DRIFT_UNKNOWN) snprintf(drift, sizeof(drift), "%s", "--");
    else snprintf(drift, sizeof(drift), "%+d", s->drift_hz);

    if (s->km < 0) snprintf(km, sizeof(km), "%s", "--");
    else snprintf(km, sizeof(km), "%d", (int)s->km);

    if (s->bearing_deg < 0) snprintf(brg, sizeof(brg), "%s", "--");
    else snprintf(brg, sizeof(brg), "%d", (int)s->bearing_deg);

    snprintf(out, n, "%-9s %-5s %-4s %5s %5s %6.1f %4d %7s %4s",
             s->call, s->grid, s->cty[0] ? s->cty : "--",
             snr, drift, (double)s->freq_hz, (int)s->power_dbm, km, brg);
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
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_pos(s_lbl_title, 16, 12);

    /* The dial, boxed like the FT8 page's preset. Read-only for now: the
     * standard-dial picker is the next piece (see docs/wspr-ui-design.md - a
     * free-entry keypad is deliberately NOT wanted, because every band has one
     * canonical WSPR frequency and anything else is simply not in the
     * sub-band). */
    lv_obj_t *box = lv_obj_create(s_container);
    lv_obj_set_size(box, LEFT_W - 32, 56);
    lv_obj_set_pos(box, 16, 76);
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
    lv_obj_set_pos(s_lbl_cycle, 16, 148);

    s_bar_cycle = lv_bar_create(s_container);
    lv_obj_set_size(s_bar_cycle, LEFT_W - 32, 10);
    lv_obj_set_pos(s_bar_cycle, 16, 184);
    lv_bar_set_range(s_bar_cycle, 0, 120);
    lv_bar_set_value(s_bar_cycle, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_PRIMARY), LV_PART_INDICATOR);

    s_lbl_status = lv_label_create(s_container);
    lv_label_set_text(s_lbl_status, "");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_pos(s_lbl_status, 16, 206);

    s_lbl_heard = lv_label_create(s_container);
    lv_label_set_text(s_lbl_heard, "Heard nothing yet");
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_lbl_heard, 16, 240);

    /* No TX control on this page yet, and saying so beats an inert button.
     * WSPR TX exists and has been on the air, but its duty-cycle scheduler is
     * not built - see docs/wspr-ui-design.md. */
    lv_obj_t *note = lv_label_create(s_container);
    lv_label_set_text(note, "Receive only.\nTX is not wired to\nthis page yet.");
    lv_obj_set_style_text_font(note, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(note, 16, 280);

    /* ---------------- right pane: the log ---------------- */
    lv_obj_t *hdr = lv_label_create(s_container);
    lv_label_set_text(hdr, HEADER);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(hdr, LEFT_W + 8, 10);

    s_list = lv_obj_create(s_container);
    lv_obj_set_size(s_list, MID_W - LEFT_W - 16, MID_H - 44);
    lv_obj_set_pos(s_list, LEFT_W + 8, 36);
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
    lv_obj_set_style_text_font(s_lbl_rows, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_rows, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_rows, 6, 0);
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
