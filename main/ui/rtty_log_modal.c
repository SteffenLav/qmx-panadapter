/* RTTY log confirmation + contest settings dialogs - see rtty_log_modal.h. R030.
 *
 * Built on manual_log_modal.c (fields, keyboard, Log/Close), with two
 * differences that matter on the RTTY page:
 *
 * ⛔ IT NEVER COVERS THE TX PANE. The 73 press that opens it is still being
 * sent, and STOP must stay in reach while it is (the Type... keyboard's rule,
 * rtty_screen_view.c). Scrim, panel and keyboard all start at x = PANE_W.
 *
 * ⭐ IT CLOSES AFTER LOGGING. One RTTY QSO is one dialog: the next station's
 * call and exchange come from the text, not from typing, so there is nothing
 * to keep open for. */
#include "rtty_log_modal.h"
#include "cw_screen_view.h"   /* CW_PANE_W */
#include "rtty_log.h"
#include "rtty_qso.h"
#include "rtty_tx.h"
#include "rtty_screen_view.h"
#include "cw_callsign.h"
#include "adif/adif_log.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "ui_theme.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "rtty_log_ui";

#define PANE_W   CW_PANE_W
#define AREA_W   (1280 - PANE_W)
#define PANEL_W  (AREA_W - 20)

/* ---- shared ------------------------------------------------------------------ */

/* h = how far down the scrim reaches. The log dialog stops it above the
 * decoded text, so a tap on a word there still reaches the page and fills a
 * field (rtty_log_modal_fill_word). */
static lv_obj_t *mk_scrim(int h)
{
    lv_obj_t *m = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m, AREA_W, h);
    lv_obj_set_pos(m, PANE_W, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);   /* eats taps under the panel */
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

static lv_obj_t *mk_panel(lv_obj_t *scrim, int h)
{
    lv_obj_t *p = lv_obj_create(scrim);
    lv_obj_set_size(p, PANEL_W, h);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 70);   /* the keyboard owns the bottom 280 px */
    lv_obj_set_style_bg_color(p, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 16, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *mk_text(lv_obj_t *parent, const lv_font_t *f, uint32_t col, int x, int y, const char *t)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *parent, const char *txt, uint32_t col, int w, int h,
                           lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

/* On the top layer, not in the scrim: the log dialog's scrim no longer
 * reaches the bottom of the screen. Hidden until a field is tapped; while it
 * is up it covers the text, and Cancel on it uncovers the text again. */
static lv_obj_t *mk_keyboard(lv_event_cb_t cb)
{
    lv_obj_t *kb = lv_keyboard_create(lv_layer_top());
    ui_theme_style_keyboard(kb);
    lv_obj_set_size(kb, AREA_W, 280);
    lv_obj_set_pos(kb, PANE_W, 720 - 280);
    ui_theme_keyboard_attach_caps_cycle_upper(kb);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, cb, LV_EVENT_CANCEL, NULL);
    return kb;
}

static lv_obj_t *s_focus_kb;   /* the keyboard of whichever dialog is open */

static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!s_focus_kb) return;
    lv_keyboard_set_textarea(s_focus_kb, ta);
    ui_osk_show(s_focus_kb);
}

static lv_obj_t *mk_field(lv_obj_t *parent, const char *label, int x, int y, int w, int maxlen,
                          lv_event_cb_t changed)
{
    mk_text(parent, &lv_font_montserrat_22, UI_COLOR_TEXT_SECONDARY, x, y, label);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, 56);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, x, y + 28);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxlen);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(ta);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_CLICKED, NULL);
    if (changed) lv_obj_add_event_cb(ta, changed, LV_EVENT_VALUE_CHANGED, NULL);
    return ta;
}

/* Trim and upper-case: RTTY is upper-case on the air and in the log. */
static void clean(const char *in, char *out, size_t out_sz)
{
    size_t n = 0;
    while (*in && isspace((unsigned char)*in)) in++;
    while (*in && n + 1 < out_sz) out[n++] = (char)toupper((unsigned char)*in++);
    while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    out[n] = 0;
}

/* ---- log confirmation ---------------------------------------------------------- */

static lv_obj_t *s_log_low;   /* scrim below the text box (band strip, bottom bar) */
static lv_obj_t *s_log, *s_log_panel, *s_log_kb, *s_log_hdr, *s_log_note, *s_log_heard;
static lv_obj_t *s_ta_call, *s_ta_rs, *s_ta_rr, *s_ta_xs, *s_ta_xr;
static lv_obj_t *s_lbl_xs, *s_lbl_xr;
static lv_obj_t *s_btn_log, *s_btn_cancel;

static void log_close(void)
{
    if (!s_log) return;
    lv_obj_add_flag(s_log_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_log, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_log_low, LV_OBJ_FLAG_HIDDEN);
}

bool rtty_log_modal_is_open(void)
{
    return s_log && !lv_obj_has_flag(s_log, LV_OBJ_FLAG_HIDDEN);
}

static void log_refresh_enabled(void)
{
    char call[20];
    clean(lv_textarea_get_text(s_ta_call), call, sizeof call);
    if (call[0]) lv_obj_clear_state(s_btn_log, LV_STATE_DISABLED);
    else         lv_obj_add_state(s_btn_log, LV_STATE_DISABLED);
}

static void log_changed_cb(lv_event_t *e) { (void)e; log_refresh_enabled(); }

static void log_do_cb(lv_event_t *e)
{
    (void)e;
    char call[20], rs[8], rr[8], xs[16], xr[16], err[80];
    clean(lv_textarea_get_text(s_ta_call), call, sizeof call);
    clean(lv_textarea_get_text(s_ta_rs), rs, sizeof rs);
    clean(lv_textarea_get_text(s_ta_rr), rr, sizeof rr);
    clean(lv_textarea_get_text(s_ta_xs), xs, sizeof xs);
    clean(lv_textarea_get_text(s_ta_xr), xr, sizeof xr);
    if (!call[0]) return;
    if (!rtty_log_record(call, rs, rr, xs, xr, err, sizeof err)) {
        /* Keep everything in the boxes: this is the only copy of the QSO. */
        lv_label_set_text(s_log_note, err);
        lv_obj_set_style_text_color(s_log_note, lv_color_hex(UI_COLOR_DANGER_BORDER), 0);
        return;
    }
    char t[64];
    snprintf(t, sizeof t, "Logged %s - %d in the log", call, adif_log_count());
    ui_toast(t);
    /* The QSO is done: the station and what it sent go, so the next one
     * starts clean. Clearing the call also un-dims nothing by mistake: the
     * pane re-reads it through rtty_tx_call_gen(). */
    rtty_tx_set_call("");
    rtty_qso_reset();
    log_close();
}

static void log_close_cb(lv_event_t *e) { (void)e; log_close(); }

/* A word tapped in the decoded text while the dialog is up. It goes where it
 * belongs, so no field has to be focused first: a report to RST rcvd, a
 * callsign to Callsign, a number (letter-shift mapped) to Exch rcvd. The
 * line under the fields says what went where. */
void rtty_log_modal_fill_word(const char *w)
{
    if (!rtty_log_modal_is_open() || !w || !w[0]) return;
    char v[16], note[64];
    lv_obj_t *ta = NULL;
    const char *what = "";
    rtty_word_t k = rtty_qso_word(w, v, sizeof v);
    if (k == RTTY_WORD_RST) {
        ta = s_ta_rr; what = "RST rcvd";
    } else if (cw_call_structure_ok(w)) {
        snprintf(v, sizeof v, "%s", w); ta = s_ta_call; what = "Callsign";
    } else if (k == RTTY_WORD_EXCH && !lv_obj_has_flag(s_ta_xr, LV_OBJ_FLAG_HIDDEN)) {
        ta = s_ta_xr; what = "Exch rcvd";
    }
    if (!ta) {
        snprintf(note, sizeof note, "'%s' is not a call, report or exchange", w);
    } else {
        lv_textarea_set_text(ta, v);
        snprintf(note, sizeof note, "%s <- %s", what, v);
        log_refresh_enabled();
    }
    lv_label_set_text(s_log_note, note);
    lv_obj_set_style_text_color(s_log_note, lv_color_hex(ta ? 0x5FD4FF : UI_COLOR_TEXT_SECONDARY), 0);
    ESP_LOGI(TAG, "tap-fill '%s': %s", w, note);
}

static void log_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (!lv_obj_has_state(s_btn_log, LV_STATE_DISABLED)) log_do_cb(NULL);
    } else {
        lv_obj_add_flag(s_log_kb, LV_OBJ_FLAG_HIDDEN);   /* Cancel hides the keys only */
    }
}

static void log_build(void)
{
    if (s_log) return;
    s_log = mk_scrim(rtty_screen_view_text_top_y());
    s_log_low = mk_scrim(720 - rtty_screen_view_text_bottom_y());
    lv_obj_set_y(s_log_low, rtty_screen_view_text_bottom_y());
    s_log_panel = mk_panel(s_log, 330);
    mk_text(s_log_panel, &lv_font_montserrat_28, UI_COLOR_TEXT, 0, 0, "Log this QSO?");
    s_log_hdr = mk_text(s_log_panel, &lv_font_montserrat_22, UI_COLOR_TEXT_SECONDARY, 0, 0, "");
    lv_obj_align(s_log_hdr, LV_ALIGN_TOP_RIGHT, 0, 6);

    /* Row 1: Call | RST sent | RST rcvd. Row 2 (contest): Exch sent | Exch rcvd. */
    s_ta_call = mk_field(s_log_panel, "Callsign", 0,   44, 300, 16, log_changed_cb);
    s_ta_rs   = mk_field(s_log_panel, "RST sent", 320, 44, 150, 4, NULL);
    s_ta_rr   = mk_field(s_log_panel, "RST rcvd", 490, 44, 150, 4, NULL);
    s_ta_xs   = mk_field(s_log_panel, "Exch sent", 0,   138, 300, 12, NULL);
    s_ta_xr   = mk_field(s_log_panel, "Exch rcvd", 320, 138, 320, 12, NULL);
    /* the two exchange labels, to hide outside contest mode */
    s_lbl_xs = lv_obj_get_child(s_log_panel, lv_obj_get_index(s_ta_xs) - 1);
    s_lbl_xr = lv_obj_get_child(s_log_panel, lv_obj_get_index(s_ta_xr) - 1);

    /* What the decoder heard, and where from - the operator's check. */
    s_log_heard = mk_text(s_log_panel, &lv_font_montserrat_22, 0x5FD4FF, 660, 72, "");
    lv_label_set_long_mode(s_log_heard, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_log_heard, PANEL_W - 32 - 660);

    s_log_note = mk_text(s_log_panel, &lv_font_montserrat_22, UI_COLOR_TEXT_SECONDARY, 0, 254, "");
    lv_label_set_long_mode(s_log_note, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_log_note, 500);

    s_btn_log = mk_button(s_log_panel, "Log", UI_COLOR_SUCCESS, 180, 64, log_do_cb, NULL);
    lv_obj_align(s_btn_log, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_btn_log, lv_color_hex(UI_COLOR_BT_OFF), LV_STATE_DISABLED);
    s_btn_cancel = mk_button(s_log_panel, "Cancel", UI_COLOR_DANGER, 180, 64, log_close_cb, NULL);
    lv_obj_align(s_btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -196, 0);

    s_log_kb = mk_keyboard(log_kb_cb);
}

void rtty_log_modal_open(void)
{
    log_build();
    s_focus_kb = s_log_kb;
    settings_rtty_contest_t ct;
    settings_get_rtty_contest(&ct);

    char call[16], my[16], rst[8] = "", ex[16] = "";
    bool me = false;
    rtty_tx_get_call(call, sizeof call);
    settings_get_my_callsign(my, sizeof my);
    bool heard = rtty_qso_get(my, rst, sizeof rst, ex, sizeof ex, &me);

    lv_textarea_set_text(s_ta_call, call);
    lv_textarea_set_text(s_ta_rs, "599");
    /* RST rcvd only when heard: an empty field is "not exchanged", which is
     * honest; a filled-in 599 nobody sent is not. */
    lv_textarea_set_text(s_ta_rr, heard ? rst : "");

    char sent[16];
    rtty_log_sent_exch(sent, sizeof sent);
    lv_textarea_set_text(s_ta_xs, ct.on ? sent : "");
    lv_textarea_set_text(s_ta_xr, ct.on && heard ? ex : "");
    lv_obj_t *xo[4] = { s_ta_xs, s_ta_xr, s_lbl_xs, s_lbl_xr };
    for (int i = 0; i < 4; i++) {
        if (ct.on) lv_obj_clear_flag(xo[i], LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(xo[i], LV_OBJ_FLAG_HIDDEN);
    }

    char h[96];
    if (heard) snprintf(h, sizeof h, "Heard: %s %s\n%s", rst, ct.on ? ex : "",
                        me ? "after your call" : "check it is theirs");
    else       snprintf(h, sizeof h, "Nothing heard -\ntype what they sent");
    lv_label_set_text(s_log_heard, h);

    uint32_t hz = cat_get_frequency();
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char b[96];
    snprintf(b, sizeof b, "%.3f MHz  RTTY  %02d:%02dZ%s%s", hz / 1e6, tm.tm_hour, tm.tm_min,
             ct.on && ct.name[0] ? "  " : "", ct.on ? ct.name : "");
    lv_label_set_text(s_log_hdr, b);

    bool dupe = rtty_log_is_dupe(call, hz);
    lv_label_set_text(s_log_note, dupe ? "DUPE: logged on this band, this contest"
                                       : "Tap a word in the text to fill its field");
    lv_obj_set_style_text_color(s_log_note, lv_color_hex(dupe ? 0xFF5050 : UI_COLOR_TEXT_SECONDARY), 0);

    log_refresh_enabled();
    /* Enter = Log, Esc = Cancel on a physical keyboard. On every open: the
     * contest dialog registers its own, and neither is ever deleted. */
    ui_kbd_set_buttons(s_btn_log, s_btn_cancel);
    lv_obj_clear_flag(s_log, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_log_low, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_log);
    lv_obj_move_foreground(s_log_low);
    ESP_LOGI(TAG, "confirm: %s rst %s exch '%s'%s%s", call, heard ? rst : "-", ex,
             me ? " (after our call)" : "", dupe ? " DUPE" : "");
    /* No pre-focus and no keyboard until a field is tapped (ui_theme.h). */
}

void rtty_log_modal_close_log(void)
{
    if (rtty_log_modal_is_open()) ESP_LOGI(TAG, "confirm closed: the station changed elsewhere");
    log_close();
}

void rtty_log_modal_close(void)
{
    log_close();
    rtty_contest_modal_close();
}

/* ---- contest settings ------------------------------------------------------------ */

static lv_obj_t *s_ct, *s_ct_kb, *s_ct_btn_on, *s_ct_lbl_on, *s_ct_close;
static lv_obj_t *s_ta_name, *s_ta_next, *s_ta_exch;
static bool      s_ct_on;

static void ct_style_on(void)
{
    lv_obj_set_style_bg_color(s_ct_btn_on, lv_color_hex(s_ct_on ? 0x2d5a3f : 0x2a2f37), 0);
    lv_label_set_text(s_ct_lbl_on, s_ct_on ? "Contest mode: ON" : "Contest mode: off");
}

static void ct_toggle_cb(lv_event_t *e) { (void)e; s_ct_on = !s_ct_on; ct_style_on(); }

/* Close = save. The settings are small and every field is visible, so there
 * is no separate Save to forget. */
static void ct_close_save(void)
{
    if (!s_ct || lv_obj_has_flag(s_ct, LV_OBJ_FLAG_HIDDEN)) return;
    settings_rtty_contest_t c;
    settings_get_rtty_contest(&c);
    char name[24], exch[12], next[8];
    clean(lv_textarea_get_text(s_ta_name), name, sizeof name);
    clean(lv_textarea_get_text(s_ta_exch), exch, sizeof exch);
    clean(lv_textarea_get_text(s_ta_next), next, sizeof next);
    int n = atoi(next);
    bool changed = (c.on != s_ct_on) || strcmp(c.name, name);
    c.on = s_ct_on;
    snprintf(c.name, sizeof c.name, "%s", name);
    snprintf(c.exch, sizeof c.exch, "%s", exch);
    if (n >= 1 && n <= 9999) c.serial_next = (uint16_t)n;
    settings_set_rtty_contest(&c);
    if (changed) rtty_log_contest_changed();
    ESP_LOGI(TAG, "contest: %s '%s' next %u exch '%s'", c.on ? "ON" : "off", c.name,
             (unsigned)c.serial_next, c.exch);
    lv_obj_add_flag(s_ct_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ct, LV_OBJ_FLAG_HIDDEN);
}

void rtty_contest_modal_close(void) { ct_close_save(); }

static void ct_close_cb(lv_event_t *e) { (void)e; ct_close_save(); }

static void ct_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) ct_close_save();
    else lv_obj_add_flag(s_ct_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ct_build(void)
{
    if (s_ct) return;
    s_ct = mk_scrim(720);
    lv_obj_t *p = mk_panel(s_ct, 300);
    mk_text(p, &lv_font_montserrat_28, UI_COLOR_TEXT, 0, 0, "RTTY contest");
    s_ct_btn_on = mk_button(p, "", 0x2a2f37, 300, 56, ct_toggle_cb, &s_ct_lbl_on);
    lv_obj_align(s_ct_btn_on, LV_ALIGN_TOP_RIGHT, 0, -4);
    s_ta_name = mk_field(p, "Contest name (log CONTEST_ID)", 0, 50, 420, 23, NULL);
    s_ta_next = mk_field(p, "Next serial", 440, 50, 160, 4, NULL);
    s_ta_exch = mk_field(p, "Sent exch", 620, 50, PANEL_W - 32 - 620, 11, NULL);
    lv_textarea_set_placeholder_text(s_ta_name, "WAE-DX-RTTY");
    lv_textarea_set_placeholder_text(s_ta_exch, "serial");
    lv_textarea_set_accepted_chars(s_ta_next, "0123456789");
    mk_text(p, &lv_font_montserrat_22, UI_COLOR_TEXT_SECONDARY, 0, 150,
            "ON: CQ TEST, short Exch (599 + serial), TU. The serial moves on when\n"
            "a QSO is logged. Dupes are this contest's QSOs on the band.");
    s_ct_close = mk_button(p, "Close", UI_COLOR_PRIMARY, 180, 64, ct_close_cb, NULL);
    lv_obj_align(s_ct_close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    s_ct_kb = mk_keyboard(ct_kb_cb);
}

void rtty_contest_modal_open(void)
{
    ct_build();
    s_focus_kb = s_ct_kb;
    settings_rtty_contest_t c;
    settings_get_rtty_contest(&c);
    s_ct_on = c.on;
    ct_style_on();
    char n[8];
    snprintf(n, sizeof n, "%u", (unsigned)c.serial_next);
    lv_textarea_set_text(s_ta_name, c.name);
    lv_textarea_set_text(s_ta_next, n);
    lv_textarea_set_text(s_ta_exch, c.exch);
    ui_kbd_set_buttons(s_ct_close, s_ct_close);   /* Enter and Esc both save + close */
    lv_obj_clear_flag(s_ct, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ct);
}
