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
#include "rtty_tx.h"
#include "cw_callsign.h"
#include "storage/settings.h"
#include <ctype.h>
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
#define TXT_PAD       8
/* The text box ENDS LEFT OF THE OPERATOR WATERMARK, box and text both
 * (operator, 2026-11-12, for the CW page and then this one: "the greyed box
 * is covering it - i want it clear of that box even if the decoded text is not
 * covering it"). Same rule and constants as the CW ticker (CW_TICKER_X2 in
 * cw_screen_view.c): x 1243, so 923 px wide and (923 - 16) / 15 = 60 columns
 * at qmx_mono_25's 15 px advance. Was 960 px and 63 columns. */
#define TXT_BOX_X2    (UI_WATERMARK_X - UI_WATERMARK_GAP)
#define TXT_BOX_W     (TXT_BOX_X2 - PANE_W)
#define TXT_COLS      ((TXT_BOX_W - 2 * TXT_PAD) / 15)
_Static_assert(PANE_W + TXT_PAD + TXT_COLS * 15 <= TXT_BOX_X2 - TXT_PAD,
               "RTTY text runs into the watermark gap");

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
static lv_obj_t *s_kb_wrap, *s_kb, *s_kb_ta;   /* Type... : keyboard + what was typed */
static int       s_typed_len;                  /* chars of s_kb_ta already sent */
static bool      s_ta_resetting;               /* our own lv_textarea_set_text() */
static char      s_call[16];                   /* {CALL}: the callsign tapped in the text */
static char      s_err_shown[96];              /* rtty_tx's last refusal, as last shown */
static uint32_t  s_err_until;                  /* lv_tick until which it stays up */
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
#define RTTY_BAR_DB 30.0f   /* M/S bars: full scale, dB over the noise floor */
/* ...and their zero. The peak of 5 bins over their median reads ~4 dB on pure
 * noise (harness, YU65AEC rxcap 780 Hz off the signal: 4.2 / 3.8 dB; on the
 * signal 23.1 / 12.6 dB), so 0 dB would leave the bars a sixth full on noise. */
#define RTTY_BAR_DB0 3.0f

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

/* ---- TX (R010) ------------------------------------------------------------
 * Layout approved by the operator 2026-11-14 (docs/rtty-tx-mock.png). Every
 * press here is a human decision that can key the radio: the rows are in
 * docs/unattended-tx-inventory.md. The refusals (DiGi, callsign, sim, WSPR,
 * band guard, ...) are rtty_tx_send()'s, not this file's. */

/* Show a refusal for 8 s in the status lines. */
static void tx_show_err(const char *e)
{
    snprintf(s_err_shown, sizeof s_err_shown, "%s", e);
    s_err_until = lv_tick_get() + 8000;
}

static void tx_send(const char *text, bool end)
{
    char err[96] = "";
    if (rtty_tx_send(text, end, err, sizeof err) != ESP_OK) tx_show_err(err);
}

/* The four fixed messages. CALL is the station tapped in the text box, MY
 * the callsign from Identity, GRID the locator (left out when unset). The
 * leading '\n' starts the over on a fresh line at the receiver. */
enum { MSG_CQ, MSG_ANSWER, MSG_EXCH, MSG_73 };

static void macro_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    char my[16], grid[12], t[200];
    settings_get_my_callsign(my, sizeof my);
    settings_get_my_grid(grid, sizeof grid);
    if (!my[0]) { tx_show_err("no callsign set (Identity)"); return; }
    if (which != MSG_CQ && !s_call[0]) { tx_show_err("tap their callsign in the text first"); return; }
    const char *c = s_call;
    switch (which) {
    case MSG_CQ:
        snprintf(t, sizeof t, "\nCQ CQ CQ DE %s %s %s\nCQ CQ CQ DE %s %s %s PSE K\n",
                 my, my, my, my, my, my);
        break;
    case MSG_ANSWER:
        snprintf(t, sizeof t, "\n%s %s DE %s %s %s K\n", c, c, my, my, my);
        break;
    case MSG_EXCH:
        if (grid[0])
            snprintf(t, sizeof t, "\n%s DE %s TNX FER CALL UR RST 599 599 QTH %.4s %.4s\n%s DE %s K\n",
                     c, my, grid, grid, c, my);
        else
            snprintf(t, sizeof t, "\n%s DE %s TNX FER CALL UR RST 599 599\n%s DE %s K\n", c, my, c, my);
        break;
    default:
        snprintf(t, sizeof t, "\n%s DE %s TNX FER QSO 73 GL\n%s DE %s SK\n", c, my, c, my);
        break;
    }
    ESP_LOGI(TAG, "TX press: %s", which == MSG_CQ ? "CQ" : which == MSG_ANSWER ? "Answer" :
             which == MSG_EXCH ? "Exch" : "73");
    tx_send(t, true);
}

static void type_close(bool end_over)
{
    if (!s_kb_wrap) return;
    lv_obj_del(s_kb_wrap);
    s_kb_wrap = s_kb = s_kb_ta = NULL;
    /* Closing the keyboard ends the over once what was typed has gone. Only
     * when keyed: on an idle radio rtty_tx_send("") would START an over. */
    if (end_over && rtty_tx_keyed()) tx_send("", true);
}

/* Each character is sent as it is typed (approved: "chars sent live"). A
 * backspace cannot unsend; it only edits the field. */
static void type_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta_resetting || !s_kb_ta) return;
    const char *txt = lv_textarea_get_text(s_kb_ta);
    int n = (int)strlen(txt);
    if (n > s_typed_len) tx_send(txt + s_typed_len, false);
    s_typed_len = n;
    if (n > 160) {   /* keep the field short; what was sent is in the echo */
        s_ta_resetting = true;
        lv_textarea_set_text(s_kb_ta, "");
        s_ta_resetting = false;
        s_typed_len = 0;
    }
}

static void type_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) type_close(true);
}

/* The keyboard covers the text box only, not the pane: STOP must stay in
 * reach while typing. 960 x 280 keys + a 50 px line of what was typed. */
static void type_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_kb_wrap) { type_close(true); return; }   /* second press closes */
    s_kb_wrap = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_kb_wrap, MID_W - PANE_W, 330);
    lv_obj_set_pos(s_kb_wrap, PANE_W, 720 - 330);
    lv_obj_set_style_bg_color(s_kb_wrap, lv_color_hex(0x0d1116), 0);
    lv_obj_set_style_bg_opa(s_kb_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb_wrap, 0, 0);
    lv_obj_set_style_pad_all(s_kb_wrap, 0, 0);
    lv_obj_set_style_radius(s_kb_wrap, 0, 0);
    lv_obj_clear_flag(s_kb_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_kb_ta = lv_textarea_create(s_kb_wrap);
    lv_obj_set_size(s_kb_ta, MID_W - PANE_W, 50);
    lv_obj_set_pos(s_kb_ta, 0, 0);
    lv_obj_set_style_text_font(s_kb_ta, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(s_kb_ta, lv_color_hex(0xFF9F9F), 0);
    lv_obj_set_style_bg_color(s_kb_ta, lv_color_hex(0x14181e), 0);
    lv_textarea_set_placeholder_text(s_kb_ta, "Type - each key is sent as you type");
    s_typed_len = 0;
    lv_obj_add_event_cb(s_kb_ta, type_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_kb = lv_keyboard_create(s_kb_wrap);
    ui_theme_style_keyboard(s_kb);
    lv_obj_set_size(s_kb, MID_W - PANE_W, 280);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_theme_keyboard_attach_caps_cycle_upper(s_kb);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_28, 0);
    lv_keyboard_set_textarea(s_kb, s_kb_ta);
    lv_obj_add_event_cb(s_kb, type_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, type_kb_cb, LV_EVENT_CANCEL, NULL);
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "TX press: STOP");
    rtty_tx_stop();
    type_close(false);
}

/* CALL: a tap on a callsign in the decoded text (approved). The word under
 * the finger is taken from the text grid; it must be shaped like a callsign
 * (cw_call_structure_ok - shape only, the test the CW page uses). */
#define CALLCH(ch) (isalnum((unsigned char)(ch)) || (ch) == '/')
static void text_tap_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p; lv_indev_get_point(in, &p);
    lv_area_t a; lv_obj_get_coords(s_text_box, &a);
    const int pitch = (int)lv_font_get_line_height(&qmx_mono_25) + 2;
    int row = (p.y - a.y1 - TXT_PAD) / pitch;
    int col = (p.x - a.x1 - TXT_PAD) / 15;
    if (p.y - a.y1 < TXT_PAD || p.x - a.x1 < TXT_PAD) return;
    if (row < 0 || row >= TXT_ROWS || col < 0) return;
    const char *ln = s_lines[row];
    int len = (int)strlen(ln);
    if (col >= len || !CALLCH(ln[col])) return;
    int b = col, en = col;
    while (b > 0 && CALLCH(ln[b - 1])) b--;
    while (en < len && CALLCH(ln[en])) en++;
    char w[16];
    int n = en - b;
    if (n >= (int)sizeof w) return;
    for (int i = 0; i < n; i++) w[i] = (char)toupper((unsigned char)ln[b + i]);
    w[n] = 0;
    if (!cw_call_structure_ok(w)) { ESP_LOGI(TAG, "tap on '%s': not a callsign", w); return; }
    snprintf(s_call, sizeof s_call, "%s", w);
    ESP_LOGI(TAG, "call selected: %s", s_call);
}
#undef CALLCH

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
    /* dB over the noise floor, full scale RTTY_BAR_DB. They were each tone
     * over the LARGER tone, so on noise both sat near full and said nothing
     * (operator, 2026-11-12: "almost fully extended on the noise floor"). */
    set_bar(s_bar_m, (st.mark_db - RTTY_BAR_DB0) / (RTTY_BAR_DB - RTTY_BAR_DB0));
    set_bar(s_bar_s, (st.space_db - RTTY_BAR_DB0) / (RTTY_BAR_DB - RTTY_BAR_DB0));
    set_bar(s_bar_sig, st.sig_lvl);
    lv_obj_set_style_bg_color(s_bar_sig, lv_color_hex(st.present ? 0x30E060 : 0x5F6670), 0);

    char b[48];
    snprintf(b, sizeof b, "AFC %+d Hz", (int)(st.afc_hz + (st.afc_hz >= 0 ? 0.5f : -0.5f)));
    lv_label_set_text(s_lbl_afc, st.cfg.afc ? b : "AFC off");

    uint32_t dial = ui_get_dial_freq_hz();
    char s[128];
    rtty_tx_status_t tx;
    rtty_tx_status(&tx);
    /* A refusal from inside the over (dial move, band guard) arrives late. */
    if (!tx.keyed && tx.err[0] && strcmp(tx.err, s_err_shown) != 0) tx_show_err(tx.err);
    if (tx.keyed) s_err_until = 0;
    bool tx_line = tx.keyed || (int32_t)(s_err_until - lv_tick_get()) > 0;
    if (tx.keyed) {
        /* The last 24 characters sent: what fits 296 px at montserrat_20. */
        const char *ec = tx.echo;
        size_t el = strlen(ec);
        if (el > 24) ec += el - 24;
        snprintf(s, sizeof s, "TX %d s  centre %d Hz\n%s%s", tx.seconds, tx.centre_hz,
                 el > 24 ? "..." : "", ec);
    } else if (tx_line) {
        snprintf(s, sizeof s, "TX refused: %s", s_err_shown);
    } else if (st.fs_hz <= 0) {
        snprintf(s, sizeof s, "Waiting for audio\nfrom the radio");
    } else if (s_call[0]) {
        snprintf(s, sizeof s, "%s at %d Hz\nTo: %s", st.present ? "Decoding" : "Listening",
                 st.cfg.offset_hz + (int)st.afc_hz, s_call);
    } else {
        snprintf(s, sizeof s, "%s at %d Hz\n%lu chars", st.present ? "Decoding" : "Listening",
                 st.cfg.offset_hz + (int)st.afc_hz, (unsigned long)st.chars);
    }
    lv_label_set_text(s_lbl_status, s);
    lv_obj_set_style_text_font(s_lbl_status, tx_line ? &lv_font_montserrat_20 : &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(tx_line ? 0xFF7070 : 0xC8CCD2), 0);
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

    /* TX (R010): the box reserved at (10,372) 300x168 since R006, three rows
     * of 145x52 at a 58 px pitch. Layout approved 2026-11-14. */
    mk_btn(s_pane, 10, 372, 145, 52, "CQ", macro_cb, (void *)MSG_CQ, NULL);
    mk_btn(s_pane, 165, 372, 145, 52, "Answer", macro_cb, (void *)MSG_ANSWER, NULL);
    mk_btn(s_pane, 10, 430, 145, 52, "Exch", macro_cb, (void *)MSG_EXCH, NULL);
    mk_btn(s_pane, 165, 430, 145, 52, "73", macro_cb, (void *)MSG_73, NULL);
    mk_btn(s_pane, 10, 488, 145, 52, "Type...", type_open_cb, NULL, NULL);
    lv_obj_t *stop = mk_btn(s_pane, 165, 488, 145, 52, "STOP", stop_cb, NULL, NULL);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0xB02020), 0);

    s_lbl_status = mk_label(s_pane, 12, MID_H - 76, &lv_font_montserrat_26, 0xC8CCD2, "");
    lv_obj_set_style_text_line_space(s_lbl_status, 2, 0);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, 296);

    /* Decoded text, 8 rows, above the band-plan strip. */
    s_text_box = mk_rect(s_container, PANE_W, s_txt_top, TXT_BOX_W,
                         MID_H - BANDPLAN_H - s_txt_top, 0x0d1116);
    lv_obj_set_style_border_width(s_text_box, 1, 0);
    lv_obj_set_style_border_color(s_text_box, lv_color_hex(0x2a313b), 0);
    /* Eats its own taps: a tap on the text must not move the decoder to the
     * waterfall row hidden under it. A tap on a callsign selects CALL. */
    lv_obj_add_flag(s_text_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_text_box, text_tap_cb, LV_EVENT_CLICKED, NULL);
    const int pitch = (int)lv_font_get_line_height(&qmx_mono_25) + 2;
    for (int r = 0; r < TXT_ROWS; r++) {
        s_row[r] = mk_label(s_text_box, TXT_PAD, TXT_PAD + r * pitch, &qmx_mono_25, COL_OLD, "");
        lv_label_set_long_mode(s_row[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_row[r], TXT_BOX_W - 2 * TXT_PAD);
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
    /* ⛔ NOT foregrounded - the CW page's rule (cw_screen_view_show). Raising
     * the container put it over the left-edge strip, so neither the swipe nor
     * the grip tap reached left_drawer_open(): bench2 booted into RTTY and
     * could not leave the page (operator, 2026-11-12, "stuck there"). The
     * container is built after the spectrum/waterfall in ui_init(), so it
     * already sits above them, and below the strips ui_init() raises last. */
    s_shown = true;
}

void rtty_screen_view_hide(void)
{
    if (!s_container) return;
    speed_close();
    /* TX belongs to this page: leaving it stops an over at once (the decoder
     * it transmits on is switched off just below). */
    rtty_tx_stop();
    type_close(false);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_shown = false;
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.on = false;
    rtty_rx_apply(&c);
}

lv_obj_t *rtty_screen_view_get_container(void) { return s_container; }

/* The part of the waterfall NOT under the pane or the text box, for ui.c's
 * carpet shift - the CW page's rule (cw_screen_view_carpet_visible), for the
 * reason measured there: invalidating the whole carpet on every shift also
 * redraws the opaque pane and text box on top of it (CW: 57-62 ms frames
 * against 16 ms). Until this existed the RTTY page took that whole-carpet
 * path. [1] is the strip right of the text box, under the watermark: the box
 * stops short of it, so the carpet shows there and must move with the rest. */
bool rtty_screen_view_carpet_visible(lv_area_t a[2])
{
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return false;
    a[0].x1 = PANE_W;
    a[0].x2 = MID_W - 1;
    a[0].y1 = MID_Y;
    a[0].y2 = MID_Y + s_txt_top - 1;
    a[1].x1 = TXT_BOX_X2;
    a[1].x2 = MID_W - 1;
    a[1].y1 = MID_Y + s_txt_top;
    a[1].y2 = MID_Y + MID_H - BANDPLAN_H - 1;
    return true;
}
