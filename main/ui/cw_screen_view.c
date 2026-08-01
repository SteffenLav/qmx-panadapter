// CW page — Tier 1 (see docs/cw-page-design.md and cw_screen_view.h).
//
// Layout (shack density; POTA density is a Tier 2 follow-up):
//   - Left pane (LEFT pane width, full middle-band height): six memory
//     buttons (tap = send via KY, long-press = edit), a WPM stepper (KS),
//     a Park button ({PARK} macro), and a TX/decode status line.
//   - Decode ticker: a strip across the bottom of the waterfall showing the
//     QMX decoder's output (TB;), fed by cw_screen_feed_decoded().
//   The panadapter spectrum/waterfall stay LIVE underneath — the container
//   itself is transparent and non-clickable so spectrum taps pass through.
//
// Threading: TB text arrives on the CDC-ACM RX callback context; it lands in
// a spinlock-guarded ring here and an LVGL timer renders it (the ft8_status
// pattern). All CAT writes go through cat.c's poll task (the standing rule).
//
// ⚠ HARDWARE-UNVERIFIED: KY/KS/TB are implemented from the CAT manuals
// (1_03 + 1_04 agree) but no QMX has been on the bench since this was
// written. First-session checklist is at the bottom of cw-page-design.md.
// Notably there is NO abort button yet: the manual documents no KY abort in
// TS480-compat-OFF mode, and guessing (RX;? mode bounce?) risks worse than
// letting an 80-char buffer finish. Resolve on hardware first.

#include "cw_screen_view.h"
#include "ui_theme.h"
#include "ui.h"
#include "settings.h"
#include "cat.h"

#include <string.h>
#include <strings.h>   // strncasecmp
#include <ctype.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cw_screen";

// Same middle-band framing as ft8_screen_view.c — keep in step.
#define TOP_BAR_H     60
#define BOTTOM_BAR_H  36
#define MID_Y         TOP_BAR_H
#define MID_H         (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W         1280
// Same width as the FT8 page's left pane (LEFT_W in ft8_screen_view.c) so the
// two pages read as the same grid - operator guidance 2026-08-01: reuse the
// composition, not just the code ("I already know this page").
#define PANE_W        320
#define TICKER_H      100
// The band-plan strip (BANDPLAN_H in ui.c) sits directly above the bottom
// bar and MUST stay visible and touchable on the CW page - it is how the
// operator sees they are inside the CW segment and drags along it. The pane
// and ticker therefore stop above it (hardware-observed 2026-08-01: the
// first cut covered it completely).
#define BANDPLAN_H    22

#define N_CW_MEM      6
#define CW_WPM_MIN    5
#define CW_WPM_MAX    40
#define CW_WPM_FALLBACK 18   // stepper base before the radio has answered KS;

// KY's transmit buffer is 80 chars; the expanded message is capped to fit.
#define CW_SEND_MAX   80

static lv_obj_t *s_container = NULL;
static lv_obj_t *s_pane      = NULL;
static lv_obj_t *s_ticker    = NULL;
static lv_obj_t *s_ticker_lbl = NULL;
static lv_obj_t *s_btn_mem[N_CW_MEM] = {0};
static lv_obj_t *s_lbl_mem[N_CW_MEM] = {0};
static lv_obj_t *s_lbl_wpm   = NULL;
static lv_obj_t *s_lbl_park  = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_timer_t *s_timer   = NULL;
static bool      s_shown     = false;

// ---- decode ring (written from the CDC RX context, read by the timer) ------
#define DEC_RING_CAP 512
static char           s_dec_ring[DEC_RING_CAP];
static size_t         s_dec_len = 0;          // chars currently held (<= CAP)
static bool           s_dec_dirty = false;
static portMUX_TYPE   s_dec_mux = portMUX_INITIALIZER_UNLOCKED;

void cw_screen_feed_decoded(const char *s, int n)
{
    if (!s || n <= 0) return;
    portENTER_CRITICAL(&s_dec_mux);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r' || c == '\n') c = ' ';
        if (s_dec_len == DEC_RING_CAP) {
            // Drop the oldest quarter in one memmove so appends stay O(1)-ish.
            memmove(s_dec_ring, s_dec_ring + DEC_RING_CAP / 4,
                    DEC_RING_CAP - DEC_RING_CAP / 4);
            s_dec_len = DEC_RING_CAP - DEC_RING_CAP / 4;
        }
        s_dec_ring[s_dec_len++] = c;
    }
    s_dec_dirty = true;
    portEXIT_CRITICAL(&s_dec_mux);
}

// ---- macro expansion --------------------------------------------------------
// {MYCALL} and {PARK} expand from settings; everything upper-cased (CW is
// caseless and the QMX keyer expects plain text). Returns false when a macro
// the message uses has no value yet (empty callsign / park).
static bool expand_macros(const char *in, char *out, size_t out_sz,
                          const char **missing)
{
    qmx_settings_t s;
    settings_load_all(&s);
    size_t o = 0;
    *missing = NULL;
    for (const char *p = in; *p && o + 1 < out_sz; ) {
        const char *val = NULL;
        if (strncasecmp(p, "{MYCALL}", 8) == 0) {
            if (!s.my_callsign[0]) { *missing = "callsign (Identity)"; return false; }
            val = s.my_callsign; p += 8;
        } else if (strncasecmp(p, "{PARK}", 6) == 0) {
            if (!s.cw_park[0]) { *missing = "park (tap Park)"; return false; }
            val = s.cw_park; p += 6;
        }
        if (val) {
            while (*val && o + 1 < out_sz) out[o++] = (char)toupper((unsigned char)*val++);
        } else {
            out[o++] = (char)toupper((unsigned char)*p++);
        }
    }
    out[o] = '\0';
    return true;
}

// Button label: the expanded preview when macros resolve, the raw text
// otherwise — either way the operator sees what a tap will actually send.
static void refresh_mem_labels(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    for (int i = 0; i < N_CW_MEM; i++) {
        if (!s_lbl_mem[i]) continue;
        char exp[CW_SEND_MAX + 1];
        const char *missing = NULL;
        const char *txt = s.cw_msg[i][0] ? s.cw_msg[i] : "(empty)";
        if (s.cw_msg[i][0] && expand_macros(s.cw_msg[i], exp, sizeof(exp), &missing))
            txt = exp;
        lv_label_set_text(s_lbl_mem[i], txt);
    }
    // ASCII fallback only - an em-dash is outside the compiled montserrat
    // glyph range and renders as a tofu box (hardware-observed 2026-08-01).
    if (s_lbl_park)
        lv_label_set_text_fmt(s_lbl_park, "Park: %s",
                              s.cw_park[0] ? s.cw_park : "--");
}

// ---- single-field editor modal ---------------------------------------------
// Edits one memory slot or the park reference at a time — one big textarea
// plus the on-screen keyboard, so it stays finger-friendly (six stacked
// fields plus a keyboard do not fit 720 px).
static lv_obj_t *s_edit_modal = NULL;
static lv_obj_t *s_edit_ta    = NULL;
static lv_obj_t *s_edit_title = NULL;
static lv_obj_t *s_edit_kb    = NULL;
static int       s_edit_slot  = -1;    // 0..5 = memory, -1 = park

static void edit_close(void)
{
    if (s_edit_modal) lv_obj_add_flag(s_edit_modal, LV_OBJ_FLAG_HIDDEN);
}

static void edit_save_cb(lv_event_t *e)
{
    (void)e;
    const char *raw = s_edit_ta ? lv_textarea_get_text(s_edit_ta) : NULL;
    char buf[44] = {0};
    if (raw) strncpy(buf, raw, sizeof(buf) - 1);
    for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (s_edit_slot >= 0) settings_set_cw_msg((uint8_t)s_edit_slot, buf);
    else                  settings_set_cw_park(buf);
    ESP_LOGI(TAG, "saved %s: '%s'",
             s_edit_slot >= 0 ? "memory" : "park", buf);
    edit_close();
    refresh_mem_labels();
}

static void edit_cancel_cb(lv_event_t *e) { (void)e; edit_close(); }

static void edit_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
}

static void edit_ta_focused_cb(lv_event_t *e)
{
    (void)e;
    if (!s_edit_kb) return;
    lv_keyboard_set_textarea(s_edit_kb, s_edit_ta);
    lv_obj_clear_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_edit_kb);
}

static void edit_build(void)
{
    if (s_edit_modal) return;
    lv_obj_t *scr = lv_screen_active();

    s_edit_modal = lv_obj_create(scr);
    lv_obj_set_size(s_edit_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_edit_modal, 0, 0);
    lv_obj_set_style_bg_color(s_edit_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_edit_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_edit_modal, 0, 0);
    lv_obj_set_style_pad_all(s_edit_modal, 0, 0);
    lv_obj_clear_flag(s_edit_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_edit_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(s_edit_modal);
    lv_obj_set_size(panel, 1040, 240);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_edit_title = lv_label_create(panel);
    lv_label_set_text(s_edit_title, "CW memory");
    lv_obj_set_style_text_color(s_edit_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_edit_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_edit_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "{MYCALL} and {PARK} are filled in when sent");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, 0, 8);

    s_edit_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_edit_ta, 660, 60);
    lv_obj_align(s_edit_ta, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_textarea_set_one_line(s_edit_ta, true);
    lv_textarea_set_max_length(s_edit_ta, 43);
    lv_obj_set_style_text_font(s_edit_ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_edit_ta);
    lv_obj_add_event_cb(s_edit_ta, edit_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *btn_cancel = lv_btn_create(panel);
    lv_obj_set_size(btn_cancel, 150, 60);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -170, 54);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_add_event_cb(btn_cancel, edit_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_color(lc, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24, 0);
    lv_obj_center(lc);

    lv_obj_t *btn_save = lv_btn_create(panel);
    lv_obj_set_size(btn_save, 150, 60);
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, 0, 54);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(btn_save, 8, 0);
    lv_obj_add_event_cb(btn_save, edit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(btn_save);
    lv_label_set_text(ls, "Save");
    lv_obj_set_style_text_color(ls, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ls, &lv_font_montserrat_24, 0);
    lv_obj_center(ls);
    ui_kbd_set_buttons(btn_save, btn_cancel);

    s_edit_kb = lv_keyboard_create(s_edit_modal);
    ui_theme_style_keyboard(s_edit_kb);
    lv_obj_set_size(s_edit_kb, LV_PCT(100), 280);
    lv_obj_align(s_edit_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_theme_keyboard_attach_caps_cycle_upper(s_edit_kb);
    lv_obj_set_style_text_font(s_edit_kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_edit_kb, edit_kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_edit_kb, edit_kb_event_cb, LV_EVENT_CANCEL, NULL);
}

static void edit_open(int slot)
{
    edit_build();
    s_edit_slot = slot;
    qmx_settings_t s;
    settings_load_all(&s);
    if (slot >= 0) {
        lv_label_set_text_fmt(s_edit_title, "CW memory %d", slot + 1);
        lv_textarea_set_text(s_edit_ta, s.cw_msg[slot]);
    } else {
        lv_label_set_text(s_edit_title, "POTA park reference");
        lv_textarea_set_text(s_edit_ta, s.cw_park);
    }
    lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_edit_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_edit_modal);
    ui_theme_focus_textarea(s_edit_ta);
}

// ---- memory buttons ---------------------------------------------------------
// Tap = expand + send; long-press = edit. LVGL delivers CLICKED after a long
// press too, so the press duration decides which action the CLICKED handler
// takes (the User-Manual button pattern in ui.c).
static uint32_t s_mem_press_tick = 0;

static void mem_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_mem_press_tick = lv_tick_get();
}

static void mem_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= N_CW_MEM) return;
    if (s_mem_press_tick && lv_tick_elaps(s_mem_press_tick) >= 600) {
        edit_open(idx);
        return;
    }
    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.cw_msg[idx][0]) { edit_open(idx); return; }   // empty slot: edit it

    char exp[CW_SEND_MAX + 1];
    const char *missing = NULL;
    if (!expand_macros(s.cw_msg[idx], exp, sizeof(exp), &missing)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Set your %s first", missing ? missing : "identity");
        ui_toast(msg);
        return;
    }
    if (!cat_is_ready()) { ui_toast("No QMX connected"); return; }
    cat_request_cw_send(exp);
    ESP_LOGI(TAG, "M%d send: '%s'", idx + 1, exp);
    if (s_lbl_status) {
        lv_label_set_text_fmt(s_lbl_status, "Keying:\n%s", exp);
        lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_RED), 0);
    }
}

static void park_clicked_cb(lv_event_t *e) { (void)e; edit_open(-1); }

// ---- WPM stepper -------------------------------------------------------------
static void wpm_step(int delta)
{
    int cur = cat_get_keyer_wpm();
    if (cur <= 0) cur = CW_WPM_FALLBACK;
    cur += delta;
    if (cur < CW_WPM_MIN) cur = CW_WPM_MIN;
    if (cur > CW_WPM_MAX) cur = CW_WPM_MAX;
    cat_request_keyer_wpm((uint8_t)cur);
    if (s_lbl_wpm) lv_label_set_text_fmt(s_lbl_wpm, "%d WPM", cur);
}

static void wpm_minus_cb(lv_event_t *e) { (void)e; wpm_step(-1); }
static void wpm_plus_cb(lv_event_t *e)  { (void)e; wpm_step(+1); }

// ---- 500 ms refresh: ticker text, WPM readback, TX status -------------------
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_shown) return;

    bool dirty;
    char snap[DEC_RING_CAP + 1];
    size_t n;
    portENTER_CRITICAL(&s_dec_mux);
    dirty = s_dec_dirty;
    s_dec_dirty = false;
    n = s_dec_len;
    if (dirty) memcpy(snap, s_dec_ring, n);
    portEXIT_CRITICAL(&s_dec_mux);
    if (dirty && s_ticker_lbl) {
        snap[n] = '\0';
        // Show the tail that fits: the label wraps ~3 lines; ~180 chars.
        const char *tail = (n > 180) ? snap + (n - 180) : snap;
        lv_label_set_text(s_ticker_lbl, tail);
    }

    int wpm = cat_get_keyer_wpm();
    if (wpm > 0 && s_lbl_wpm) lv_label_set_text_fmt(s_lbl_wpm, "%d WPM", wpm);

    int rem = cat_get_cw_tx_remaining();
    if (s_lbl_status) {
        if (!cat_is_ready()) {
            lv_label_set_text(s_lbl_status, "No QMX connected");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        } else if (rem > 0) {
            lv_label_set_text_fmt(s_lbl_status, "Keying... %d%s chars left",
                                  rem, rem >= 9 ? "+" : "");
            lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_RED), 0);
        } else if (rem == 0) {
            lv_label_set_text(s_lbl_status, "Receiving - decoder on");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        } else {
            lv_label_set_text(s_lbl_status, "Decoder starting...");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        }
    }
}

// ---- build -------------------------------------------------------------------
static lv_obj_t *make_pane_btn(lv_obj_t *parent, int x, int y, int w, int h,
                               uint32_t bg, uint32_t border)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    return b;
}

void cw_screen_view_init(lv_obj_t *parent)
{
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    // Transparent and click-through: the live spectrum/waterfall underneath
    // must keep their tap-to-tune and drag gestures everywhere the pane and
    // ticker don't cover.
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);

    // Left pane (stops above the band-plan strip).
    s_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_pane, PANE_W, MID_H - BANDPLAN_H);
    lv_obj_set_pos(s_pane, 0, 0);
    lv_obj_set_style_bg_color(s_pane, lv_color_hex(0x14181e), 0);
    lv_obj_set_style_bg_opa(s_pane, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pane, 0, 0);
    lv_obj_set_style_pad_all(s_pane, 0, 0);
    lv_obj_clear_flag(s_pane, LV_OBJ_FLAG_SCROLLABLE);

    // Page header - the FT8 pane's "MODE: FT8" gold title, same font and
    // colour, so the two pages read as siblings (operator request 2026-08-01).
    lv_obj_t *hdr = lv_label_create(s_pane);
    lv_label_set_text(hdr, "MODE: CW");
    lv_obj_set_style_text_color(hdr, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(hdr, 14, 6);

    // Six memory buttons, full pane width (pitch tightened vs the first cut
    // so the header fits without sacrificing a memory slot).
    int y = 64;
    for (int i = 0; i < N_CW_MEM; i++) {
        s_btn_mem[i] = make_pane_btn(s_pane, 10, y, PANE_W - 20, 56, 0x2a2f37, 0x555555);
        lv_obj_add_event_cb(s_btn_mem[i], mem_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn_mem[i], mem_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_lbl_mem[i] = lv_label_create(s_btn_mem[i]);
        lv_label_set_text(s_lbl_mem[i], "");
        lv_label_set_long_mode(s_lbl_mem[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_lbl_mem[i], PANE_W - 44);
        lv_obj_set_style_text_color(s_lbl_mem[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(s_lbl_mem[i], &lv_font_montserrat_20, 0);
        lv_obj_center(s_lbl_mem[i]);
        y += 62;
    }

    // WPM stepper row.
    y += 4;
    lv_obj_t *bm = make_pane_btn(s_pane, 10, y, 70, 54, 0x2a2f37, 0x555555);
    lv_obj_add_event_cb(bm, wpm_minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lm = lv_label_create(bm);
    lv_label_set_text(lm, "-");
    lv_obj_set_style_text_color(lm, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lm, &lv_font_montserrat_28, 0);
    lv_obj_center(lm);

    s_lbl_wpm = lv_label_create(s_pane);
    lv_label_set_text(s_lbl_wpm, "-- WPM");
    lv_obj_set_style_text_color(s_lbl_wpm, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_lbl_wpm, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_wpm, 96, y + 14);

    lv_obj_t *bp = make_pane_btn(s_pane, PANE_W - 80, y, 70, 54, 0x2a2f37, 0x555555);
    lv_obj_add_event_cb(bp, wpm_plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lp = lv_label_create(bp);
    lv_label_set_text(lp, "+");
    lv_obj_set_style_text_color(lp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_28, 0);
    lv_obj_center(lp);
    y += 60;

    // Park button.
    lv_obj_t *bpark = make_pane_btn(s_pane, 10, y, PANE_W - 20, 50, 0x1c2128, 0x7a4a10);
    lv_obj_add_event_cb(bpark, park_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_park = lv_label_create(bpark);
    lv_label_set_text(s_lbl_park, "Park: --");
    lv_obj_set_style_text_color(s_lbl_park, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_lbl_park, &lv_font_montserrat_22, 0);
    lv_obj_center(s_lbl_park);
    y += 56;

    // Status line.
    s_lbl_status = lv_label_create(s_pane);
    lv_label_set_text(s_lbl_status, "");
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, PANE_W - 20);
    lv_obj_set_pos(s_lbl_status, 10, y + 4);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);

    // Decode ticker across the bottom, to the right of the pane, ending
    // above the band-plan strip so the strip stays visible and draggable.
    s_ticker = lv_obj_create(s_container);
    lv_obj_set_size(s_ticker, MID_W - PANE_W, TICKER_H);
    lv_obj_set_pos(s_ticker, PANE_W, MID_H - TICKER_H - BANDPLAN_H);
    lv_obj_set_style_bg_color(s_ticker, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_ticker, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_ticker, lv_color_hex(0x2a323c), 0);
    lv_obj_set_style_border_width(s_ticker, 1, 0);
    lv_obj_set_style_radius(s_ticker, 0, 0);
    lv_obj_set_style_pad_all(s_ticker, 8, 0);
    lv_obj_clear_flag(s_ticker, LV_OBJ_FLAG_SCROLLABLE);

    s_ticker_lbl = lv_label_create(s_ticker);
    lv_label_set_text(s_ticker_lbl, "CW decode (from the QMX's own decoder) appears here");
    lv_label_set_long_mode(s_ticker_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ticker_lbl, MID_W - PANE_W - 24);
    lv_obj_set_style_text_color(s_ticker_lbl, lv_color_hex(0xc8d0c8), 0);
    lv_obj_set_style_text_font(s_ticker_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_pos(s_ticker_lbl, 0, 0);

    ESP_LOGI(TAG, "CW screen built");
}

lv_obj_t *cw_screen_view_get_container(void) { return s_container; }

void cw_screen_view_show(void)
{
    if (!s_container) return;
    refresh_mem_labels();
    cat_query_keyer_wpm();
    cat_cw_poll_set_active(true);
    if (!s_timer) s_timer = lv_timer_create(tick_cb, 500, NULL);
    s_shown = true;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    // Deliberately NOT foregrounded: the edge-swipe strips must stay on top
    // (the left-edge swipe is how the operator LEAVES this page), and the
    // container was built after the spectrum/waterfall so it already sits
    // above them in z-order.
}

void cw_screen_view_hide(void)
{
    if (!s_container) return;
    s_shown = false;
    cat_cw_poll_set_active(false);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    edit_close();
}
