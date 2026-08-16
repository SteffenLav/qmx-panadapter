#include "qmx_term_view.h"
#include "qmx_term.h"
#include "ui.h"                 // ui_help_overlay_changed(), ui_toast()
#include "ui_theme.h"

#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "term_view";

LV_FONT_DECLARE(qmx_mono_25);

/* The grid. CELL_W must match the font's advance width EXACTLY or every
 * reverse-video highlight drifts along its row - see the note at the top of
 * font_qmx_mono_25.c. adv_w there is 240 sixteenths = 15 px. */
#define CELL_W      15
#define ROW_H       27
#define GRID_W      (ANSI_COLS * CELL_W)   /* 1200 */
#define GRID_H      (ANSI_ROWS * ROW_H)    /* 648  */
#define HEADER_H    62

#define POLL_MS     350

static lv_obj_t  *s_overlay;
static lv_obj_t  *s_grid;
static lv_obj_t  *s_rows[ANSI_ROWS];
static lv_obj_t  *s_state;
static lv_timer_t *s_timer;
static bool       s_open;
static uint32_t   s_seen_seq;

/* Reverse-video blocks, rebuilt on every change. There are one or two on a
 * normal menu, so a small fixed pool is plenty and avoids churning LVGL's
 * allocator at 3 Hz. Anything beyond the pool is simply not highlighted, which
 * degrades to "no highlight" rather than to a wrong one. */
#define MAX_REV 24
static lv_obj_t *s_rev[MAX_REV];
static int       s_rev_used;

bool qmx_term_view_is_open(void) { return s_open; }

static void set_state(const char *txt, uint32_t colour)
{
    if (!s_state) return;
    lv_label_set_text(s_state, txt);
    lv_obj_set_style_text_color(s_state, lv_color_hex(colour), 0);
}

/* Paint the whole grid from the model. Caller must NOT hold the screen lock -
 * this takes it itself and gives it straight back. */
static void repaint(void)
{
    const ansi_term_t *t = qmx_term_lock_screen();
    if (!t) return;

    if (t->dirty_seq == s_seen_seq) { qmx_term_unlock_screen(); return; }
    s_seen_seq = t->dirty_seq;

    char line[ANSI_COLS + 1];
    int rev_n = 0;
    struct { int r, c, len; } runs[MAX_REV];

    for (int r = 0; r < ANSI_ROWS; r++) {
        ansi_term_row_text(t, r, line);
        int e = ANSI_COLS;
        while (e > 0 && line[e - 1] == ' ') e--;
        line[e] = '\0';
        if (s_rows[r]) lv_label_set_text(s_rows[r], line);

        for (int c = 0; c < ANSI_COLS; ) {
            if (t->cell[r][c].reverse) {
                int s = c;
                while (c < ANSI_COLS && t->cell[r][c].reverse) c++;
                if (rev_n < MAX_REV) { runs[rev_n].r = r; runs[rev_n].c = s;
                                       runs[rev_n].len = c - s; rev_n++; }
            } else c++;
        }
    }
    qmx_term_unlock_screen();

    /* Highlights are drawn as filled blocks BEHIND the text, with the text
     * recoloured to dark for those rows... which cannot be done per-run on a
     * plain label. So instead each run gets its own small label on TOP of its
     * block, carrying just that run's characters. Exact because the font is
     * fixed-pitch and CELL_W is integral. */
    for (int i = 0; i < rev_n; i++) {
        if (!s_rev[i]) {
            s_rev[i] = lv_obj_create(s_grid);
            lv_obj_remove_style_all(s_rev[i]);
            lv_obj_set_style_bg_opa(s_rev[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_rev[i], lv_color_hex(0xD8D8D8), 0);
            lv_obj_clear_flag(s_rev[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(s_rev[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *lbl = lv_label_create(s_rev[i]);
            lv_obj_set_style_text_font(lbl, &qmx_mono_25, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        lv_obj_set_pos(s_rev[i], runs[i].c * CELL_W, runs[i].r * ROW_H);
        lv_obj_set_size(s_rev[i], runs[i].len * CELL_W, ROW_H);
        lv_obj_clear_flag(s_rev[i], LV_OBJ_FLAG_HIDDEN);

        /* Re-read the run's text from the row label we just set. */
        const char *row_txt = s_rows[runs[i].r] ? lv_label_get_text(s_rows[runs[i].r]) : "";
        char frag[ANSI_COLS + 1];
        int rl = (int)strlen(row_txt);
        int n = 0;
        for (int k = runs[i].c; k < runs[i].c + runs[i].len && k < ANSI_COLS; k++)
            frag[n++] = (k < rl) ? row_txt[k] : ' ';
        frag[n] = '\0';
        lv_label_set_text(lv_obj_get_child(s_rev[i], 0), frag);
    }
    for (int i = rev_n; i < s_rev_used; i++)
        if (s_rev[i]) lv_obj_add_flag(s_rev[i], LV_OBJ_FLAG_HIDDEN);
    s_rev_used = rev_n;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_open) return;
    if (!qmx_term_is_open()) {
        /* The device's own idle watchdog closed it. Say which, or the screen
         * simply freezing looks like a fault. */
        set_state("session timed out - closed", 0xFFA040);
        return;
    }
    repaint();
}

static void key_cb(lv_event_t *e)
{
    const char *k = (const char *)lv_event_get_user_data(e);
    if (!k) return;
    qmx_term_key(k);
    s_seen_seq = 0;                 /* force a repaint on the next tick */
}

static void close_cb(lv_event_t *e) { (void)e; qmx_term_view_close(); }

static lv_obj_t *make_key(lv_obj_t *parent, const char *label, const char *key,
                          int x, int w)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 48);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2a3138), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, (void *)key);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return b;
}

static void build(void)
{
    if (s_overlay) return;

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   // swallow taps to the panadapter

    lv_obj_t *hdr = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_HOR_RES, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Radio menus");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    s_state = lv_label_create(hdr);
    lv_label_set_text(s_state, "");
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_20, 0);
    lv_obj_align(s_state, LV_ALIGN_LEFT_MID, 200, 0);

    /* Keys along the header, so the 1200 px grid keeps the whole width below. */
    make_key(hdr, LV_SYMBOL_UP,    "up",     440,  70);
    make_key(hdr, LV_SYMBOL_DOWN,  "down",   516,  70);
    make_key(hdr, LV_SYMBOL_LEFT,  "left",   592,  70);
    make_key(hdr, LV_SYMBOL_RIGHT, "right",  668,  70);
    make_key(hdr, "Enter",         "enter",  750, 120);
    make_key(hdr, "Back",          "ctrl-q", 876, 110);

    lv_obj_t *cb = lv_btn_create(hdr);
    lv_obj_set_size(cb, 130, 48);
    lv_obj_align(cb, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x3a2222), 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0xB05050), 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_add_event_cb(cb, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);

    s_grid = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, GRID_W, GRID_H);
    lv_obj_set_pos(s_grid, (LV_HOR_RES - GRID_W) / 2, HEADER_H + 4);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < ANSI_ROWS; r++) {
        s_rows[r] = lv_label_create(s_grid);
        lv_obj_set_style_text_font(s_rows[r], &qmx_mono_25, 0);
        lv_obj_set_style_text_color(s_rows[r], lv_color_hex(0xD8D8D8), 0);
        lv_label_set_long_mode(s_rows[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_rows[r], GRID_W);
        lv_obj_set_pos(s_rows[r], 0, r * ROW_H);
        lv_label_set_text(s_rows[r], "");
    }
}

void qmx_term_view_open(void)
{
    build();
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    s_open = true;
    s_seen_seq = 0;
    for (int r = 0; r < ANSI_ROWS; r++) if (s_rows[r]) lv_label_set_text(s_rows[r], "");
    for (int i = 0; i < s_rev_used; i++) if (s_rev[i]) lv_obj_add_flag(s_rev[i], LV_OBJ_FLAG_HIDDEN);
    s_rev_used = 0;

    /* Stand the panadapter's touch navigation down. Without this the top-bar
     * Band/Mode/BW hit zones - direct children of the screen, foregrounded above
     * this overlay - swallow taps aimed at our own keys, exactly as they did to
     * the Reader's Back/Exit buttons in v1.5.0. */
    ui_help_overlay_changed();

    set_state("connecting...", 0x888888);
    if (!qmx_term_open()) {
        set_state("no second serial port on the radio", 0xFF6050);
        ui_toast("Set the QMX's System config > GPS & Ser. ports > USB serial ports "
                 "to 2, then power-cycle it.");
    } else {
        set_state("connected", 0x60C060);
        repaint();
    }
    if (!s_timer) s_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
    lv_timer_resume(s_timer);
    ESP_LOGI(TAG, "open");
}

void qmx_term_view_close(void)
{
    if (!s_open) return;
    s_open = false;
    if (s_timer) lv_timer_pause(s_timer);
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    ui_help_overlay_changed();     // hand the top bar and edge swipes back
    qmx_term_close();              // walks the radio's own "Exit terminal"
    ESP_LOGI(TAG, "close");
}
