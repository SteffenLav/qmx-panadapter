/* RTTY page - see rtty_screen_view.h. R006.
 *
 * Layout agreed with the operator 2026-11-12 from a 1280x720 mock-up, with
 * 8 text rows ("8 rows") over a 190 px waterfall - now 6 (operator,
 * 2026-11-14: "Reduce the decode lines from 8 to 6 - to give more space for
 * the wf"), +58 px of waterfall (2 x 29 px pitch). RTTY prints continuously,
 * ~6 characters/s, and the CW page's 4-row ticker would scroll text away
 * before it can be read. */
#include "rtty_screen_view.h"
#include "cw_screen_view.h"   /* CW_PANE_W - the shared left-pane width */
#include "ui_theme.h"
#include "ui.h"
#include "ui_mode.h"
#include "rtty_rx.h"
#include "rtty_tx.h"
#include "rtty_qso.h"
#include "rtty_log.h"
#include "rtty_log_modal.h"
#include "cat/cat.h"
#include "cw_callsign.h"
#include "storage/settings.h"
#include <ctype.h>
#include <math.h>
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

#define TXT_ROWS      6   /* was 8; operator 2026-11-14, for waterfall room */
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
#define COL_TX        0xFF5050   /* what we sent, red (operator 2026-11-14) */
#define COL_MARK      0xFF9F40   /* tone markers + level bars */
#define BTN_BG        0x2a2f37
#define BTN_ON        0x2d5a3f
/* TX pane, round 2 (operator approved docs/rtty-tx-mock2-*.png, 2026-11-14):
 * TX buttons blue-grey so they read apart from the RX settings above; the
 * call-needing ones dim until a call is tapped; the sending one orange, the
 * colour of the tone lines; STOP dark red idle, bright red keyed. */
#define TX_BTN        0x24324a
#define TX_DIM_BG     0x1b1f26
#define TX_DIM_TXT    0x5a616b
#define TX_SENDING    0xC0601A
#define STOP_IDLE     0x4a1c1c
#define STOP_IDLE_TXT 0xB08080
#define STOP_KEYED    0xE02020
#define COL_CALL      0x5FD4FF

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
static lv_obj_t *s_tx_btn[5], *s_tx_lbl[5];    /* CQ, Answer, Exch, 73, Type... */
static lv_obj_t *s_btn_stop, *s_lbl_stop;
static lv_obj_t *s_lbl_to, *s_lbl_call;        /* "To:" + the tapped call, on the divider */
static lv_obj_t *s_call_box;                   /* outline round the tapped call in the text */
static lv_obj_t *s_lbl_tx;                     /* "TX" on the divider; "TX #013" in contest mode */
/* R030 QSO card: over the waterfall above the text box, the CW station
 * card's place (cw_screen_view.c). Shown while a call is picked. */
static lv_obj_t *s_card, *s_card_call, *s_card_mark, *s_card_heard;
static bool      s_card_dupe, s_card_worked;   /* for s_call, worked out once per pick */
static char      s_card_shown[72];             /* what the card last showed */
static char      s_tx_shown[24];               /* last divider text */
static int       s_call_row = -1, s_call_col, s_call_len;
static int       s_sending = -1;               /* s_tx_btn index lit while keyed, -1 none */
static int       s_tx_style = -1;              /* last applied tx_style_key(), -1 = never */
static int       s_typed_len;                  /* chars of s_kb_ta already sent */
static bool      s_ta_resetting;               /* our own lv_textarea_set_text() */
static char      s_call[16];                   /* copy of rtty_tx_get_call(): the station being worked */
static uint32_t  s_call_gen;                   /* rtty_tx_call_gen() that s_call reflects */
static char      s_err_shown[96];              /* rtty_tx's last refusal, as last shown */
static uint32_t  s_err_until;                  /* lv_tick until which it stays up */
static lv_timer_t *s_timer;
static bool s_shown;
static int  s_txt_top;      /* container y of the text box */

/* Text grid: TXT_ROWS lines, the last is the one being written. */
static char     s_lines[TXT_ROWS][TXT_COLS + 1];
static int      s_col;
static uint32_t s_cursor;
static uint32_t s_tx_cursor;     /* rtty_tx_text_read() position */
static bool     s_row_tx[TXT_ROWS];   /* the row holds sent text: drawn red */
static bool     s_cur_tx;        /* the line being written is sent text */
static bool     s_fresh;         /* a source switch just opened this line */
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
    memmove(&s_row_tx[0], &s_row_tx[1], sizeof(s_row_tx[0]) * (TXT_ROWS - 1));
    s_lines[TXT_ROWS - 1][0] = 0;
    s_row_tx[TXT_ROWS - 1] = s_cur_tx;
    s_col = 0;
    if (s_call_row >= 0) s_call_row--;   /* the boxed call moved up a row */
}

/* Sent and received text never share a row: one colour per label. A switch
 * of source starts a new line unless the current one is still empty. */
static void text_source(bool tx)
{
    if (tx == s_cur_tx) return;
    s_cur_tx = tx;
    if (s_col > 0) text_newline();
    s_row_tx[TXT_ROWS - 1] = tx;
    s_fresh = true;
}

static void text_put(char c)
{
    if (c == '\r') return;
    /* An over starts with '\n', and text_source() already opened a line. */
    bool fresh = s_fresh;
    s_fresh = false;
    if (c == '\n' && fresh && s_col == 0) return;
    if (c == '\n') { text_newline(); s_text_dirty = true; return; }
    if (s_col >= TXT_COLS) text_newline();
    s_lines[TXT_ROWS - 1][s_col++] = c;
    s_lines[TXT_ROWS - 1][s_col] = 0;
    s_text_dirty = true;
}

static void text_clear(void)
{
    memset(s_lines, 0, sizeof(s_lines));
    memset(s_row_tx, 0, sizeof(s_row_tx));
    s_cur_tx = false;
    s_call_row = -1;
    s_col = 0;
    s_text_dirty = true;
}

static void text_render(void)
{
    if (!s_text_dirty) return;
    s_text_dirty = false;
    for (int r = 0; r < TXT_ROWS; r++) {
        lv_label_set_text(s_row[r], s_lines[r]);
        uint32_t col = s_row_tx[r] ? COL_TX : (r == TXT_ROWS - 1 ? COL_NEW : COL_OLD);
        lv_obj_set_style_text_color(s_row[r], lv_color_hex(col), 0);
    }
    if (!s_call_box) return;
    if (s_call_row < 0) { lv_obj_add_flag(s_call_box, LV_OBJ_FLAG_HIDDEN); return; }
    const int pitch = (int)lv_font_get_line_height(&qmx_mono_25) + 2;
    lv_obj_set_pos(s_call_box, TXT_PAD + s_call_col * 15 - 3, TXT_PAD + s_call_row * pitch - 2);
    lv_obj_set_size(s_call_box, s_call_len * 15 + 6, pitch + 2);
    lv_obj_clear_flag(s_call_box, LV_OBJ_FLAG_HIDDEN);
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
    /* R028: green while Auto is on (the pane's convention, green = on). */
    lv_obj_set_style_text_color(s_lbl_speed, lv_color_hex(c.auto_mode ? 0x30E060 : 0x5FD4FF), 0);
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

static const float SPEEDS[] = { 45.45f, 50.0f, 75.0f, 100.0f };   /* rtty_auto_bauds too */
#define NSPEEDS ((int)(sizeof SPEEDS / sizeof SPEEDS[0]))
#define PICK_AUTO 99
static const int   SHIFTS[] = { 170, 200, 425, 450, 850 };

/* ⛔ ASYNC: speed_pick_cb / speed_close_cb are CLICKED handlers of the modal
 * or of buttons inside it, so a plain delete freed the object LVGL was still
 * dispatching through - the fault type_close() hit (see there). The modal is
 * rebuilt on every open to show the current choice, so it is deleted, but
 * only after the event has finished. */
static void speed_close(void)
{
    if (s_speed_modal) { lv_obj_delete_async(s_speed_modal); s_speed_modal = NULL; }
}

static void speed_pick_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    /* A fixed speed or shift is a choice and turns Auto (R028) off; Auto
     * toggles. The web page's selects do the same (webserver.c). */
    if (v == PICK_AUTO) c.auto_mode = !c.auto_mode;
    else if (v < 100) { c.baud = SPEEDS[v]; c.auto_mode = false; }
    else { c.shift_hz = (float)SHIFTS[v - 100]; c.auto_mode = false; }
    rtty_rx_apply(&c);
    refresh_buttons();
    speed_close();
}

static void speed_close_cb(lv_event_t *e) { (void)e; speed_close(); }
static void speed_contest_cb(lv_event_t *e) { (void)e; speed_close(); rtty_contest_modal_open(); }

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
    for (int i = 0; i < NSPEEDS; i++) {
        char b[12];
        if (i == 0) snprintf(b, sizeof b, "45.45"); else snprintf(b, sizeof b, "%d", (int)SPEEDS[i]);
        lv_obj_t *bt = mk_btn(p, i * 160, 34, 150, 56, b, speed_pick_cb, (void *)(intptr_t)i, NULL);
        if (SPEEDS[i] == c.baud) lv_obj_set_style_bg_color(bt, lv_color_hex(BTN_ON), 0);
    }
    /* R028: Auto - speed, shift and Reverse follow the station. Green = on,
     * as the pane's toggles; the speed/shift buttons still show what runs. */
    {
        lv_obj_t *lbl;
        lv_obj_t *bt = mk_btn(p, NSPEEDS * 160, 34, 150, 56, "Auto", speed_pick_cb,
                              (void *)(intptr_t)PICK_AUTO, &lbl);
        if (c.auto_mode) lv_obj_set_style_text_color(lbl, lv_color_hex(0x30E060), 0);
        if (c.auto_mode) lv_obj_set_style_border_color(bt, lv_color_hex(0x30E060), 0);
        if (c.auto_mode) lv_obj_set_style_border_width(bt, 3, 0);
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
    /* R030: the contest settings, reachable from here and from the "TX" label. */
    mk_btn(p, 0, 216, 200, 56, "Contest...", speed_contest_cb, NULL, NULL);
}

/* ---- TX (R010) ------------------------------------------------------------
 * Layout approved by the operator 2026-11-14 (docs/rtty-tx-mock.png), restyled
 * the same day (docs/rtty-tx-mock2-*.png). Every
 * press here is a human decision that can key the radio: the rows are in
 * docs/unattended-tx-inventory.md. The refusals (callsign, sim, WSPR,
 * band guard, ...) are rtty_tx_send()'s, not this file's. */

/* Show a refusal for 8 s in the status lines. */
static void tx_show_err(const char *e)
{
    snprintf(s_err_shown, sizeof s_err_shown, "%s", e);
    s_err_until = lv_tick_get() + 8000;
}

static bool tx_send(const char *text, bool end)
{
    char err[96] = "";
    if (rtty_tx_send(text, end, err, sizeof err) == ESP_OK) return true;
    tx_show_err(err);
    return false;
}

/* The four fixed messages: built in rtty_tx_macro() (rtty_tx.c), which the
 * web page calls too, so both send the same text. */
static void macro_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    char err[96] = "";
    ESP_LOGI(TAG, "TX press: %s", which == RTTY_MSG_CQ ? "CQ" : which == RTTY_MSG_ANSWER ? "Answer" :
             which == RTTY_MSG_EXCH ? "Exch" : "73");
    if (rtty_tx_macro((rtty_msg_t)which, err, sizeof err) == ESP_OK) {
        s_sending = which;
        /* R030: 73/TU ends the QSO - ask to log it. Only from THIS press, not
         * from rtty_tx_macro(): a 73 sent from the web page must not pop a
         * dialog on a Tab5 nobody may be looking at. */
        if (which == RTTY_MSG_73) rtty_log_modal_open();
    } else {
        tx_show_err(err);
    }
}

/* ⛔ HIDE, NEVER DELETE. This deleted s_kb_wrap, and it is called from the
 * keyboard's own READY/CANCEL event: LVGL went on dispatching through the
 * freed keyboard - bench2 2026-11-14, Instruction access fault in taskLVGL
 * (lv_keyboard_def_event_cb -> lv_obj_event_base), cyan screen, right after
 * the first typed over ended. The same fault is recorded at
 * still_notice_ok_cb() in ui.c, and every modal there hides. */
static bool type_is_open(void)
{
    return s_kb_wrap && !lv_obj_has_flag(s_kb_wrap, LV_OBJ_FLAG_HIDDEN);
}

static bool s_kb_phys;   /* layout last applied: a physical keyboard was up */

/* Physical keyboards (operator 2026-11-14: "I want to use my ble keyb ... on
 * screen keyb should go away or not be shown if ble is connected", and "the
 * attachable keyb should work"). Both the BLE and the snap-on keyboard type
 * into whatever ui_kbd_note_focus() names, and ui_osk_show() is the project's
 * one gate that keeps the on-screen keyboard away while either is connected.
 * This line was opened by a button, never focused, and showed its keyboard
 * directly - so neither physical keyboard reached it. With one connected the
 * line alone shows, ABOVE the text box, so the decoded text stays in view. */
static void type_layout(void)
{
    s_kb_phys = ui_phys_kbd_active();
    const int w = MID_W - PANE_W;
    if (s_kb_phys) {
        lv_obj_set_size(s_kb_wrap, w, 50);
        lv_obj_set_pos(s_kb_wrap, PANE_W, MID_Y + s_txt_top - 50);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_size(s_kb_wrap, w, 330);
        lv_obj_set_pos(s_kb_wrap, PANE_W, 720 - 330);
    }
    ui_osk_show(s_kb);   /* hides it again itself while a keyboard is up */
}

static void type_close(bool end_over)
{
    if (!type_is_open()) return;
    lv_obj_add_flag(s_kb_wrap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_kb_ta, LV_STATE_FOCUSED);
    ui_kbd_note_unfocus(s_kb_ta);
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
    if (n > s_typed_len && tx_send(txt + s_typed_len, false)) s_sending = 4;
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

/* The line's two buttons, registered with ui_kbd_set_buttons(): a physical
 * keyboard's Enter clicks the first (a new line is sent), Esc the second. */
static void type_newline_cb(lv_event_t *e) { (void)e; if (s_kb_ta) lv_textarea_add_char(s_kb_ta, '\n'); }
static void type_done_cb(lv_event_t *e)    { (void)e; type_close(true); }

/* The keyboard covers the text box only, not the pane: STOP must stay in
 * reach while typing. 960 x 280 keys + a 50 px line of what was typed. */
static void type_open_cb(lv_event_t *e)
{
    (void)e;
    if (type_is_open()) { type_close(true); return; }   /* second press closes */
    if (!s_kb_wrap) {                                    /* built once, reused */
        const int w = MID_W - PANE_W, bw = 100;
        s_kb_wrap = lv_obj_create(lv_layer_top());
        lv_obj_set_style_bg_color(s_kb_wrap, lv_color_hex(0x0d1116), 0);
        lv_obj_set_style_bg_opa(s_kb_wrap, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_kb_wrap, 0, 0);
        lv_obj_set_style_pad_all(s_kb_wrap, 0, 0);
        lv_obj_set_style_radius(s_kb_wrap, 0, 0);
        lv_obj_clear_flag(s_kb_wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_kb_wrap, LV_OBJ_FLAG_HIDDEN);

        s_kb_ta = lv_textarea_create(s_kb_wrap);
        lv_obj_set_size(s_kb_ta, w - 2 * (bw + 6), 50);
        lv_obj_set_pos(s_kb_ta, 0, 0);
        lv_obj_set_style_text_font(s_kb_ta, &qmx_mono_25, 0);
        lv_obj_set_style_text_color(s_kb_ta, lv_color_hex(0xFF9F9F), 0);
        lv_obj_set_style_bg_color(s_kb_ta, lv_color_hex(0x14181e), 0);
        lv_textarea_set_placeholder_text(s_kb_ta, "Type - each key is sent as you type");
        lv_obj_add_event_cb(s_kb_ta, type_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *nl = mk_btn_f(s_kb_wrap, w - 2 * bw - 6, 2, bw, 46, &lv_font_montserrat_24,
                                LV_SYMBOL_NEW_LINE, type_newline_cb, NULL, NULL);
        lv_obj_t *dn = mk_btn_f(s_kb_wrap, w - bw, 2, bw, 46, &lv_font_montserrat_24,
                                "Done", type_done_cb, NULL, NULL);
        ui_kbd_set_buttons(nl, dn);

        s_kb = lv_keyboard_create(s_kb_wrap);
        ui_theme_style_keyboard(s_kb);
        lv_obj_set_size(s_kb, w, 280);
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        ui_theme_keyboard_attach_caps_cycle_upper(s_kb);
        lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_28, 0);
        lv_keyboard_set_textarea(s_kb, s_kb_ta);
        lv_obj_add_event_cb(s_kb, type_kb_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_kb, type_kb_cb, LV_EVENT_CANCEL, NULL);
    }
    s_ta_resetting = true;
    lv_textarea_set_text(s_kb_ta, "");
    s_ta_resetting = false;
    s_typed_len = 0;
    lv_obj_clear_flag(s_kb_wrap, LV_OBJ_FLAG_HIDDEN);
    type_layout();
    lv_obj_add_state(s_kb_ta, LV_STATE_FOCUSED);   /* cursor shows where keys go */
    ui_kbd_note_focus(s_kb_ta);
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "TX press: STOP");
    rtty_tx_stop();
    type_close(false);
}

/* R030. A new station: start the exchange capture from what is on screen
 * now - its report may already be in the line the call was tapped in. Sent
 * rows are left out: our own 599 is not theirs. */
static void qso_seed_from_grid(void)
{
    rtty_qso_reset();
    for (int r = 0; r < TXT_ROWS; r++) {
        if (s_row_tx[r] || !s_lines[r][0]) continue;
        rtty_qso_feed(s_lines[r], strlen(s_lines[r]));
        rtty_qso_feed(" ", 1);
    }
}

/* Dupe / worked-before for the picked call, once per pick (the first dupe
 * check of a contest scans the device log). */
static void card_new_call(void)
{
    uint32_t hz = cat_get_frequency();
    s_card_dupe = rtty_log_is_dupe(s_call, hz);
    s_card_worked = !s_card_dupe && rtty_log_worked_on_band(s_call, hz);
    s_card_shown[0] = 0;
}

static void card_log_cb(lv_event_t *e) { (void)e; if (s_call[0]) rtty_log_modal_open(); }
static void contest_open_cb(lv_event_t *e) { (void)e; rtty_contest_modal_open(); }

static void card_refresh(void)
{
    if (!s_card) return;
    rtty_log_heard_t hs;
    memset(&hs, 0, sizeof hs);
    if (!s_call[0]) {
        rtty_log_set_heard(&hs);   /* the web shows nothing either */
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    settings_rtty_contest_t ct;
    settings_get_rtty_contest(&ct);
    char my[16], rst[8], ex[16], h[40];
    bool me;
    settings_get_my_callsign(my, sizeof my);
    bool heard = rtty_qso_get(my, rst, sizeof rst, ex, sizeof ex, &me);
    if (heard) snprintf(h, sizeof h, "Rcvd %s %s", rst, ct.on ? ex : "");
    else       snprintf(h, sizeof h, "Rcvd -");
    /* The same facts for the web page's log line (R030). */
    snprintf(hs.call, sizeof hs.call, "%s", s_call);
    if (heard) { snprintf(hs.rst, sizeof hs.rst, "%s", rst); snprintf(hs.exch, sizeof hs.exch, "%s", ex); }
    hs.heard = heard; hs.to_me = heard && me; hs.dupe = s_card_dupe; hs.worked = s_card_worked;
    rtty_log_set_heard(&hs);
    char sent[12] = "";
    if (ct.on) rtty_log_sent_exch(sent, sizeof sent);
    /* the key holds everything shown, so only a change re-sets the labels */
    char key[sizeof s_card_shown];
    snprintf(key, sizeof key, "%s|%s|%s|%d%d%d", s_call, h, sent, s_card_dupe, s_card_worked, ct.on);
    if (strcmp(key, s_card_shown) != 0) {
        snprintf(s_card_shown, sizeof s_card_shown, "%s", key);
        lv_label_set_text(s_card_call, s_call);
        lv_label_set_text(s_card_heard, h);
        char m[24];
        uint32_t mc;
        if (s_card_dupe)        { snprintf(m, sizeof m, "DUPE"); mc = 0xFF5050; }
        else if (ct.on)         { snprintf(m, sizeof m, "Send %s", sent); mc = 0xFF9F40; }
        else if (s_card_worked) { snprintf(m, sizeof m, "Worked on band"); mc = 0x8A93A0; }
        else                    { snprintf(m, sizeof m, "New"); mc = 0x30E060; }
        lv_label_set_text(s_card_mark, m);
        lv_obj_set_style_text_color(s_card_mark, lv_color_hex(mc), 0);
        lv_obj_set_style_border_color(s_card, lv_color_hex(s_card_dupe ? 0xFF5050 : 0x555555), 0);
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

/* The divider's "TX" says when contest mode is on, and what goes out next. */
static void tx_label_refresh(void)
{
    settings_rtty_contest_t ct;
    settings_get_rtty_contest(&ct);
    char t[24];
    if (ct.on) { char ex[12]; rtty_log_sent_exch(ex, sizeof ex); snprintf(t, sizeof t, "TX #%s", ex); }
    else       snprintf(t, sizeof t, "TX");
    if (!strcmp(t, s_tx_shown)) return;
    snprintf(s_tx_shown, sizeof s_tx_shown, "%s", t);
    lv_label_set_text(s_lbl_tx, t);
    lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(ct.on ? 0xFF9F40 : 0x8A93A0), 0);
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
    /* R030: with the log dialog up, a tapped word fills its field instead of
     * picking a new station. */
    if (rtty_log_modal_is_open()) { rtty_log_modal_fill_word(w); return; }
    if (!cw_call_structure_ok(w)) { ESP_LOGI(TAG, "tap on '%s': not a callsign", w); return; }
    rtty_tx_set_call(w);   /* shape already checked; shared with the web */
    snprintf(s_call, sizeof s_call, "%s", w);
    s_call_gen = rtty_tx_call_gen();
    qso_seed_from_grid();
    card_new_call();
    s_call_row = row; s_call_col = b; s_call_len = n;
    s_text_dirty = true;   /* text_render() places the box */
    s_tx_style = -1;       /* undim Answer / Exch / 73, show To: */
    ESP_LOGI(TAG, "call selected: %s", s_call);
}
#undef CALLCH

static void tx_btn_style(int i, uint32_t bg, uint32_t fg)
{
    lv_obj_set_style_bg_color(s_tx_btn[i], lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(s_tx_lbl[i], lv_color_hex(fg), 0);
}

/* Re-style only on a change: keyed, which button is sending, call or not. */
static void tx_refresh(bool keyed)
{
    if (!keyed) s_sending = -1;
    int key = (keyed ? 1 : 0) | ((s_sending + 1) << 1) | (s_call[0] ? 64 : 0);
    if (key == s_tx_style) return;
    s_tx_style = key;
    for (int i = 0; i < 5; i++) {
        bool needs_call = (i >= 1 && i <= 3);
        if (keyed && i == s_sending)       tx_btn_style(i, TX_SENDING, 0xFFFFFF);
        else if (needs_call && !s_call[0]) tx_btn_style(i, TX_DIM_BG, TX_DIM_TXT);
        else                               tx_btn_style(i, TX_BTN, 0xE8E8E8);
    }
    lv_obj_set_style_bg_color(s_btn_stop, lv_color_hex(keyed ? STOP_KEYED : STOP_IDLE), 0);
    lv_obj_set_style_text_color(s_lbl_stop, lv_color_hex(keyed ? 0xFFFFFF : STOP_IDLE_TXT), 0);
    if (s_call[0]) {
        lv_label_set_text(s_lbl_call, s_call);
        lv_obj_clear_flag(s_lbl_call, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_to, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(s_lbl_call);
        lv_obj_align_to(s_lbl_to, s_lbl_call, LV_ALIGN_OUT_LEFT_MID, 0, 0);
    } else {
        lv_obj_add_flag(s_lbl_call, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_to, LV_OBJ_FLAG_HIDDEN);
    }
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

/* The two tones in absolute Hz, cached so a pan can re-place the lines
 * without a status fetch. 0 = nothing to draw yet. */
static int64_t s_marker_hz[2];

/* ⛔ POSITION FROM THE LIVE DRAG, NOT FROM THE COMMITTED PAN. While the
 * spectrum is being dragged the canvas slides bodily by whole pixels and
 * ui_get_pan_offset_hz() has not moved yet, so hz_to_x() alone leaves these
 * lines standing where the view used to be until the release. Operator,
 * 2026-11-19: "everything moves smoothly with the mouse pointer except the
 * two orange guide lines". */
static void markers_redraw(void)
{
    int64_t lo; int32_t span;
    ui_screen_view_hz(&lo, &span);
    const int off = ui_stroll_offset_px();
    for (int i = 0; i < 2; i++) {
        if (!s_marker[i]) continue;
        const bool on = (s_marker_hz[i] > 0 && span > 0);
        const int  x  = on ? hz_to_x(s_marker_hz[i]) + off : 0;
        /* ⛔ The clamp runs AFTER the offset, never before: a line being
         * dragged in from off-screen has a negative hz_to_x() and a positive
         * offset, and testing the unshifted x would keep it hidden the whole
         * way in. */
        if (!on || x < ui_spec_x0() || x >= MID_W - 1) {
            lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_marker[i], x - 1);
    }
}

static void place_markers(const rtty_rx_status_t *st, uint32_t dial)
{
    float half = st->cfg.shift_hz * 0.5f;
    float centre = (float)st->cfg.offset_hz + st->afc_hz;
    for (int i = 0; i < 2; i++) {
        float off = centre + (i ? half : -half);
        s_marker_hz[i] = dial ? (int64_t)dial + (int64_t)off : 0;
    }
    markers_redraw();
}

/* The spectrum is being dragged. Called from the gesture at its own rate -
 * the page timer runs at 200 ms, which over a 33 Hz drag would step the lines
 * along in visible jumps behind a smoothly sliding canvas. */
void rtty_screen_view_pan_moved(void)
{
    if (s_shown) markers_redraw();
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

    /* The station being worked, changed on the web page: take it, and drop
     * the box (its row/column are only known for a tap here). */
    uint32_t cg = rtty_tx_call_gen();
    if (cg != s_call_gen) {
        s_call_gen = cg;
        rtty_tx_get_call(s_call, sizeof s_call);
        qso_seed_from_grid();
        card_new_call();
        /* The station changed on the web page - or it logged the QSO there,
         * which clears the call. A dialog still open here is for the old one
         * and would log it a second time: close it. */
        rtty_log_modal_close_log();
        s_call_row = -1;
        s_text_dirty = true;
        s_tx_style = -1;
    }

    /* A Clear from either surface (this pane or the web page). */
    uint32_t g = rtty_rx_text_clear_gen();
    if (g != s_clear_seen) {
        s_clear_seen = g;
        s_cursor = rtty_rx_text_count();
        s_tx_cursor = rtty_tx_text_count();
        text_clear();
    }
    char buf[128];
    size_t n;
    bool rx_any = false;
    while ((n = rtty_rx_text_read(&s_cursor, buf, sizeof buf)) > 0) {
        if (!rx_any) { text_source(false); rx_any = true; }
        for (size_t i = 0; i < n; i++) text_put(buf[i]);
        if (s_call[0]) rtty_qso_feed(buf, n);
    }
    bool tx_any = false;
    while ((n = rtty_tx_text_read(&s_tx_cursor, buf, sizeof buf)) > 0) {
        if (!tx_any) { text_source(true); tx_any = true; }
        for (size_t i = 0; i < n; i++) text_put(buf[i]);
    }
    text_render();

    rtty_rx_service();   /* logs + Auto decisions, off the audio task - see rtty_rx.h */
    rtty_rx_status_t st;
    rtty_rx_status(&st);
    /* R028: Auto changes speed/shift/Reverse from the audio task, and the web
     * page changes any of them - repaint the buttons when what they show is
     * no longer what runs. */
    {
        static rtty_rx_cfg_t shown;
        static bool shown_ok;
        const rtty_rx_cfg_t *k = &st.cfg;
        if (!shown_ok || k->baud != shown.baud || k->shift_hz != shown.shift_hz ||
            k->reverse != shown.reverse || k->afc != shown.afc || k->squelch != shown.squelch ||
            k->auto_mode != shown.auto_mode) {
            refresh_buttons();
            shown = *k; shown_ok = true;
        }
    }
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
    /* ONE line since round 2 (the call moved to the divider, the centre is
     * what the orange lines show). A refusal longer than the pane scrolls. */
    if (tx.keyed) {
        /* The last 12 characters sent: what fits beside "TX 99 s" at 24 px. */
        const char *ec = tx.echo;
        size_t el = strlen(ec);
        if (el > 12) ec += el - 12;
        snprintf(s, sizeof s, "TX %d s  %s%s", tx.seconds, el > 12 ? "..." : "", ec);
    } else if (tx_line) {
        snprintf(s, sizeof s, "%s", s_err_shown);
    } else if (st.fs_hz <= 0) {
        snprintf(s, sizeof s, "Waiting for audio");
    } else {
        snprintf(s, sizeof s, "%s at %d Hz", st.present ? "Decoding" : "Listening",
                 st.cfg.offset_hz + (int)st.afc_hz);
    }
    if (strcmp(lv_label_get_text(s_lbl_status), s) != 0) lv_label_set_text(s_lbl_status, s);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(tx_line ? 0xFF5050 : 0xC8CCD2), 0);
    tx_refresh(tx.keyed);
    card_refresh();
    tx_label_refresh();
    if (type_is_open() && ui_phys_kbd_active() != s_kb_phys) type_layout();
    place_markers(&st, dial);
}

/* ⛔ A TAP TUNES THE DIAL. Until 2026-11-14 it moved only the decoder and
 * left the dial (R006), so "Freq:" kept the dial while the lines sat kHz away
 * ("the Freq is NOT adjusting to the tap" - operator, who chose this over a
 * Freq readout of dial + offset). The dial goes where the tapped signal lands
 * at RTTY_TAP_AUDIO_HZ: mid-passband, so TX needs no dial move (rtty_tx's QSY
 * stays for an offset set any other way). The decoder offset is the exact
 * remainder after the dial's own quantisation. If the radio refuses the write
 * the dial has not moved, and the decoder goes onto the tap from where it is. */
void rtty_screen_view_tap_hz(uint32_t tapped_hz)
{
    /* Not an RF frequency: refuse. A keypad misread of 140 Hz got through
     * here, kept the dial, and put the decoder at -14 MHz (clamped to -3000). */
    if (tapped_hz < RTTY_TUNE_MIN_HZ) {
        ESP_LOGW(TAG, "tap: %lu Hz is not an RF frequency - ignored", (unsigned long)tapped_hz);
        ui_toast("Not a valid frequency");
        return;
    }
    uint32_t dial = cat_get_frequency();
    if (tapped_hz > RTTY_TAP_AUDIO_HZ) {
        uint32_t want = cat_quantise_dial_hz(tapped_hz - RTTY_TAP_AUDIO_HZ);
        if (cat_set_frequency_forced(want) == ESP_OK) {
            ui_update_frequency(want);
            dial = want;
        } else {
            ESP_LOGW(TAG, "tap: dial write to %lu refused - decoder moved, dial kept",
                     (unsigned long)want);
        }
    }
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.offset_hz = (int)((int64_t)tapped_hz - (int64_t)dial);
    c.on = true;
    rtty_rx_apply(&c);
    ESP_LOGI(TAG, "tap: %lu Hz -> dial %lu, decoder at %d Hz", (unsigned long)tapped_hz,
             (unsigned long)dial, c.offset_hz);
}

/* ⛔ A DIAL MOVE RESTARTS THE DECODER, AS A TAP DOES. Operator 2026-11-14:
 * "if i use the dial or the tap to tune - in both cases we need to have the
 * AFC in action - now it looks like its only the tap to tune". From the code:
 * once a station is decoded, acq_held keeps AFC and acquisition within
 * RTTY_HOLD_HZ (20 Hz) of it (R026), and only a restart clears that. A tap
 * restarts (rtty_rx_apply); the knob did not, so after the first decode the
 * lines moved onto a new station while AFC stayed on the old one's offset.
 * The offset from the dial is kept - the lines move with the dial, as a
 * radio's audio does. Not while keyed: rtty_tx's own QSY applies the cfg it
 * needs, and a dial change mid-over is not a retune by the operator. */
void rtty_screen_view_dial_moved(void)
{
    if (ui_mode_get() != UI_MODE_RTTY || rtty_tx_keyed()) return;
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    if (!c.on) return;
    rtty_rx_apply(&c);
}

/* See the header: Freq on this page is the mark frequency. */
int rtty_screen_view_mark_offset_hz(void)
{
    rtty_rx_status_t st;
    rtty_rx_status(&st);
    int c = st.cfg.offset_hz + (st.afc_held ? (int)lroundf(st.afc_hz) : 0);
    int h = (int)lroundf(st.cfg.shift_hz / 2.0f);
    return st.cfg.reverse ? c - h : c + h;
}

void rtty_screen_view_tune_mark_hz(uint32_t mark_hz)
{
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    int h = (int)lroundf(c.shift_hz / 2.0f);
    int64_t centre = (int64_t)mark_hz + (c.reverse ? h : -h);
    if (centre <= 0) return;
    ESP_LOGI(TAG, "tune: mark %lu Hz -> centre %lld Hz", (unsigned long)mark_hz, (long long)centre);
    rtty_screen_view_tap_hz((uint32_t)centre);
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
    /* Round 2: a "TX" divider at y 378 with the tapped call at its right
     * end, then 56 px rows at a 62 px pitch - the RX rows' size and pitch. */
    s_lbl_tx = mk_label(s_pane, 14, 366, &lv_font_montserrat_22, 0x8A93A0, "TX");
    /* R030: a tap on it opens the contest settings. It sits over the
     * divider line, so it gets the pane colour behind, and a finger-sized
     * hit area round a 22 px label. */
    lv_obj_set_style_bg_color(s_lbl_tx, lv_color_hex(0x14181e), 0);
    lv_obj_set_style_bg_opa(s_lbl_tx, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_right(s_lbl_tx, 8, 0);
    lv_obj_add_flag(s_lbl_tx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_lbl_tx, 18);
    lv_obj_add_event_cb(s_lbl_tx, contest_open_cb, LV_EVENT_CLICKED, NULL);
    mk_rect(s_pane, 50, 377, 260, 2, 0x3a414c);
    /* The line is made after the label, so it drew across "TX #007" (bench1
     * 2026-11-16, R030): the label goes back on top, its pane-coloured
     * background then hides the line under it. "TX" alone ended left of x 50,
     * which is why this never showed before contest mode. */
    lv_obj_move_foreground(s_lbl_tx);
    s_lbl_call = mk_label(s_pane, 0, 0, &lv_font_montserrat_24, COL_CALL, "");
    lv_obj_align(s_lbl_call, LV_ALIGN_TOP_RIGHT, -10, 364);
    s_lbl_to = mk_label(s_pane, 0, 0, &lv_font_montserrat_24, 0x8A93A0, "To: ");
    /* Both sit over the divider line: give them the pane colour behind. */
    lv_obj_t *cover[2] = { s_lbl_call, s_lbl_to };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(cover[i], lv_color_hex(0x14181e), 0);
        lv_obj_set_style_bg_opa(cover[i], LV_OPA_COVER, 0);
    }
    lv_obj_set_style_pad_left(s_lbl_to, 8, 0);
    lv_obj_add_flag(s_lbl_call, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_to, LV_OBJ_FLAG_HIDDEN);

    static const char *const TXN[5] = { "CQ", "Answer", "Exch", "73", "Type..." };
    for (int i = 0; i < 5; i++) {
        int x = (i & 1) ? 165 : 10, y = 396 + (i / 2) * 62;
        s_tx_btn[i] = mk_btn(s_pane, x, y, 145, 56, TXN[i], i < 4 ? macro_cb : type_open_cb,
                             (void *)(intptr_t)i, &s_tx_lbl[i]);
    }
    s_btn_stop = mk_btn(s_pane, 165, 520, 145, 56, "STOP", stop_cb, NULL, &s_lbl_stop);

    s_lbl_status = mk_label(s_pane, 12, 586, &lv_font_montserrat_24, 0xC8CCD2, "");
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_status, 296);
    s_tx_style = -1;

    /* Decoded text, TXT_ROWS rows, above the band-plan strip. */
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
    /* Outline round the tapped call; follows it up as the text scrolls. */
    s_call_box = mk_rect(s_text_box, 0, 0, 10, 10, 0x0d1116);
    lv_obj_set_style_bg_opa(s_call_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_call_box, 2, 0);
    lv_obj_set_style_border_color(s_call_box, lv_color_hex(COL_CALL), 0);
    lv_obj_add_flag(s_call_box, LV_OBJ_FLAG_HIDDEN);

    /* R030 QSO card, 330 x 122, right-aligned with the text box, just above it. */
    s_card = mk_rect(s_container, TXT_BOX_X2 - 330, s_txt_top - 132, 330, 122, 0x1c2128);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(s_card, 8, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   /* eats taps: not a waterfall tune */
    s_card_call  = mk_label(s_card, 10, 6,  &lv_font_montserrat_28, COL_CALL, "");
    s_card_mark  = mk_label(s_card, 10, 46, &lv_font_montserrat_22, 0x30E060, "");
    s_card_heard = mk_label(s_card, 10, 80, &lv_font_montserrat_22, 0xE8E8E8, "");
    lv_obj_t *cl[3] = { s_card_call, s_card_mark, s_card_heard };
    for (int i = 0; i < 3; i++) {
        lv_label_set_long_mode(cl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(cl[i], 190);
    }
    lv_obj_t *lb = mk_btn(s_card, 206, 8, 112, 102, "Log", card_log_cb, NULL, NULL);
    lv_obj_set_style_bg_color(lb, lv_color_hex(UI_COLOR_SUCCESS), 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
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
    s_tx_cursor = rtty_tx_text_count();
    refresh_buttons();
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    /* â›” NOT foregrounded - the CW page's rule (cw_screen_view_show). Raising
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
    rtty_log_modal_close();   /* R030; the contest dialog saves as it closes */
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_shown = false;
    rtty_rx_cfg_t c;
    rtty_rx_get_cfg(&c);
    c.on = false;
    rtty_rx_apply(&c);
}

lv_obj_t *rtty_screen_view_get_container(void) { return s_container; }

int rtty_screen_view_text_top_y(void) { return MID_Y + s_txt_top; }
int rtty_screen_view_text_bottom_y(void) { return MID_Y + MID_H - BANDPLAN_H; }

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
