/* RTTY page - see rtty_screen_view.h. R006.
 *
 * Layout agreed with the operator 2026-11-12 from a 1280x720 mock-up, with
 * 8 text rows ("8 rows") over a 190 px waterfall: RTTY prints continuously,
 * ~6 characters/s, and the CW page's 4-row ticker would scroll text away
 * before it can be read. */
#include "rtty_screen_view.h"
#include "cw_screen_view.h"   /* CW_PANE_W - the shared left-pane width */
#include "ui_theme.h"
#include "ui.h"
#include "rtty_rx.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "rtty_screen";

/* Same middle-band framing as cw_screen_view.c - keep in step. */
#define TOP_BAR_H     60
#define BOTTOM_BAR_H  36
#define MID_Y         TOP_BAR_H
#define MID_H         (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W         1280
#define PANE_W        CW_PANE_W
#define BANDPLAN_H    22

#define TXT_ROWS      8
/* (MID_W - PANE_W) = 960 px at qmx_mono_25's 15 px advance, minus one - the
 * same arithmetic as the CW ticker's CW_TICKER_COLS. */
#define TXT_COLS      63
#define TXT_PAD       8

#define COL_NEW       0x30E0E0   /* current line, cyan - as the CW ticker */
#define COL_OLD       0x30E060   /* earlier lines, green */
#define COL_MARK      0xFF9F40   /* tone markers + level bars */
#define BTN_BG        0x2a2f37
#define BTN_ON        0x2d5a3f

LV_FONT_DECLARE(qmx_mono_25);

static lv_obj_t *s_container, *s_pane, *s_text_box;
static lv_obj_t *s_row[TXT_ROWS];
static lv_obj_t *s_bar_m, *s_bar_s, *s_bar_sig, *s_sig_tick;
static lv_obj_t *s_lbl_afc, *s_lbl_speed, *s_lbl_status;
static lv_obj_t *s_btn_rev, *s_btn_afc, *s_btn_sq;
static lv_obj_t *s_lbl_rev, *s_lbl_afc_btn, *s_lbl_sq;
static lv_obj_t *s_marker[2];
static lv_obj_t *s_speed_modal;
static lv_timer_t *s_timer;
static bool s_shown;
static int  s_txt_top;      /* container y of the text box */

/* Text grid: TXT_ROWS lines, the last is the one being written. */
static char     s_lines[TXT_ROWS][TXT_COLS + 1];
static int      s_col;
static uint32_t s_cursor;
static bool     s_text_dirty;
static uint32_t s_clear_seen;   /* rtty_rx_text_clear_gen() last acted on */

#define BAR_W 230

/* ---- text ---------------------------------------------------------------- */

static void text_newline(void)
{
    memmove(s_lines[0], s_lines[1], sizeof(s_lines[0]) * (TXT_ROWS - 1));
    s_lines[TXT_ROWS - 1][0] = 0;
    s_col = 0;
}

static void text_put(char c)
{
    if (c == '\r') return;
    if (c == '\n') { text_newline(); s_text_dirty = true; return; }
    if (s_col >= TXT_COLS) text_newline();
    s_lines[TXT_ROWS - 1][s_col++] = c;
    s_lines[TXT_ROWS - 1][s_col] = 0;
    s_text_dirty = true;
}

static void text_clear(void)
{
    memset(s_lines, 0, sizeof(s_lines));
    s_col = 0;
    s_text_dirty = true;
}

static void text_render(void)
{
    if (!s_text_dirty) return;
    s_text_dirty = false;
    for (int r = 0; r < TXT_ROWS; r++) {
        lv_label_set_text(s_row[r], s_lines[r]);
        lv_obj_set_style_text_color(s_row[r], lv_color_hex(r == TXT_ROWS - 1 ? COL_NEW : COL_OLD), 0);
    }
}

/* ---- controls ------------------------------------------------------------ */

static void style_toggle(lv_obj_t *b, lv_obj_t *l, const char *txt, bool on)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? BTN_ON : BTN_BG), 0);
    lv_label_set_text(l, txt);
}

static void refresh_buttons(void)
{
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    /* Words only, green = on. At montserrat_26 "Squelch: off" is wider than
     * the 145 px button, and the pane cannot widen (CW_PANE_W is shared with
     * the spectrum mapping). Operator 2026-11-12: fonts "far too small". */
    style_toggle(s_btn_rev, s_lbl_rev, "Reverse", c.reverse);
    style_toggle(s_btn_afc, s_lbl_afc_btn, "AFC", c.afc);
    style_toggle(s_btn_sq, s_lbl_sq, "Squelch", c.squelch);
    char b[24];
    if (c.baud == 45.45f) snprintf(b, sizeof b, "45.45/%d", (int)c.shift_hz);
    else snprintf(b, sizeof b, "%d/%d", (int)(c.baud + 0.5f), (int)c.shift_hz);
    lv_label_set_text(s_lbl_speed, b);
}

static void toggle_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    if (which == 0) c.reverse = !c.reverse;
    if (which == 1) c.afc = !c.afc;
    if (which == 2) c.squelch = !c.squelch;
    rtty_rx_apply(&c);
    refresh_buttons();
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    rtty_rx_text_clear();   /* the timer sees the new generation and clears the grid */
}

static lv_obj_t *mk_btn_f(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *f,
                          const char *txt, lv_event_cb_t cb, void *ud, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(BTN_BG), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8E8E8), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lbl_out) *lbl_out = l;
    return b;
}

#define BTN_FONT (&lv_font_montserrat_26)
#define mk_btn(p, x, y, w, h, t, cb, ud, lo) mk_btn_f(p, x, y, w, h, BTN_FONT, t, cb, ud, lo)

/* ---- speed / shift picker ------------------------------------------------ */

static const float SPEEDS[] = { 45.45f, 50.0f, 75.0f };
static const int   SHIFTS[] = { 170, 200, 425, 450, 850 };

static void speed_close(void)
{
    if (s_speed_modal) { lv_obj_del(s_speed_modal); s_speed_modal = NULL; }
}

static void speed_pick_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    if (v < 100) c.baud = SPEEDS[v];          /* 0..2: speed index */
    else c.shift_hz = (float)SHIFTS[v - 100]; /* 100..: shift index */
    rtty_rx_apply(&c);
    refresh_buttons();
    speed_close();
}

static void speed_close_cb(lv_event_t *e) { (void)e; speed_close(); }

static void speed_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_speed_modal) return;
    s_speed_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_speed_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_speed_modal, 0, 0);
    lv_obj_set_style_bg_color(s_speed_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_speed_modal, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_speed_modal, 0, 0);
    lv_obj_clear_flag(s_speed_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_speed_modal, speed_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *p = lv_obj_create(s_speed_modal);
    lv_obj_set_size(p, 860, 300);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);   /* taps on the panel do not close */

    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    lv_obj_t *t1 = lv_label_create(p);
    lv_label_set_text(t1, "Speed (baud)");
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(0xC8CCD2), 0);
    lv_obj_set_pos(t1, 0, 0);
    for (int i = 0; i < 3; i++) {
        char b[12];
        if (i == 0) snprintf(b, sizeof b, "45.45"); else snprintf(b, sizeof b, "%d", (int)SPEEDS[i]);
        lv_obj_t *bt = mk_btn(p, i * 160, 34, 150, 56, b, speed_pick_cb, (void *)(intptr_t)i, NULL);
        if (SPEEDS[i] == c.baud) lv_obj_set_style_bg_color(bt, lv_color_hex(BTN_ON), 0);
    }
    lv_obj_t *t2 = lv_label_create(p);
    lv_label_set_text(t2, "Shift (Hz)");
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t2, lv_color_hex(0xC8CCD2), 0);
    lv_obj_set_pos(t2, 0, 108);
    for (int i = 0; i < 5; i++) {
        char b[12];
        snprintf(b, sizeof b, "%d", SHIFTS[i]);
        lv_obj_t *bt = mk_btn(p, i * 160, 144, 150, 56, b, speed_pick_cb, (void *)(intptr_t)(100 + i), NULL);
        if ((float)SHIFTS[i] == c.shift_hz) lv_obj_set_style_bg_color(bt, lv_color_hex(BTN_ON), 0);
    }
    mk_btn(p, 650, 216, 150, 56, "Close", speed_close_cb, NULL, NULL);
}

/* ---- live state ---------------------------------------------------------- */

static int hz_to_x(int64_t hz)
{
    int64_t lo; int32_t span;
    ui_screen_view_hz(&lo, &span);
    if (span <= 0) return -1;
    int x0 = ui_spec_x0();
    return x0 + (int)((hz - lo) * (int64_t)(MID_W - x0) / span);
}

static void place_markers(const rtty_rx_status_t *st, uint32_t dial)
{
    float half = st->cfg.shift_hz * 0.5f;
    float centre = (float)st->cfg.offset_hz + st->afc_hz;
    for (int i = 0; i < 2; i++) {
        float off = centre + (i ? half : -half);
        int x = dial ? hz_to_x((int64_t)dial + (int64_t)off) : -1;
        if (x < ui_spec_x0() || x >= MID_W - 1) { lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_marker[i], x - 1);
    }
}

static void set_bar(lv_obj_t *bar, float frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    lv_obj_set_width(bar, (int)(frac * BAR_W));
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_shown) return;

    /* A Clear from either surface (this pane or the web page). */
    uint32_t g = rtty_rx_text_clear_gen();
    if (g != s_clear_seen) {
        s_clear_seen = g;
        s_cursor = rtty_rx_text_count();
        text_clear();
    }
    char buf[128];
    size_t n;
    while ((n = rtty_rx_text_read(&s_cursor, buf, sizeof buf)) > 0)
        for (size_t i = 0; i < n; i++) text_put(buf[i]);
    text_render();

    rtty_rx_status_t st;
    rtty_rx_status(&st);
    float big = st.mark_lvl > st.space_lvl ? st.mark_lvl : st.space_lvl;
    set_bar(s_bar_m, big > 0 ? st.mark_lvl / big : 0);
    set_bar(s_bar_s, big > 0 ? st.space_lvl / big : 0);
    set_bar(s_bar_sig, st.sig_lvl);
    lv_obj_set_style_bg_color(s_bar_sig, lv_color_hex(st.present ? 0x30E060 : 0x5F6670), 0);

    char b[48];
    snprintf(b, sizeof b, "AFC %+d Hz", (int)(st.afc_hz + (st.afc_hz >= 0 ? 0.5f : -0.5f)));
    lv_label_set_text(s_lbl_afc, st.cfg.afc ? b : "AFC off");

    uint32_t dial = ui_get_dial_freq_hz();
    char s[96];
    if (st.fs_hz <= 0)
        snprintf(s, sizeof s, "Waiting for audio\nfrom the radio");
    else
        snprintf(s, sizeof s, "%s at %d Hz\n%lu chars", st.present ? "Decoding" : "Listening",
                 st.cfg.offset_hz + (int)st.afc_hz, (unsigned long)st.chars);
    lv_label_set_text(s_lbl_status, s);
    place_markers(&st, dial);
}

void rtty_screen_view_tap_hz(uint32_t tapped_hz, uint32_t dial_hz)
{
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.offset_hz = (int)((int64_t)tapped_hz - (int64_t)dial_hz);
    c.on = true;
    rtty_rx_apply(&c);
    ESP_LOGI(TAG, "tap: decoder to %d Hz from the dial (dial not moved)", c.offset_hz);
}

/* ---- build --------------------------------------------------------------- */

static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const lv_font_t *f, uint32_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

void rtty_screen_view_init(lv_obj_t *parent)
{
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    /* Click-through, as the CW page: taps on the spectrum and waterfall must
     * reach ui.c, which sends them to rtty_screen_view_tap_hz(). */
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);

    /* Tone markers first, so the pane and the text box draw over them. */
    s_txt_top = MID_H - BANDPLAN_H - (TXT_ROWS * ((int)lv_font_get_line_height(&qmx_mono_25) + 2) + 2 * TXT_PAD);
    for (int i = 0; i < 2; i++) {
        s_marker[i] = mk_rect(s_container, 0, 0, 2, s_txt_top, COL_MARK);
        lv_obj_set_style_bg_opa(s_marker[i], LV_OPA_70, 0);
        lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_pane = mk_rect(s_container, 0, 0, PANE_W, MID_H, 0x14181e);
    lv_obj_add_flag(s_pane, LV_OBJ_FLAG_CLICKABLE);   /* the pane eats its own taps */

    /* Title, as the CW page's, in the colour of the drawer button. */
    mk_label(s_pane, 14, 6, &lv_font_montserrat_48, UI_COLOR_MODE_RTTY_TEXT, "MODE: RTTY");

    /* Tuning aid: mark, space, and signal level with the squelch tick. */
    lv_obj_t *box = mk_rect(s_pane, 10, 70, 300, 104, 0x0d1116);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2a313b), 0);
    mk_label(box, 8, 4, &lv_font_montserrat_24, 0xC8CCD2, "M");
    mk_rect(box, 50, 10, BAR_W, 18, 0x252b34);
    s_bar_m = mk_rect(box, 50, 10, 0, 18, COL_MARK);
    mk_label(box, 8, 36, &lv_font_montserrat_24, 0xC8CCD2, "S");
    mk_rect(box, 50, 42, BAR_W, 18, 0x252b34);
    s_bar_s = mk_rect(box, 50, 42, 0, 18, COL_MARK);
    mk_label(box, 4, 68, &lv_font_montserrat_24, 0xC8CCD2, "Sig");
    mk_rect(box, 50, 74, BAR_W, 18, 0x252b34);
    s_bar_sig = mk_rect(box, 50, 74, 0, 18, 0x5F6670);
    s_sig_tick = mk_rect(box, 50 + (int)(0.48f * BAR_W) - 1, 70, 2, 26, 0xFFFFFF);

    s_lbl_afc = mk_label(s_pane, 12, 190, &lv_font_montserrat_26, 0xE8E8E8, "AFC");
    /* 24, not 26: "45.45/170" has to fit 130 px beside an "AFC +150 Hz"
     * readout that ends near x = 171. */
    lv_obj_t *sp = mk_btn_f(s_pane, 180, 182, 130, 44, &lv_font_montserrat_24, "45.45/170",
                            speed_open_cb, NULL, &s_lbl_speed);
    lv_obj_set_style_text_color(s_lbl_speed, lv_color_hex(0x5FD4FF), 0);
    (void)sp;

    s_btn_rev = mk_btn(s_pane, 10, 236, 145, 56, "Reverse", toggle_cb, (void *)0, &s_lbl_rev);
    s_btn_afc = mk_btn(s_pane, 165, 236, 145, 56, "AFC", toggle_cb, (void *)1, &s_lbl_afc_btn);
    s_btn_sq  = mk_btn(s_pane, 10, 302, 145, 56, "Squelch", toggle_cb, (void *)2, &s_lbl_sq);
    mk_btn(s_pane, 165, 302, 145, 56, "Clear text", clear_cb, NULL, NULL);

    /* Reserved for TX (R010) - left empty on purpose so the pane does not
     * reflow when TX lands. */
    lv_obj_t *tx = mk_rect(s_pane, 10, 372, 300, 168, 0x14181e);
    lv_obj_set_style_border_width(tx, 1, 0);
    lv_obj_set_style_border_color(tx, lv_color_hex(0x3a414c), 0);
    lv_obj_t *txl = mk_label(tx, 0, 0, &lv_font_montserrat_24, 0x6b727c, "TX not built yet");
    lv_obj_center(txl);

    s_lbl_status = mk_label(s_pane, 12, MID_H - 76, &lv_font_montserrat_26, 0xC8CCD2, "");
    lv_obj_set_style_text_line_space(s_lbl_status, 2, 0);

    /* Decoded text, 8 rows, above the band-plan strip. */
    s_text_box = mk_rect(s_container, PANE_W, s_txt_top, MID_W - PANE_W,
                         MID_H - BANDPLAN_H - s_txt_top, 0x0d1116);
    lv_obj_set_style_border_width(s_text_box, 1, 0);
    lv_obj_set_style_border_color(s_text_box, lv_color_hex(0x2a313b), 0);
    /* Eats its own taps: a tap on the text must not move the decoder to the
     * waterfall row hidden under it. */
    lv_obj_add_flag(s_text_box, LV_OBJ_FLAG_CLICKABLE);
    const int pitch = (int)lv_font_get_line_height(&qmx_mono_25) + 2;
    for (int r = 0; r < TXT_ROWS; r++) {
        s_row[r] = mk_label(s_text_box, TXT_PAD, TXT_PAD + r * pitch, &qmx_mono_25, COL_OLD, "");
        lv_label_set_long_mode(s_row[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_row[r], MID_W - PANE_W - 2 * TXT_PAD);
    }
    text_clear();
    text_render();
    refresh_buttons();

    s_timer = lv_timer_create(timer_cb, 200, NULL);
    ESP_LOGI(TAG, "RTTY page built: text %d rows x %d cols at y %d", TXT_ROWS, TXT_COLS, s_txt_top);
}

void rtty_screen_view_show(void)
{
    if (!s_container) return;
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.on = true;
    rtty_rx_apply(&c);
    s_cursor = rtty_rx_text_count();   /* only what arrives from now on */
    refresh_buttons();
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_container);
    s_shown = true;
}

void rtty_screen_view_hide(void)
{
    if (!s_container) return;
    speed_close();
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_shown = false;
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.on = false;
    rtty_rx_apply(&c);
}

lv_obj_t *rtty_screen_view_get_container(void) { return s_container; }
