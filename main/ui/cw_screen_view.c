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
// ⭐ CW TRANSMIT KEYS THE RADIO - confirmed OFF AIR by the operator on
// 2026-09-30: "the cw is keyed clearly on the antenna - i have tried it and
// it works". That retires the long-standing "KY does not key" finding, which
// was recorded across two bench evenings in 2026-08 and is explained rather
// than contradicted: practice mode suppresses BOTH the sidetone and the LCD
// `M` indicator, so the two observables those evenings relied on were both
// lying. RF on the antenna is the observable that cannot lie. Do NOT
// reinstate an "unverified" caveat on the KY path.
//
// ⚠ Still unresolved, and NOT covered by that: there is NO abort button. The
// manual documents no KY abort in TS480-compat-OFF mode, and guessing (RX;?
// mode bounce?) risks worse than letting an 80-char buffer finish. That is
// the next hardware question, not a code one.

#include "cw_screen_view.h"
#include "ui_theme.h"
#include "ui.h"
#include "settings.h"
#include "cat.h"
#include "cw_qso.h"
#include "cw_decode.h"   // cw_squelch_push() - the SHARED noise filter
#include "util/dxcc.h"
#include "adif/adif_log.h"

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
#define PANE_W        CW_PANE_W   // exported in cw_screen_view.h - ui.c needs it too
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
static lv_obj_t *s_lbl_hint  = NULL;   // copilot: plain-language read of the decode
static lv_obj_t *s_btn_mem[N_CW_MEM] = {0};
static lv_obj_t *s_lbl_mem[N_CW_MEM] = {0};
static lv_obj_t *s_lbl_wpm   = NULL;
static lv_obj_t *s_lbl_park  = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_timer_t *s_timer   = NULL;
static bool      s_shown     = false;

// ESM NEXT button (Tier 2): the one-tap QSO ladder. Header line names the
// state, body shows exactly what a tap will key. Long-press resets to CQ.
static lv_obj_t *s_btn_next     = NULL;
static lv_obj_t *s_lbl_next_hdr = NULL;
static lv_obj_t *s_lbl_next     = NULL;

// Station card: the captured/adopted callsign, enriched (country from the
// dxcc tables, worked-before from the ADIF cache). Tap = adopt (pounce);
// tap again while adopted = drop.
static lv_obj_t *s_card       = NULL;
static lv_obj_t *s_card_call  = NULL;
static lv_obj_t *s_card_info  = NULL;
static lv_obj_t *s_card_hint  = NULL;

// ---- decode ring (written from the CDC RX context, read by the timer) ------
#define DEC_RING_CAP 512
static char           s_dec_ring[DEC_RING_CAP];
static size_t         s_dec_len = 0;          // chars currently held (<= CAP)
static bool           s_dec_dirty = false;
/* ⛔ ONE SQUELCH, NOT TWO. This file used to carry its own noise filter, which
 * keyed on repeated single letters and knew nothing about the decoder's
 * unresolved-symbol marker - so the panadapter's CW line (cw_decode.c) dropped
 * '*' and the CW page's ticker printed it, from the same TB; bytes. Operator,
 * 2026-09-11: "the cw decoder needs the same filtering as we implemented on
 * the cw decoding on the panadapter - no * for example".
 *
 * cw_squelch_push() is portable and host-tested, it drops '*' AND counts it
 * toward the E/T run (so "T*T*E*T" reads as one stretch of the decoder
 * struggling rather than as text interrupted by noise), and it is the same
 * code the other screen uses - which is the point. Two filters over one stream
 * is two answers to one question. */
static cw_squelch_t   s_tick_sq;
static portMUX_TYPE   s_dec_mux = portMUX_INITIALIZER_UNLOCKED;

// Wipe everything the decode stream produced: the ticker's ring, the copilot
// hint, the captured/adopted station and the ESM ladder. Called when
// simulation mode is switched off so the phantom's QSO does not linger on
// screen looking like real copy (operator-reported 2026-08-04) - the same
// contract FT8 has, where ft8_op_mode_set() clears the decode list on a sim
// exit. Safe from the LVGL thread only (touches labels).
void cw_screen_clear_decode(void)
{
    portENTER_CRITICAL(&s_dec_mux);
    s_dec_len = 0;
    s_dec_dirty = false;
    portEXIT_CRITICAL(&s_dec_mux);
    memset(&s_tick_sq, 0, sizeof(s_tick_sq));   // drop whatever it was holding
    cw_qso_reset();          // drops target + ladder
    cw_qso_clear_capture();  // drops the heard call and the hint
    if (s_ticker_lbl) lv_label_set_text(s_ticker_lbl, "");
    if (s_lbl_hint)   lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_card)       lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

// (The bespoke ticker squelch that used to be described here is gone - the
// ticker now shares cw_decode.c's, which additionally drops the decoder's '*'
// unresolved-symbol marker. See the note by s_tick_sq. The important property
// is unchanged: the filter applies ONLY to the display string, because
// cw_qso_feed() still gets the raw stream, so callsign capture, the copilot
// hints and the ESM ladder cannot lose a character to it.)

// Append one character. Short critical section - see the cyan-flash note.
static void ticker_emit_char(char c)
{
    portENTER_CRITICAL(&s_dec_mux);
    if (s_dec_len == DEC_RING_CAP) {
        memmove(s_dec_ring, s_dec_ring + DEC_RING_CAP / 4,
                DEC_RING_CAP - DEC_RING_CAP / 4);
        s_dec_len = DEC_RING_CAP - DEC_RING_CAP / 4;
    }
    s_dec_ring[s_dec_len++] = c;
    s_dec_dirty = true;
    portEXIT_CRITICAL(&s_dec_mux);
}

void cw_screen_feed_decoded(const char *s, int n)
{
    if (!s || n <= 0) return;
    cw_qso_feed(s, n);   // RAW stream: capture / ESM / hints are never filtered
    // ⚠ Raw is right for cw_qso: its tokenizer already treats anything outside
    // [A-Z0-9/-] as a token separator, so '*' splits tokens there instead of
    // corrupting them, and the capture wants every character it can get.
    //
    // The TICKER is what the operator reads, and it goes through the same
    // squelch as the panadapter's CW line - see the note by s_tick_sq.
    // Characters reach it AS THEY DECODE apart from the few the squelch is
    // holding to decide on, which is what an operator following a QSO needs
    // (operator, 2026-08-04).
    for (int i = 0; i < n; i++) {
        char rel[CW_SQUELCH_HOLD + 2];
        int m = cw_squelch_push(&s_tick_sq, s[i], rel, sizeof(rel));
        for (int k = 0; k < m; k++) ticker_emit_char(rel[k]);
    }
}

/* ---- where {PARK} comes from ------------------------------------------------
 *
 * TWO FIELDS USED TO MEAN THE SAME FACT and never spoke to each other:
 * cw_park, typed on this page and kept for ever, and the activation
 * (act_type + act_ref) that the Activation window sets and that the ADIF
 * writer already stamps on every QSO as MY_SIG / MY_<PROG>_REF. Start a POTA
 * activation at OZ-0001 and this page still read "Park: --", with {PARK}
 * refusing to expand. Operator, 2026-10-31: "Parks: -- will it change
 * according to what was activated?"
 *
 * ⭐ THE RUNNING ACTIVATION WINS, and that is the arguable half of this.
 * cw_park has no lifecycle - it is whatever was typed last, weeks ago - while
 * an activation is a deliberate statement of where the operator is RIGHT NOW,
 * and the log is already being written with it. Keying a stale reference is
 * the worse failure of the two. cw_park is what answers when nothing is being
 * activated, which is the case it was written for.
 *
 * Narrow accessors only. This is called from the 500 ms tick and from every
 * memory-label refresh; settings_load_all() here would be a 3.9 kB local on
 * the LVGL stack several times a second.
 *
 * out gets the reference; prog gets the programme word ("POTA", "SOTA", ...)
 * when it came from an activation, or NULL when it came from cw_park.
 * Returns false when there is no reference at all. */
static bool park_resolve(char *out, size_t out_sz, const char **prog)
{
    if (prog) *prog = NULL;
    if (!out || out_sz == 0) return false;
    out[0] = '\0';

    if (settings_get_activation_ref(out, out_sz)) {
        if (prog) *prog = settings_activation_sig_name();
        return true;
    }
    settings_get_cw_park(out, out_sz);
    return out[0] != '\0';
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
    char tgt[16];
    char park[16];
    cw_qso_get_target(tgt, sizeof(tgt));
    for (const char *p = in; *p && o + 1 < out_sz; ) {
        const char *val = NULL;
        if (strncasecmp(p, "{MYCALL}", 8) == 0) {
            if (!s.my_callsign[0]) { *missing = "callsign (Identity)"; return false; }
            val = s.my_callsign; p += 8;
        } else if (strncasecmp(p, "{PARK}", 6) == 0) {
            /* The activation's reference if one is running, else cw_park -
             * see park_resolve(). The hint names BOTH ways to set it, because
             * either one fixes a refusal and the operator may well be about
             * to start an activation anyway. */
            if (!park_resolve(park, sizeof(park), NULL)) {
                *missing = "park (tap Park, or start an activation)";
                return false;
            }
            val = park; p += 6;
        } else if (strncasecmp(p, "{CALL}", 6) == 0) {
            if (!tgt[0]) { *missing = "station (tap the card)"; return false; }
            val = tgt; p += 6;
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
    /* ASCII fallback only - an em-dash is outside the compiled montserrat
     * glyph range and renders as a tofu box (hardware-observed 2026-08-01).
     *
     * ⛔ NO "Park: " PREFIX ON THE INHERITED FORM, and the reason is pixels,
     * not taste. The button is PANE_W-20 = 300 px and the label is
     * montserrat_22; act_ref holds up to 15 characters and the longest
     * programme word is WWBOTA, so "Park: DL/BE-001 (WWBOTA)" is about 30
     * characters - past the button at this font. "WWBOTA DL/BE-001" is 16 and
     * fits with room, and it is how these references are written anyway. The
     * pane cannot widen and the font does not shrink. */
    if (s_lbl_park) {
        char ref[16];
        const char *prog = NULL;
        if (!park_resolve(ref, sizeof(ref), &prog)) lv_label_set_text(s_lbl_park, "Park: --");
        else if (prog)  lv_label_set_text_fmt(s_lbl_park, "%s %s", prog, ref);
        else            lv_label_set_text_fmt(s_lbl_park, "Park: %s", ref);
    }
}

// ---- editor modal ------------------------------------------------------------
// ALL SIX memories in one window (operator request 2026-08-02) - long-press
// any memory button opens it with that field focused; Save writes all six.
// Park mode (edit_open(-1)) reuses the same modal with only the first field
// shown. Buttons live in the title row (the P4-style top-right placement
// from v1.3.0) so six fields + the on-screen keyboard fit 720 px.
static lv_obj_t *s_edit_modal = NULL;
static lv_obj_t *s_edit_ta6[N_CW_MEM] = {0};
static lv_obj_t *s_edit_num[N_CW_MEM] = {0};
static lv_obj_t *s_edit_title = NULL;
static lv_obj_t *s_edit_kb    = NULL;

static void refresh_next_label(void);   // fwd (ESM section below)

// Size, placement and caps behaviour for both of this page's keyboards. The
// LOOK is ui_theme_style_keyboard()'s job alone now, keys included.
static void style_edit_keyboard(lv_obj_t *kb)
{
    ui_theme_style_keyboard(kb);
    lv_obj_set_size(kb, LV_PCT(100), 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_theme_keyboard_attach_caps_cycle_upper(kb);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void edit_close(void)
{
    if (s_edit_modal) lv_obj_add_flag(s_edit_modal, LV_OBJ_FLAG_HIDDEN);
}

static void edit_save_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < N_CW_MEM; i++) {
        char buf[44] = {0};
        const char *raw = lv_textarea_get_text(s_edit_ta6[i]);
        if (raw) snprintf(buf, sizeof(buf), "%s", raw);
        for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
        settings_set_cw_msg((uint8_t)i, buf);
    }
    ESP_LOGI(TAG, "saved all 6 CW memories");
    edit_close();
    refresh_mem_labels();
    refresh_next_label();
}

static void edit_cancel_cb(lv_event_t *e) { (void)e; edit_close(); }

static void edit_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
}

/* The field the macro buttons paste into. The keyboard already tracks it, but
 * lv_keyboard_get_textarea() reads back NULL once the keyboard is hidden, and
 * the buttons have to work with it down. */
static lv_obj_t *s_edit_focus = NULL;
static lv_obj_t *s_edit_macro_lbl[3];   /* the three button labels */
static lv_obj_t *s_edit_sends = NULL;   /* the live "sends:" line */

/* What each button PASTES - always the token, never the value. The label
 * shows the value; the memory must keep the macro or it would freeze at
 * whatever was current when it was written. */
static const char *const EDIT_MACROS[3] = { "{MYCALL}", "{PARK}", "{CALL}" };

/* What each button SAYS. The value once it is known, the plain name until
 * then - operator, 2026-11-01: "My call should only say My call until I have
 * filled out the Call sign form. Same with My park."
 *
 * ⛔ THE LABELS ARE REFRESHED ON EVERY OPEN, not built once. The editor
 * window is built lazily and then lives for the boot, so a callsign entered
 * afterwards, an activation started, or a station adopted would all leave
 * these reading the name they had when the window was first created. */
static void edit_macro_labels_refresh(void)
{
    char buf[20];

    if (s_edit_macro_lbl[0]) {
        settings_get_my_callsign(buf, sizeof(buf));
        lv_label_set_text(s_edit_macro_lbl[0], buf[0] ? buf : "My call");
    }
    if (s_edit_macro_lbl[1]) {
        char ref[16];
        lv_label_set_text(s_edit_macro_lbl[1],
                          park_resolve(ref, sizeof(ref), NULL) ? ref : "My park");
    }
    if (s_edit_macro_lbl[2]) {
        char tgt[16];
        cw_qso_get_target(tgt, sizeof(tgt));
        lv_label_set_text(s_edit_macro_lbl[2], tgt[0] ? tgt : "Their call");
    }
}

/* ⭐ WHAT THIS MEMORY WILL ACTUALLY KEY, under the field being edited. The
 * braces were the whole confusion: the pane shows a memory EXPANDED and this
 * window shows it RAW, and nothing said they were the same line. Operator,
 * 2026-11-01: "I dont get the idea here". One line of the finished text
 * explains the mechanism better than any label naming it could. */
static void edit_sends_refresh(void)
{
    if (!s_edit_sends) return;
    if (!s_edit_focus) { lv_label_set_text(s_edit_sends, ""); return; }

    const char *raw = lv_textarea_get_text(s_edit_focus);
    if (!raw || !raw[0]) { lv_label_set_text(s_edit_sends, ""); return; }

    char exp[CW_SEND_MAX + 1];
    const char *missing = NULL;
    if (expand_macros(raw, exp, sizeof(exp), &missing))
        lv_label_set_text_fmt(s_edit_sends, "sends:  %s", exp);
    else
        lv_label_set_text_fmt(s_edit_sends, "needs %s", missing ? missing : "a value");
}

static void edit_ta_changed_cb(lv_event_t *e)
{
    (void)e;
    edit_sends_refresh();
}

/* ⭐ MACROS GO IN WITH ONE TAP. They could always be typed, and that was the
 * whole problem: { and } live on LVGL's symbol layer, so "{CALL}" cost eight
 * taps and two layer switches - 1#, {, abc, C, A, L, L, 1#, }, abc - for six
 * characters, in a window the operator opens once and then avoids. The hint
 * line that used to sit here named the three macros and gave no way to use
 * them. Operator, 2026-11-01: "then what is the idea to make it easy to write
 * the call etc.??"
 *
 * Pasted at the cursor of the focused field, not appended: the call usually
 * belongs at the FRONT ("{CALL} UR 599 599 BK"), which append cannot do. */
static void edit_macro_cb(lv_event_t *e)
{
    const char *macro = (const char *)lv_event_get_user_data(e);
    if (!s_edit_focus || !macro) return;

    /* A TRAILING SPACE, because a macro is never the whole message and the
     * next thing typed would otherwise run into it - "{CALL}UR". Operator,
     * 2026-11-01: "+ a space". */
    char ins[20];
    snprintf(ins, sizeof(ins), "%s ", macro);
    lv_textarea_add_text(s_edit_focus, ins);

    /* ⛔ AND THE CURSOR STAYS IN THE FIELD. Tapping the button moves LVGL's
     * focus to the button, so the textarea loses LV_STATE_FOCUSED and its
     * cursor stops blinking - the field looks inert right after the paste,
     * at the exact moment the operator is about to keep typing. Re-assert
     * the state and keep the keyboard pointed at the same field. Operator,
     * 2026-11-01: "the blinking yellow cursor should stay there after the
     * insert". lv_textarea_add_text() has already moved the cursor past what
     * it inserted, so there is no position to restore. */
    lv_obj_add_state(s_edit_focus, LV_STATE_FOCUSED);
    if (s_edit_kb) {
        lv_keyboard_set_textarea(s_edit_kb, s_edit_focus);
        lv_obj_clear_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_edit_kb);
    }
    edit_sends_refresh();
}

static void edit_ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    s_edit_focus = ta;
    edit_sends_refresh();
    // One blinking cursor at a time, keyboard follows the focused field.
    for (int i = 0; i < N_CW_MEM; i++)
        if (s_edit_ta6[i] && s_edit_ta6[i] != ta)
            lv_obj_remove_state(s_edit_ta6[i], LV_STATE_FOCUSED);
    if (!s_edit_kb) return;
    lv_keyboard_set_textarea(s_edit_kb, ta);
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
    lv_obj_set_size(panel, 1080, 420);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_edit_title = lv_label_create(panel);
    lv_label_set_text(s_edit_title, "CW memories");
    lv_obj_set_style_text_color(s_edit_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_edit_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_edit_title, LV_ALIGN_TOP_LEFT, 0, 4);

    /* The hint line that used to be here is GONE, replaced by the buttons it
     * described - a label naming three macros the operator then had to spell
     * out by hand was the gap, not the information. Tapping one says what it
     * does better than the sentence did. */
    {
        /* ⛔ x=320, NOT 212. "CW memories" at montserrat_28 runs to about
         * x=180, so a row starting at 212 sat on the title's shoulder.
         * 320 is where the hint line these buttons replaced used to start,
         * and it leaves the gap the operator asked for.
         *
         * Fixed widths, not content-sized: the labels swap between a name
         * and a callsign as settings change, and a row that re-flows every
         * time a station is adopted would move the button under the finger
         * that is reaching for it. 140 holds "Their call" and a 9-character
         * callsign at montserrat_20. */
        int mx = 320;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = lv_btn_create(panel);
            lv_obj_set_size(b, 140, 48);
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, mx, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2f37), 0);
            lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_add_event_cb(b, edit_macro_cb, LV_EVENT_CLICKED,
                                (void *)EDIT_MACROS[i]);
            s_edit_macro_lbl[i] = lv_label_create(b);
            /* Text comes from edit_macro_labels_refresh() on every open. */
            lv_label_set_text(s_edit_macro_lbl[i], "");
            /* CW blue, the page's own colour - these insert, they do not
             * commit, so they must not read as Save. */
            lv_obj_set_style_text_color(s_edit_macro_lbl[i],
                                        lv_color_hex(UI_COLOR_MODE_CW_TEXT), 0);
            lv_obj_set_style_text_font(s_edit_macro_lbl[i], &lv_font_montserrat_20, 0);
            lv_obj_center(s_edit_macro_lbl[i]);
            mx += 148;
        }
    }

    lv_obj_t *btn_cancel = lv_btn_create(panel);
    lv_obj_set_size(btn_cancel, 150, 48);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -166, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_add_event_cb(btn_cancel, edit_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_color(lc, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24, 0);
    lv_obj_center(lc);

    lv_obj_t *btn_save = lv_btn_create(panel);
    lv_obj_set_size(btn_save, 150, 48);
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(btn_save, 8, 0);
    lv_obj_add_event_cb(btn_save, edit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(btn_save);
    lv_label_set_text(ls, "Save");
    lv_obj_set_style_text_color(ls, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ls, &lv_font_montserrat_24, 0);
    lv_obj_center(ls);
    ui_kbd_set_buttons(btn_save, btn_cancel);

    for (int i = 0; i < N_CW_MEM; i++) {
        int fy = 60 + i * 54;
        s_edit_num[i] = lv_label_create(panel);
        lv_label_set_text_fmt(s_edit_num[i], "%d", i + 1);
        lv_obj_set_style_text_color(s_edit_num[i], lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(s_edit_num[i], &lv_font_montserrat_24, 0);
        lv_obj_set_pos(s_edit_num[i], 4, fy + 12);

        s_edit_ta6[i] = lv_textarea_create(panel);
        lv_obj_set_size(s_edit_ta6[i], 1010, 48);
        lv_obj_set_pos(s_edit_ta6[i], 34, fy);
        lv_textarea_set_one_line(s_edit_ta6[i], true);
        lv_textarea_set_max_length(s_edit_ta6[i], 43);
        lv_obj_set_style_text_font(s_edit_ta6[i], &lv_font_montserrat_22, 0);
        ui_theme_style_textarea(s_edit_ta6[i]);
        lv_obj_add_event_cb(s_edit_ta6[i], edit_ta_focused_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(s_edit_ta6[i], edit_ta_changed_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* Under the six fields, inside the panel's 420 px: the last field ends
     * at y=378 and the pad leaves room to y=404. */
    s_edit_sends = lv_label_create(panel);
    lv_label_set_text(s_edit_sends, "");
    lv_obj_set_style_text_color(s_edit_sends, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_edit_sends, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(s_edit_sends, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_edit_sends, 1010);
    lv_obj_set_pos(s_edit_sends, 34, 384);

    s_edit_kb = lv_keyboard_create(s_edit_modal);
    style_edit_keyboard(s_edit_kb);
    lv_obj_add_event_cb(s_edit_kb, edit_kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_edit_kb, edit_kb_event_cb, LV_EVENT_CANCEL, NULL);
}

// Open the six-memory editor. NO field is focused on open (six blinking
// cursors read as noise - operator): the first tap on a line focuses it and
// raises the keyboard.
static void edit_open(int slot)
{
    (void)slot;
    edit_build();
    qmx_settings_t s;
    settings_load_all(&s);
    for (int i = 0; i < N_CW_MEM; i++) {
        lv_textarea_set_text(s_edit_ta6[i], s.cw_msg[i]);
        lv_obj_remove_state(s_edit_ta6[i], LV_STATE_FOCUSED);
    }
    /* No field is focused on open, so nothing is a paste target yet - a
     * macro button tapped first would otherwise land in whichever line was
     * focused the last time this window was used. */
    s_edit_focus = NULL;
    edit_macro_labels_refresh();
    edit_sends_refresh();
    lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_edit_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_edit_modal);
}

// ---- park editor -------------------------------------------------------------
// The park is one short field - it gets its own compact window instead of
// the six-line editor with five dead rows (operator, 2026-08-02).
static lv_obj_t *s_park_modal = NULL;
static lv_obj_t *s_park_ta    = NULL;
static lv_obj_t *s_park_note  = NULL;
static lv_obj_t *s_park_kb    = NULL;

static void park_close(void)
{
    if (s_park_modal) lv_obj_add_flag(s_park_modal, LV_OBJ_FLAG_HIDDEN);
}

static void park_save_cb(lv_event_t *e)
{
    (void)e;
    char buf[16] = {0};
    const char *raw = s_park_ta ? lv_textarea_get_text(s_park_ta) : NULL;
    if (raw) snprintf(buf, sizeof(buf), "%s", raw);
    for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    settings_set_cw_park(buf);
    ESP_LOGI(TAG, "saved park: '%s'", buf);
    park_close();
    refresh_mem_labels();
    refresh_next_label();
}

static void park_cancel_cb(lv_event_t *e) { (void)e; park_close(); }

static void park_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_park_kb, LV_OBJ_FLAG_HIDDEN);
}

static void park_ta_focused_cb(lv_event_t *e)
{
    (void)e;
    if (!s_park_kb) return;
    lv_keyboard_set_textarea(s_park_kb, s_park_ta);
    lv_obj_clear_flag(s_park_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_park_kb);
}

static void park_build(void)
{
    if (s_park_modal) return;
    lv_obj_t *scr = lv_screen_active();

    s_park_modal = lv_obj_create(scr);
    lv_obj_set_size(s_park_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_park_modal, 0, 0);
    lv_obj_set_style_bg_color(s_park_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_park_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_park_modal, 0, 0);
    lv_obj_set_style_pad_all(s_park_modal, 0, 0);
    lv_obj_clear_flag(s_park_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_park_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(s_park_modal);
    lv_obj_set_size(panel, 700, 150);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "POTA park reference");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* ⛔ SAY WHEN THIS FIELD IS NOT THE ONE BEING SENT. A running activation
     * outranks it (see park_resolve), so without this line the operator
     * types a reference, saves, and watches the Park row keep showing the
     * other one - which reads as the save having failed. Shown only while an
     * activation is running; it is the normal case that needs no words. */
    s_park_note = lv_label_create(panel);
    lv_obj_set_style_text_color(s_park_note, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_park_note, &lv_font_montserrat_18, 0);
    lv_obj_align(s_park_note, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_obj_add_flag(s_park_note, LV_OBJ_FLAG_HIDDEN);

    s_park_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_park_ta, 320, 56);
    lv_obj_align(s_park_ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_textarea_set_one_line(s_park_ta, true);
    lv_textarea_set_max_length(s_park_ta, 15);
    lv_textarea_set_placeholder_text(s_park_ta, "US-1211");
    lv_obj_set_style_text_font(s_park_ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_park_ta);
    lv_obj_add_event_cb(s_park_ta, park_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *btn_cancel = lv_btn_create(panel);
    lv_obj_set_size(btn_cancel, 150, 56);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -166, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_add_event_cb(btn_cancel, park_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_color(lc, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24, 0);
    lv_obj_center(lc);

    lv_obj_t *btn_save = lv_btn_create(panel);
    lv_obj_set_size(btn_save, 150, 56);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(btn_save, 8, 0);
    lv_obj_add_event_cb(btn_save, park_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(btn_save);
    lv_label_set_text(ls, "Save");
    lv_obj_set_style_text_color(ls, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ls, &lv_font_montserrat_24, 0);
    lv_obj_center(ls);
    ui_kbd_set_buttons(btn_save, btn_cancel);

    s_park_kb = lv_keyboard_create(s_park_modal);
    style_edit_keyboard(s_park_kb);
    lv_obj_add_event_cb(s_park_kb, park_kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_park_kb, park_kb_event_cb, LV_EVENT_CANCEL, NULL);
}

static void park_edit_open(void)
{
    park_build();
    char cur[16];
    settings_get_cw_park(cur, sizeof(cur));
    lv_textarea_set_text(s_park_ta, cur);
    if (s_park_note) {
        char ref[16];
        if (settings_get_activation_ref(ref, sizeof(ref))) {
            lv_label_set_text_fmt(s_park_note, "%s %s is active - it wins",
                                  settings_activation_sig_name(), ref);
            lv_obj_clear_flag(s_park_note, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_park_note, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_state(s_park_ta, LV_STATE_FOCUSED);
    lv_obj_add_flag(s_park_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_park_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_park_modal);
}

// ---- sending ----------------------------------------------------------------
// The ONE path every CW transmission takes (memory buttons and the ESM NEXT
// button): macro expansion, the simulation-mode interlock (sim never reaches
// the radio - the ft8_tx rule applied here at the single choke point), the
// CAT hand-off, and the capture notification. Returns true if "sent" (real
// or sim), false with a toast when something needed is missing.
static bool send_cw_text(const char *raw)
{
    char exp[CW_SEND_MAX + 1];
    const char *missing = NULL;
    if (!expand_macros(raw, exp, sizeof(exp), &missing)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Set your %s first", missing ? missing : "identity");
        ui_toast(msg);
        return false;
    }
    /* ⛔ The NARROW accessor, never settings_load_all(): this runs on
       taskLVGL and a qmx_settings_t is 3888 bytes for the sake of one bool.
       Two tasks died of exactly that on 2026-09-30 (usb_stale_det at 4.7 s,
       dxcluster at 82-110 s) - see CLAUDE.md's "Task stacks on this board
       are TINY". taskLVGL has room for it today; the struct grows every
       release and this line does not need to. */
    const bool cw_sim = settings_get_cw_sim_en();
    if (cw_sim) {
        ESP_LOGI(TAG, "[SIM] CW send suppressed: '%s'", exp);
        cw_qso_sim_arm();            // phantom answers by pre-advance state
        cw_screen_feed_decoded(" ", 1);   // keep the stream's token boundary honest
    } else {
        if (!cat_is_ready()) { ui_toast("No QMX connected"); return false; }
        cat_request_cw_send(exp);
    }
    cw_qso_note_sent(exp);
    ESP_LOGI(TAG, "CW send: '%s'%s", exp, cw_sim ? " [SIM]" : "");
    if (s_lbl_status) {
        lv_label_set_text_fmt(s_lbl_status, "Keying:\n%s", exp);
        lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_RED), 0);
    }
    return true;
}

// ---- memory buttons ---------------------------------------------------------
// Tap = expand + send; long-press = edit. LVGL delivers CLICKED after a long
// press too, so the press duration decides which action the CLICKED handler
// takes (the User-Manual button pattern in ui.c).
// The six memory buttons are 44 px tall on a 50 px pitch - thin enough that a
// fingertip covers more than one and you cannot tell which will fire. So the
// press is a PREVIEW, not a commitment (operator, 2026-08-04):
//   - the button that would fire lights light-grey while the finger is down;
//   - dragging up/down moves that highlight, so a mis-hit is corrected
//     without lifting;
//   - lifting fires whatever is highlighted;
//   - holding still on it for MEM_LONG_MS opens the editor instead.
// The hold timer RESTARTS whenever the highlight moves, so time spent hunting
// for the right button never counts toward the long-press - which is why the
// threshold could go up to a comfortable 2 s (it was 600 ms, unusable once
// dragging exists).
#define MEM_LONG_MS   2000
#define MEM_BG_IDLE   0x2a2f37
#define MEM_BG_ARMED  0x5b636f   // light grey: "this one will fire"

static uint32_t s_mem_press_tick = 0;
static int      s_mem_sel        = -1;   // highlighted button, -1 = none
static bool     s_mem_fired      = false; // editor already opened by the hold

static void mem_highlight(int idx)
{
    for (int i = 0; i < N_CW_MEM; i++) {
        if (!s_btn_mem[i]) continue;
        lv_obj_set_style_bg_color(s_btn_mem[i],
            lv_color_hex(i == idx ? MEM_BG_ARMED : MEM_BG_IDLE), 0);
    }
}

// Which memory button is under this screen point? Uses each button's absolute
// area so it stays correct wherever the pane sits. Y only: the finger may
// stray sideways off the narrow pane while dragging vertically, and that
// should not cancel the pick.
static int mem_hit_test(const lv_point_t *p)
{
    for (int i = 0; i < N_CW_MEM; i++) {
        if (!s_btn_mem[i]) continue;
        lv_area_t a;
        lv_obj_get_coords(s_btn_mem[i], &a);
        if (p->y >= a.y1 && p->y <= a.y2) return i;
    }
    return -1;
}

static void mem_pressed_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= N_CW_MEM) return;
    s_mem_sel = idx;
    s_mem_press_tick = lv_tick_get();
    s_mem_fired = false;
    mem_highlight(idx);
}

static void mem_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (s_mem_fired) return;            // editor already up; ignore the rest
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int idx = mem_hit_test(&p);
    if (idx >= 0 && idx != s_mem_sel) {
        s_mem_sel = idx;
        s_mem_press_tick = lv_tick_get();  // hold timer restarts on the new button
        mem_highlight(idx);
        return;
    }
    // The editor opens the INSTANT the hold is earned, with the finger still
    // down - a long-press that only acts on release feels broken, because
    // nothing tells you it worked until you let go (operator, 2026-08-04).
    // PRESSING is delivered on every input read (~30 ms), so this is precise
    // enough without a dedicated timer.
    if (s_mem_sel >= 0 && s_mem_press_tick &&
        lv_tick_elaps(s_mem_press_tick) >= MEM_LONG_MS) {
        int idx_open = s_mem_sel;
        s_mem_fired = true;
        s_mem_sel = -1;
        s_mem_press_tick = 0;
        mem_highlight(-1);
        edit_open(idx_open);
    }
}

static void mem_released_cb(lv_event_t *e)
{
    (void)e;
    int idx = s_mem_sel;
    bool fired = s_mem_fired;
    s_mem_sel = -1;
    s_mem_press_tick = 0;
    s_mem_fired = false;
    mem_highlight(-1);
    if (fired) return;                  // the hold already opened the editor
    if (idx < 0 || idx >= N_CW_MEM) return;

    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.cw_msg[idx][0]) { edit_open(idx); return; }   // empty slot: edit it
    send_cw_text(s.cw_msg[idx]);
}

static void mem_press_lost_cb(lv_event_t *e)
{
    (void)e;
    s_mem_sel = -1;
    s_mem_press_tick = 0;
    s_mem_fired = false;
    mem_highlight(-1);
}

static void park_edit_open(void);   // fwd (park editor below)
static void park_clicked_cb(lv_event_t *e) { (void)e; park_edit_open(); }

// ---- ESM NEXT ---------------------------------------------------------------
static void refresh_next_label(void)
{
    if (!s_lbl_next || !s_lbl_next_hdr) return;
    char txt[CW_SEND_MAX + 1];
    const char *state_lbl = "CQ";
    if (cw_qso_next_text(txt, sizeof(txt), &state_lbl)) {
        char exp[CW_SEND_MAX + 1];
        const char *missing = NULL;
        lv_label_set_text_fmt(s_lbl_next_hdr, "NEXT sends (%s):", state_lbl);
        lv_label_set_text(s_lbl_next,
            expand_macros(txt, exp, sizeof(exp), &missing) ? exp : txt);
    } else {
        lv_label_set_text_fmt(s_lbl_next_hdr, "NEXT (%s):", state_lbl);
        lv_label_set_text(s_lbl_next, "(memory empty)");
    }
}

static uint32_t s_next_press_tick = 0;

static void next_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_next_press_tick = lv_tick_get();
}

static void next_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_next_press_tick && lv_tick_elaps(s_next_press_tick) >= 600) {
        cw_qso_reset();
        ui_toast("Ladder reset - back to CQ");
        refresh_next_label();
        return;
    }
    char txt[CW_SEND_MAX + 1];
    const char *state_lbl = NULL;
    if (!cw_qso_next_text(txt, sizeof(txt), &state_lbl)) {
        ui_toast("Nothing to send - memory empty?");
        return;
    }
    // Grab the target BEFORE advancing: the TU step logs and resets it.
    char tgt[16];
    cw_qso_get_target(tgt, sizeof(tgt));
    if (!send_cw_text(txt)) return;    // (sim_arm happens inside, pre-advance)
    if (cw_qso_on_next_sent()) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Logged %s - back to CQ", tgt);
        ui_toast(msg);
    }
    refresh_next_label();
}

// ---- station card -----------------------------------------------------------
static void card_clicked_cb(lv_event_t *e)
{
    (void)e;
    char tgt[16], heard[16];
    int age = 0;
    cw_qso_get_target(tgt, sizeof(tgt));
    if (tgt[0]) {
        cw_qso_reset();
        char msg[40];
        snprintf(msg, sizeof(msg), "Dropped %s", tgt);
        ui_toast(msg);
    } else if (cw_qso_get_heard(heard, sizeof(heard), &age) && heard[0]) {
        cw_qso_adopt(heard);
    }
    refresh_next_label();
}

// Card refresh, from the 500 ms tick. Shows the adopted target (amber
// border) or the latest heard call (grey border, fades after 2 min).
static void refresh_card(void)
{
    if (!s_card) return;
    char tgt[16], heard[16];
    int age = 0;
    cw_qso_get_target(tgt, sizeof(tgt));
    bool have_heard = cw_qso_get_heard(heard, sizeof(heard), &age);
    // (cw_qso_get_heard may have just applied an auto-adopt - re-read.)
    if (!tgt[0]) cw_qso_get_target(tgt, sizeof(tgt));

    const char *call = tgt[0] ? tgt : (have_heard && age < 120) ? heard : NULL;
    if (!call) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_card_call, call);
    const char *country = dxcc_lookup(call);
    char worked[48];
    uint32_t f = cat_get_frequency();
    if (adif_log_contains_call_on_band(call, f))
        snprintf(worked, sizeof(worked), "worked before on this band");
    else if (adif_log_contains_call(call))
        snprintf(worked, sizeof(worked), "worked before - new band");
    else
        snprintf(worked, sizeof(worked), "new station");
    char rst[4], park[12];
    cw_qso_get_rst_rcvd(rst, sizeof(rst));
    bool have_park = cw_qso_get_heard_park(park, sizeof(park));
    // ASCII separators only - a middle dot is outside the compiled montserrat
    // glyph range and renders as a tofu box (hardware-observed 2026-08-02).
    // The park reference earns its place on the second line when there is
    // one: hunting is the case where you must write it down.
    if (have_park)
        lv_label_set_text_fmt(s_card_info, "%s\npark %s%s%s",
                              country ? country : "?", park,
                              (tgt[0] && rst[0]) ? " - rcvd " : "",
                              (tgt[0] && rst[0]) ? rst : "");
    else if (tgt[0] && rst[0])
        lv_label_set_text_fmt(s_card_info, "%s\n%s - rcvd %s",
                              country ? country : "?", worked, rst);
    else
        lv_label_set_text_fmt(s_card_info, "%s\n%s",
                              country ? country : "?", worked);
    // Short enough to fit the card at montserrat_18 - what NEXT will send is
    // already spelled out on the NEXT button itself, so saying it here too
    // was both redundant and what pushed this line off the edge.
    lv_label_set_text(s_card_hint,
                      tgt[0] ? "working - tap to drop"
                             : "tap to work this station");
    lv_obj_set_style_border_color(s_card,
        lv_color_hex(tgt[0] ? 0xFFA040 : 0x555555), 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

// Copilot hint line above the ticker: the ticker shows WHAT was heard, this
// says WHAT IT MEANT (docs/cw-page-design.md Tier 3). Hidden when there is
// nothing recent to say - a stale interpretation is worse than none, so it
// fades out after HINT_TTL_S rather than lingering next to newer decode text.
#define HINT_TTL_S 25
static void refresh_hint(void)
{
    if (!s_lbl_hint) return;
    cw_hint_t kind; char subj[16]; int age = 0;
    if (!cw_qso_get_hint(&kind, subj, sizeof(subj), &age) || age > HINT_TTL_S) {
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char line[96];
    cw_qso_hint_text(kind, subj, line, sizeof(line));
    if (!line[0]) { lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN); return; }
    lv_label_set_text(s_lbl_hint, line);
    // Green when it is something to act on, muted amber for the rest.
    bool actionable = (kind == CW_HINT_ANSWERED_YOU || kind == CW_HINT_CALLING_CQ ||
                       kind == CW_HINT_CQ_FROM ||
                       kind == CW_HINT_REPEAT || kind == CW_HINT_QRZ);
    lv_obj_set_style_text_color(s_lbl_hint,
        lv_color_hex(actionable ? 0x66DD88 : 0xFFA040), 0);
    lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
}

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

    /* ⛔ THE ACTIVATION CAN CHANGE WHILE THIS PAGE IS UP. It is started and
     * stopped from the Activation window and over /api/cmd, neither of which
     * knows this page exists, and refresh_mem_labels() only ran on page entry
     * and after an edit here - so the Park row and every memory preview
     * carrying {PARK} would have shown the pre-activation answer until the
     * page was left and re-entered.
     *
     * Guarded on a CHANGE, not called every tick: refresh_mem_labels() does a
     * settings_load_all() and six macro expansions, and running that twice a
     * second for ever is not what this costs. park_resolve() is narrow, so
     * the check itself is a mutex and a string compare. */
    {
        static char s_park_seen[24];
        char ref[16];
        const char *prog = NULL;
        char now[24];
        if (!park_resolve(ref, sizeof(ref), &prog)) snprintf(now, sizeof(now), "-");
        else snprintf(now, sizeof(now), "%s/%s", prog ? prog : "", ref);
        if (strcmp(now, s_park_seen) != 0) {
            snprintf(s_park_seen, sizeof(s_park_seen), "%s", now);
            refresh_mem_labels();
            refresh_next_label();
        }
    }

    // ESM/card refresh + the phantom practice pump (CW sim only).
    {
        qmx_settings_t qs;
        settings_load_all(&qs);
        if (qs.cw_sim_en) cw_qso_sim_tick();
    }
    refresh_card();
    refresh_next_label();
    refresh_hint();

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

    // Left pane, full middle-band height - the band-plan strip starts at the
    // pane's right edge on this page (ui.c narrows it), so nothing hides.
    s_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_pane, PANE_W, MID_H);
    lv_obj_set_pos(s_pane, 0, 0);
    lv_obj_set_style_bg_color(s_pane, lv_color_hex(0x14181e), 0);
    lv_obj_set_style_bg_opa(s_pane, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pane, 0, 0);
    lv_obj_set_style_pad_all(s_pane, 0, 0);
    lv_obj_clear_flag(s_pane, LV_OBJ_FLAG_SCROLLABLE);

    /* Page header, at the FT8 pane's size and position so the two pages read
     * as siblings (operator request 2026-08-01).
     *
     * ⛔ CW BLUE, NOT GOLD. Gold is FT8's accent, and it was copied here when
     * CW was the second page and there was nothing else for it to clash with.
     * Each mode now owns a colour, and the left drawer's button carries it:
     * "it is not the drawer but the button label that must be the same colour
     * as the page it opens" (operator, on left_drawer_add_btn). This title and
     * that button are the same object seen from inside and outside the page,
     * so they read the same constant - gold here made the pair disagree.
     * Operator, 2026-10-31: "MODE: CW on the page is not the same blue colour
     * as we have in the left drawer". */
    lv_obj_t *hdr = lv_label_create(s_pane);
    lv_label_set_text(hdr, "MODE: CW");
    lv_obj_set_style_text_color(hdr, lv_color_hex(UI_COLOR_MODE_CW_TEXT), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(hdr, 14, 6);

    // Pane order (operator, 2026-08-02): header, then the SETTINGS (WPM,
    // park), then the ACTIONS (NEXT, memories), status at the bottom.

    // WPM stepper row.
    lv_obj_t *bm = make_pane_btn(s_pane, 10, 58, 70, 48, 0x2a2f37, 0x555555);
    lv_obj_add_event_cb(bm, wpm_minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lm = lv_label_create(bm);
    lv_label_set_text(lm, "-");
    lv_obj_set_style_text_color(lm, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lm, &lv_font_montserrat_28, 0);
    lv_obj_center(lm);

    // Spans the whole gap between the two buttons (they end at x=80 and start
    // at PANE_W-80) with centred text, so the number sits mid-row and STAYS
    // there as it changes width - at a fixed x it drifted between "8 WPM" and
    // "18 WPM" (operator, 2026-08-04).
    s_lbl_wpm = lv_label_create(s_pane);
    lv_label_set_text(s_lbl_wpm, "-- WPM");
    lv_obj_set_style_text_color(s_lbl_wpm, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_lbl_wpm, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_lbl_wpm, (PANE_W - 80) - 80);
    lv_obj_set_style_text_align(s_lbl_wpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_wpm, 80, 67);   // 58 + (48 - ~29 line height) / 2

    lv_obj_t *bp = make_pane_btn(s_pane, PANE_W - 80, 58, 70, 48, 0x2a2f37, 0x555555);
    lv_obj_add_event_cb(bp, wpm_plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lp = lv_label_create(bp);
    lv_label_set_text(lp, "+");
    lv_obj_set_style_text_color(lp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_28, 0);
    lv_obj_center(lp);

    // Park button.
    lv_obj_t *bpark = make_pane_btn(s_pane, 10, 110, PANE_W - 20, 44, 0x1c2128, 0x7a4a10);
    lv_obj_add_event_cb(bpark, park_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_park = lv_label_create(bpark);
    lv_label_set_text(s_lbl_park, "Park: --");
    lv_obj_set_style_text_color(s_lbl_park, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_lbl_park, &lv_font_montserrat_22, 0);
    lv_obj_center(s_lbl_park);

    // ESM NEXT button (Tier 2): green like every commit button; the body
    // label always states what a tap will key.
    s_btn_next = make_pane_btn(s_pane, 10, 158, PANE_W - 20, 68, 0x2e8b3a, 0x2e8b3a);
    lv_obj_add_event_cb(s_btn_next, next_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn_next, next_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_next_hdr = lv_label_create(s_btn_next);
    lv_label_set_text(s_lbl_next_hdr, "NEXT sends (CQ):");
    lv_obj_set_style_text_color(s_lbl_next_hdr, lv_color_hex(0xd8ffd8), 0);
    lv_obj_set_style_text_font(s_lbl_next_hdr, &lv_font_montserrat_18, 0);
    lv_obj_align(s_lbl_next_hdr, LV_ALIGN_TOP_LEFT, 4, 0);
    s_lbl_next = lv_label_create(s_btn_next);
    lv_label_set_text(s_lbl_next, "");
    lv_label_set_long_mode(s_lbl_next, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_next, PANE_W - 44);
    lv_obj_set_style_text_color(s_lbl_next, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_next, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_next, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    // Six memory buttons, single-line previews (long-press shows the full
    // text in the editor; the NEXT button carries the full ladder text).
    int y = 232;
    for (int i = 0; i < N_CW_MEM; i++) {
        s_btn_mem[i] = make_pane_btn(s_pane, 10, y, PANE_W - 20, 44, MEM_BG_IDLE, 0x555555);
        // Press = preview, drag = re-pick, release = fire, hold = edit. Acting
        // on RELEASED rather than CLICKED is what lets the finger travel to a
        // different button before lifting.
        lv_obj_add_event_cb(s_btn_mem[i], mem_pressed_cb, LV_EVENT_PRESSED,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(s_btn_mem[i], mem_pressing_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_btn_mem[i], mem_released_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn_mem[i], mem_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
        s_lbl_mem[i] = lv_label_create(s_btn_mem[i]);
        lv_label_set_text(s_lbl_mem[i], "");
        lv_label_set_long_mode(s_lbl_mem[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_lbl_mem[i], PANE_W - 44);
        lv_obj_set_style_text_color(s_lbl_mem[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(s_lbl_mem[i], &lv_font_montserrat_20, 0);
        lv_obj_center(s_lbl_mem[i]);
        y += 50;
    }
    y += 6;

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

    // Copilot hint line, immediately above the ticker and left-aligned with
    // it: the ticker is the raw copy, this is the interpretation. Kept OUT of
    // the ticker box so it cannot be mistaken for decoded text, and short
    // enough to never collide with the station card at the right end.
    s_lbl_hint = lv_label_create(s_container);
    lv_label_set_text(s_lbl_hint, "");
    lv_label_set_long_mode(s_lbl_hint, LV_LABEL_LONG_DOT);
    // Ends at x 898, just short of the station card's new left edge (910).
    lv_obj_set_width(s_lbl_hint, MID_W - PANE_W - 390);
    lv_obj_set_pos(s_lbl_hint, PANE_W + 8, MID_H - TICKER_H - BANDPLAN_H - 30);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0xFFA040), 0);
    lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);

    // Station card, floating above the ticker's right end.
    s_card = lv_obj_create(s_container);
    lv_obj_set_size(s_card, 330, 122);
    // x: clear of the operator watermark, which is a 25 px wide vertical
    // image at DISPLAY_H_RES - 4 - 25 = x 1251..1276 (build_signature in
    // ui.c). At the old MID_W - 340 the card ran to x 1270 and the
    // signature showed through its right edge (operator, 2026-08-04);
    // MID_W - 370 ends it at 1240, a 11 px gap.
    lv_obj_set_pos(s_card, MID_W - 370, MID_H - TICKER_H - BANDPLAN_H - 132);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_radius(s_card, 8, 0);
    lv_obj_set_style_pad_all(s_card, 8, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card, card_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_card_call = lv_label_create(s_card);
    lv_label_set_text(s_card_call, "");
    lv_obj_set_style_text_color(s_card_call, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_card_call, &lv_font_montserrat_28, 0);
    // Bounded like the other two: a long portable call (VK9/SM5ABC) at 28 px
    // is close to the card width, and an unbounded label runs off the edge
    // rather than truncating visibly.
    lv_obj_set_width(s_card_call, 310);
    lv_label_set_long_mode(s_card_call, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_card_call, 0, 0);

    s_card_info = lv_label_create(s_card);
    lv_label_set_text(s_card_info, "");
    // WRAP, not DOT: the info is two lines (country + worked-status) and DOT
    // truncated it to "USA..." (hardware-observed 2026-08-02).
    lv_label_set_long_mode(s_card_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_card_info, 310);
    lv_obj_set_style_text_color(s_card_info, lv_color_hex(0xdddddd), 0);
    lv_obj_set_style_text_font(s_card_info, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(s_card_info, 0, 36);

    s_card_hint = lv_label_create(s_card);
    lv_label_set_text(s_card_hint, "");
    lv_obj_set_style_text_color(s_card_hint, lv_color_hex(0x8a9199), 0);
    lv_obj_set_style_text_font(s_card_hint, &lv_font_montserrat_18, 0);
    // Bound the label to the card's inner width (330 - 2x8 pad) with DOT
    // truncation: without this it had no width at all and simply ran past
    // the card edge, clipping mid-word ("...tap to dr", operator-reported
    // 2026-08-04). DOT rather than WRAP because the card's height is fixed
    // and a second line would push out the bottom instead.
    lv_obj_set_width(s_card_hint, 310);
    lv_label_set_long_mode(s_card_hint, LV_LABEL_LONG_DOT);
    lv_obj_align(s_card_hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    ESP_LOGI(TAG, "CW screen built");
}

lv_obj_t *cw_screen_view_get_container(void) { return s_container; }

void cw_screen_view_show(void)
{
    if (!s_container) return;
    {
        qmx_settings_t s;
        settings_load_all(&s);
        cw_qso_set_my_call(s.my_callsign);   // capture heuristics need it
    }
    refresh_mem_labels();
    refresh_next_label();
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
    park_close();
}
