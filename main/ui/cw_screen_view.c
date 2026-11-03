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
// manual documents no KY abort in TS480-compat-OFF mode. Tested on bench1
// with a dummy load 2026-11-02 (see
// memory/project_cw_ky_abort_rx_does_not_work.md): `RX;` mid-send and a
// mode bounce (MD2;/MD3;) BOTH fail - the TB; t-field counts down to 0
// unaffected either way, same as sending nothing. Nothing is left in the
// CAT manual to try; the next step is asking QRP Labs, not another guess.

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
/* Four rows of qmx_mono_25 plus this container's 8 px padding top and bottom.
 * 124 is the arithmetic for a 27 px line height; the exact pitch is measured
 * from the font where the labels are built (tk_row_px), and this constant only
 * has to be >= 4 * pitch + 16. A box sized by eye instead of from its text is
 * what clipped the drawer captions on 2026-11-02 - so if the font ever changes,
 * this changes with it. */
#define TICKER_H      124
/* Columns the ticker fits: (MID_W - PANE_W) = 960 px at qmx_mono_25's 15 px
 * advance, minus one so the rightmost glyph is not flush against the border.
 * ⛔ This is the grid's WRAP MODULUS while this page is up, not a draw width. */
#define CW_TICKER_COLS 63
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
LV_FONT_DECLARE(qmx_mono_25);
/* Three layers per row, exactly as ui.c draws the panadapter's CW line, and
 * for the reason written there: LVGL 9.2 has no label recolour, and two glyphs
 * at one position in different colours leave an anti-aliased fringe. Spaces
 * paint nothing, so the layers interleave cleanly on one line.
 *  now  = cyan, written this pass
 *  prev = green, surviving from the previous pass - the boundary IS the cursor
 *  tx   = orange, the operator's own keying (UI_COLOR_TX_ACTIVE) */
static lv_obj_t *s_tk_now[CW_LINE_ROWS];
static lv_obj_t *s_tk_prev[CW_LINE_ROWS];
static lv_obj_t *s_tk_tx[CW_LINE_ROWS];
static lv_obj_t *s_lbl_hint  = NULL;   // copilot: plain-language read of the decode
static lv_obj_t *s_btn_set_a = NULL;   // "General" toggle half
static lv_obj_t *s_btn_set_b = NULL;   // "Contest" toggle half
static lv_obj_t *s_lbl_set_a = NULL;
static lv_obj_t *s_lbl_set_b = NULL;
static lv_obj_t *s_btn_mem[N_CW_MEM] = {0};
static lv_obj_t *s_lbl_mem[N_CW_MEM] = {0};
static lv_obj_t *s_lbl_wpm   = NULL;
static lv_obj_t *s_wpm_panel = NULL;   /* numeric entry popup - built lazily */
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
/* ⭐ THE SECOND MODEL IS GONE (2026-11-02). This file used to keep its own
 * 512-byte ring, its own squelch instance and its own spinlock, fed by
 * cw_screen_feed_decoded() - two models of one stream, which is the fault the
 * comment that used to live here already warned about for the squelch alone.
 * The ticker now draws cw_decode.c's grid, the same one the panadapter and the
 * browser draw, so there is one squelch, one ring and one wrap position.
 *
 * What that bought: per-character TX colour, which a proportional font could
 * never give, and Bryan N0LUF's 2-or-4 row setting for free. What it cost: the
 * CW simulator had to gain a way into the grid - see cw_decode_feed_text(). */

// Wipe everything the decode stream produced: the ticker's ring, the copilot
// hint, the captured/adopted station and the ESM ladder. Called when
// simulation mode is switched off so the phantom's QSO does not linger on
// screen looking like real copy (operator-reported 2026-08-04) - the same
// contract FT8 has, where ft8_op_mode_set() clears the decode list on a sim
// exit. Safe from the LVGL thread only (touches labels).
void cw_screen_clear_decode(void)
{
    /* The decode text itself lives in cw_decode.c now and is shared with the
     * panadapter, so this no longer clears it - wiping a grid the other screen
     * also draws would be this page reaching outside itself. The labels are
     * blanked below; the grid ages out on its own. */
    cw_qso_reset();          // drops target + ladder
    cw_qso_clear_capture();  // drops the heard call and the hint
    for (int r = 0; r < CW_LINE_ROWS; r++) {
        if (s_tk_now[r])  lv_label_set_text(s_tk_now[r],  "");
        if (s_tk_prev[r]) lv_label_set_text(s_tk_prev[r], "");
        if (s_tk_tx[r])   lv_label_set_text(s_tk_tx[r],   "");
    }
    if (s_lbl_hint)   lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_card)       lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

/* ⭐ NO FILTER LIVES HERE ANY MORE. The ticker draws cw_decode.c's grid, which
 * is squelched once on the way in, so this file holds no squelch, no ring and
 * no spinlock of its own. The important property is unchanged and is the whole
 * reason the split exists: the filter applies ONLY to what is displayed,
 * because cw_qso_feed() below still gets the RAW stream - so callsign capture,
 * the copilot hints and the ESM ladder cannot lose a character to it. */
void cw_screen_feed_decoded(const char *s, int n)
{
    if (!s || n <= 0) return;
    cw_qso_feed(s, n);   // RAW stream: capture / ESM / hints are never filtered
    /* ⚠ Raw is right for cw_qso: its tokenizer already treats anything outside
     * [A-Z0-9/-] as a token separator, so '*' splits tokens there instead of
     * corrupting them, and the capture wants every character it can get.
     *
     * The TICKER is not fed from here any more - it draws cw_decode.c's grid,
     * which cat.c fills from the same TB frame via cw_decode_feed(). So this
     * function is now exactly what its first line does. */
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
    /* ⛔ THE NARROW ACCESSOR, NOT settings_load_all() - this function was
     * breaking the very rule stated in the comment above it: called from
     * refresh_mem_labels()'s 500 ms tick and refresh_next_label(), a full
     * qmx_settings_t (3888 B) was landing on taskLVGL's stack twice a
     * second. Only my_callsign was ever read from it. */
    char my_call[16];
    settings_get_my_callsign(my_call, sizeof(my_call));
    size_t o = 0;
    *missing = NULL;
    char tgt[16];
    char park[16];
    cw_qso_get_target(tgt, sizeof(tgt));
    for (const char *p = in; *p && o + 1 < out_sz; ) {
        const char *val = NULL;
        if (strncasecmp(p, "{MYCALL}", 8) == 0) {
            if (!my_call[0]) { *missing = "callsign (Identity)"; return false; }
            val = my_call; p += 8;
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
        } else if (strncasecmp(p, "{RST}", 5) == 0) {
            /* Pro-forma 599 - never a real measurement, it's a fixed report
             * macro, the same way WSJT-X/N1MM treat a contest exchange.
             * {RST}'s own button long-press toggles the cut (settings
             * cw_rst_cut_nines): "5NN" when on, plain "599" when off.
             * Michael KZ4LY, 2026-11-03: "why waste dahs on a pro-forma
             * RST?" - cut 0->T for a serial number is a DIFFERENT macro
             * (a sequential counter), not this one, and not built yet. */
            val = settings_get_cw_rst_cut_nines() ? "5NN" : "599";
            p += 5;
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

// Which set's text a slot actually sends - toggle decided 2026-11-02 (see
// memory/project_cw_macro_set_selector_decided.md). Every reader of cw_msg[]
// goes through this, so there is exactly one place that knows the rule.
static inline const char *active_cw_msg(const qmx_settings_t *s, int i)
{
    return s->cw_msg_set ? s->cw_msg_b[i] : s->cw_msg[i];
}

static void refresh_next_label(void);   // fwd (ESM section below) - needed early by set_toggle_cb
static void refresh_mem_labels(void);    // fwd - defined right below, but set_toggle_cb (above it) needs it too

// Highlights whichever half is active - filled CW blue, the other dimmed.
// Called on page show and right after a toggle tap; nothing else writes
// cw_msg_set today, so there is no staleness case to guard against yet.
//
// Label colour follows the fill: UI_COLOR_MODE_CW_TEXT (0x5CB4F0) is a light
// blue, and white text on it reads as low-contrast (operator, 2026-11-02) -
// black reads cleanly there, so the ON half gets black text and the OFF half
// (dark 0x2a2f37) keeps white.
static void refresh_set_toggle(void)
{
    if (!s_btn_set_a || !s_btn_set_b) return;
    qmx_settings_t s;
    settings_load_all(&s);
    lv_obj_t *on  = s.cw_msg_set ? s_btn_set_b : s_btn_set_a;
    lv_obj_t *off = s.cw_msg_set ? s_btn_set_a : s_btn_set_b;
    lv_obj_t *lbl_on  = s.cw_msg_set ? s_lbl_set_b : s_lbl_set_a;
    lv_obj_t *lbl_off = s.cw_msg_set ? s_lbl_set_a : s_lbl_set_b;
    lv_obj_set_style_bg_color(on,  lv_color_hex(UI_COLOR_MODE_CW_TEXT), 0);
    lv_obj_set_style_bg_opa(on, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(off, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_bg_opa(off, LV_OPA_COVER, 0);
    if (lbl_on)  lv_obj_set_style_text_color(lbl_on,  lv_color_hex(0x000000), 0);
    if (lbl_off) lv_obj_set_style_text_color(lbl_off, lv_color_hex(0xffffff), 0);
}

// Tap either half: switch the active set and refresh everything that reads
// it (the memory previews, the NEXT ladder's next line, and this toggle).
static void set_toggle_cb(lv_event_t *e)
{
    uint8_t set = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    settings_set_cw_msg_set(set);
    refresh_set_toggle();
    refresh_mem_labels();
    refresh_next_label();
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
        const char *m = active_cw_msg(&s, i);
        const char *txt = m[0] ? m : "(empty)";
        if (m[0] && expand_macros(m, exp, sizeof(exp), &missing))
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
// The park has its OWN window (park_build() below), not this one - the
// comment that said otherwise outlived the split by months.
//
// Three rows: the title alone, then the five buttons, then the six fields in
// a scrollable container. edit_layout() owns every number that depends on
// whether the keyboard is up, and the arithmetic for why it has to is
// written there.
static lv_obj_t *s_edit_modal = NULL;
static lv_obj_t *s_edit_ta6[N_CW_MEM] = {0};
static lv_obj_t *s_edit_num[N_CW_MEM] = {0};
static lv_obj_t *s_edit_title = NULL;
static lv_obj_t *s_edit_kb    = NULL;
static uint8_t   s_edit_set   = 0;   // which set this editor instance is showing,
                                      // captured at open so a toggle elsewhere mid-edit
                                      // cannot move the Save onto the other set

static void refresh_next_label(void);   // fwd (ESM section below)
static void edit_layout(bool kb_up);    // fwd (defined with edit_build)

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
        settings_set_cw_msg_for_set(s_edit_set, (uint8_t)i, buf);
    }
    ESP_LOGI(TAG, "saved all 6 CW macros, set %s", s_edit_set ? "Contest" : "General");
    edit_close();
    refresh_mem_labels();
    refresh_next_label();
}

static void edit_cancel_cb(lv_event_t *e) { (void)e; edit_close(); }

static void edit_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
        edit_layout(false);
    }
}

/* The field the macro buttons paste into. The keyboard already tracks it, but
 * lv_keyboard_get_textarea() reads back NULL once the keyboard is hidden, and
 * the buttons have to work with it down. */
static lv_obj_t *s_edit_focus = NULL;
static lv_obj_t *s_edit_macro_lbl[4];   /* the four button labels */
static lv_obj_t *s_edit_sends = NULL;   /* the live "sends:" line */
static lv_obj_t *s_edit_panel = NULL;   /* the window itself - it resizes */
static lv_obj_t *s_edit_list  = NULL;   /* the six fields, scrollable */

/* What each button PASTES - always the token, never the value. The label
 * shows the value; the memory must keep the macro or it would freeze at
 * whatever was current when it was written. */
static const char *const EDIT_MACROS[4] = { "{MYCALL}", "{PARK}", "{CALL}", "{RST}" };

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
    /* ⛔ ALWAYS "My call", never the callsign itself. Operator, 2026-11-02:
     * "revert to My call on the first top button - not whatever the Callsign
     * reads in the Settings drawer". This is the one of the three whose value
     * he already knows by heart, so printing it says nothing while the name
     * says what the button will insert. The other two keep showing their
     * values, which DO change: the park reference and the station being
     * worked. */
    if (s_edit_macro_lbl[0])
        lv_label_set_text(s_edit_macro_lbl[0], "My call");
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
    /* Shows what {RST} currently expands to, so the cut setting (toggled by
     * long-pressing THIS button - see rst_btn_cb()) is visible without
     * opening a separate menu. */
    if (s_edit_macro_lbl[3])
        lv_label_set_text(s_edit_macro_lbl[3],
                          settings_get_cw_rst_cut_nines() ? "5NN" : "599");
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
        edit_layout(true);
        lv_obj_scroll_to_view(s_edit_focus, LV_ANIM_OFF);
    }
    edit_sends_refresh();
}

/* Long-press on the {RST} button ONLY - toggles between "599" and "5NN"
 * (settings cw_rst_cut_nines). Mirrors this page's established tap/hold
 * language (memory buttons: tap sends, hold edits) rather than opening a
 * separate settings screen for one bool. Refreshes both the button's own
 * label and the live "sends:" preview, since an already-pasted {RST} token
 * re-expands to the new value immediately. */
static void rst_btn_long_cb(lv_event_t *e)
{
    (void)e;
    settings_set_cw_rst_cut_nines(!settings_get_cw_rst_cut_nines());
    edit_macro_labels_refresh();
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
    /* ⛔ ORDER. Shrink the list before scrolling into it: scroll_to_view
     * against the tall viewport decides nothing needs to move, and then the
     * resize leaves the focused field below the fold. */
    edit_layout(true);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

/* ⛔ TWO GEOMETRIES, ONE WINDOW - the keyboard decides which.
 *
 * Operator, 2026-11-02: "the CW memories window should be centred on the
 * screen - now its sitting at the top", "I like a bit more space between
 * the fields 1-6", and "I want the whole window including all fonts on
 * fields, text, headers and buttons labels to be bigger - we are 60+ most
 * of the HAM's and wear glasses".
 *
 * Those three cannot all hold at once on 720 px, and the arithmetic says so
 * plainly: six fields at a readable pitch need ~410 px, the title and button
 * rows take 104, the "sends:" preview 28, the panel's own padding 32. That
 * is 574 px of window. The keyboard is 280. 574 + 280 = 854 > 720, short by
 * 134 px. Every candidate fix is a trade:
 *
 *   - shrink the keyboard: 1 px per px, and 134 px off 280 puts the key rows
 *     at 36 px. He asked for bigger targets, not smaller ones.
 *   - two columns: a 43-character field at montserrat_28 is ~690 px wide.
 *     Two of them do not fit 1168.
 *   - leave the fonts alone: that is the request, refused.
 *
 * So the window is full size and the FIELD LIST scrolls, but only while the
 * keyboard is up - which is the one moment the operator is looking at a
 * single line anyway. With the keyboard down, which is what he sees when the
 * window opens, all six fields are visible at full size and the window is
 * centred. Nothing is traded away in the state he judged it in.
 *
 * ⛔ THE "sends:" LINE MOVES WITH THE LIST. It is the live preview of the
 * field being typed into, so it is worth nothing behind the keys. */
static void edit_layout(bool kb_up)
{
    if (!s_edit_panel) return;

    /* Panel: 648 tall and centred (36..684 of 720) with the keyboard down;
     * 424 tall at y=8 with it up, so its bottom edge lands 8 px clear of the
     * keyboard at y=440 instead of being swallowed by it. */
    lv_obj_set_height(s_edit_panel, kb_up ? 424 : 648);
    lv_obj_align(s_edit_panel, LV_ALIGN_TOP_MID, 0, kb_up ? 8 : 36);

    /* Content box is 948 x (panel - 32), and the three rows are spaced 32 px
     * apart, not 8 and 12: title 0..40, buttons 72..128, list from 160.
     * Operator, 2026-11-02: "too little space between 1st and 2nd and 3rd
     * row". Six fields at pitch 70 are 408 px.
     *
     * The list is the only thing that gives when the keyboard is up - the
     * row rhythm above it stays put, because a header that re-flows the
     * moment a field is tapped moves the buttons under the finger. */
    lv_obj_set_height(s_edit_list, kb_up ? 196 : 408);
    lv_obj_set_y(s_edit_sends, kb_up ? 364 : 580);
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
    s_edit_panel = panel;
    /* ⛔ 960, NOT 1200. The window was 1200 wide for one reason: Cancel and
     * Save were pinned to the right edge while the three macro buttons sat
     * at the left, so the button row carried a 195 px hole and the window
     * had to be wide enough to hold it. Operator, 2026-11-02: "Cancel and
     * Save buttons too far to the right so window need to be wider than
     * necessary". Group all five left instead and the row measures 928, so
     * the window is 240 px narrower with nothing lost.
     * Height and placement belong to edit_layout(); this is just a start. */
    lv_obj_set_size(panel, 1090, 648);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_edit_title = lv_label_create(panel);
    lv_label_set_text(s_edit_title, "CW macros");   // edit_open() sets the real "- Contest/POTA" text
    lv_obj_set_style_text_color(s_edit_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_edit_title, &lv_font_montserrat_32, 0);
    /* ALONE ON THE TOP LINE. The five buttons used to share it, which is
     * what forced them narrow and put the third one under Cancel. Operator,
     * 2026-11-02: "the five buttons at the top could jump down on the 2nd
     * line so CW memories sat alone on the top line". */
    lv_obj_align(s_edit_title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* THE SECOND ROW, now six buttons (2026-11-03 adds {RST}):
     *
     *   0   My call 170 | 182 My park 170 | 364 Their call 200 | 584 RST 100 -> 684
     *   724 Cancel  150 | 894 Save    150                                   -> 1044
     *
     * RST sits right after the three original macros with a smaller 20 px
     * gap (it reads as part of the same group, not a fourth category); the
     * original 48 px gap before Cancel/Save is unchanged and still the only
     * wide gap in the row, so a thumb reaching for a macro still cannot
     * land on Save. Cancel/Save narrowed 160->150 and the panel widened
     * 980->1090 to fit RST without re-flowing anything else - "widen, don't
     * shrink the font" (the macro button font is untouched).
     *
     * Fixed widths, not content-sized: the labels swap between a name and a
     * callsign as settings change, and a row that re-flows every time a
     * station is adopted would move the button under the finger reaching
     * for it.
     *
     * ⛔ ALL FIVE LABELS ARE montserrat_28. The three macro buttons were 24
     * while Cancel and Save were 28, which the operator spotted at once:
     * "the blue text in those buttons are smaller why?". There was no
     * reason for it - 24 was what fitted when these buttons were 128 px
     * wide, and it outlived that. "Their call" takes 200 rather than 180 at
     * the larger size so a 9-character callsign still fits inside it. */
    {
        static const int MX[4] = { 0, 182, 364, 584 };
        static const int MW[4] = { 170, 170, 200, 100 };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *b = lv_btn_create(panel);
            lv_obj_set_size(b, MW[i], 56);
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, MX[i], 72);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2f37), 0);
            lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_add_event_cb(b, edit_macro_cb, LV_EVENT_CLICKED,
                                (void *)EDIT_MACROS[i]);
            /* RST alone also toggles 599/5NN on a hold - see its own comment. */
            if (i == 3)
                lv_obj_add_event_cb(b, rst_btn_long_cb, LV_EVENT_LONG_PRESSED, NULL);
            s_edit_macro_lbl[i] = lv_label_create(b);
            /* Text comes from edit_macro_labels_refresh() on every open. */
            lv_label_set_text(s_edit_macro_lbl[i], "");
            /* CW blue, the page's own colour - these insert, they do not
             * commit, so they must not read as Save. */
            lv_obj_set_style_text_color(s_edit_macro_lbl[i],
                                        lv_color_hex(UI_COLOR_MODE_CW_TEXT), 0);
            lv_obj_set_style_text_font(s_edit_macro_lbl[i], &lv_font_montserrat_28, 0);
            lv_obj_center(s_edit_macro_lbl[i]);
        }
    }

    lv_obj_t *btn_cancel = lv_btn_create(panel);
    lv_obj_set_size(btn_cancel, 150, 56);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_LEFT, 724, 72);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_add_event_cb(btn_cancel, edit_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_color(lc, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_28, 0);
    lv_obj_center(lc);

    lv_obj_t *btn_save = lv_btn_create(panel);
    lv_obj_set_size(btn_save, 150, 56);
    lv_obj_align(btn_save, LV_ALIGN_TOP_LEFT, 894, 72);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(btn_save, 8, 0);
    lv_obj_add_event_cb(btn_save, edit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(btn_save);
    lv_label_set_text(ls, "Save");
    lv_obj_set_style_text_color(ls, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ls, &lv_font_montserrat_28, 0);
    lv_obj_center(ls);
    ui_kbd_set_buttons(btn_save, btn_cancel);

    /* The six fields live in their own container so the title and the two
     * buttons CANNOT scroll away - Save has to stay reachable whatever the
     * list is doing. Transparent and borderless: it is a viewport, not a
     * box, and must not read as one. */
    s_edit_list = lv_obj_create(panel);
    lv_obj_set_width(s_edit_list, 948);
    lv_obj_set_y(s_edit_list, 160);
    lv_obj_set_x(s_edit_list, 0);
    lv_obj_set_style_bg_opa(s_edit_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_edit_list, 0, 0);
    lv_obj_set_style_pad_all(s_edit_list, 0, 0);
    lv_obj_set_style_radius(s_edit_list, 0, 0);
    lv_obj_set_scroll_dir(s_edit_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_edit_list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < N_CW_MEM; i++) {
        /* Pitch 70 for a 58-tall field: 12 px of air between the lines,
         * where it used to be 6. Operator, 2026-11-02: "I like a bit more
         * space between the fields 1-6". */
        int fy = i * 70;
        s_edit_num[i] = lv_label_create(s_edit_list);
        lv_label_set_text_fmt(s_edit_num[i], "%d", i + 1);
        lv_obj_set_style_text_color(s_edit_num[i], lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(s_edit_num[i], &lv_font_montserrat_28, 0);
        lv_obj_set_pos(s_edit_num[i], 4, fy + 14);

        /* 904 of the list's 948, after the number column. The fields never
         * needed the full 1020 of the old window - operator, 2026-11-02:
         * "the fields for 1-6 need not be this wide" - and 43 characters at
         * montserrat_28 measures about 730 px, so the longest message still
         * has room. The slack on the right is where the scrollbar sits
         * without covering any text. */
        s_edit_ta6[i] = lv_textarea_create(s_edit_list);
        lv_obj_set_size(s_edit_ta6[i], 904, 58);
        lv_obj_set_pos(s_edit_ta6[i], 44, fy);
        lv_textarea_set_one_line(s_edit_ta6[i], true);
        lv_textarea_set_max_length(s_edit_ta6[i], 43);
        lv_obj_set_style_text_font(s_edit_ta6[i], &lv_font_montserrat_28, 0);
        ui_theme_style_textarea(s_edit_ta6[i]);
        lv_obj_add_event_cb(s_edit_ta6[i], edit_ta_focused_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(s_edit_ta6[i], edit_ta_changed_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* WHAT THIS MEMORY WILL ACTUALLY KEY, under the field being edited. Its
     * y belongs to edit_layout(): it has to follow the list up when the
     * keyboard rises, because a preview behind the keys is worth nothing. */
    s_edit_sends = lv_label_create(panel);
    lv_label_set_text(s_edit_sends, "");
    lv_obj_set_style_text_color(s_edit_sends, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_edit_sends, &lv_font_montserrat_22, 0);
    lv_label_set_long_mode(s_edit_sends, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_edit_sends, 904);
    lv_obj_set_pos(s_edit_sends, 44, 580);

    edit_layout(false);

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
    s_edit_set = s.cw_msg_set;
    if (s_edit_title)
        lv_label_set_text_fmt(s_edit_title, "CW macros - %s",
                               s_edit_set ? "Contest" : "General");
    for (int i = 0; i < N_CW_MEM; i++) {
        lv_textarea_set_text(s_edit_ta6[i], active_cw_msg(&s, i));
        lv_obj_remove_state(s_edit_ta6[i], LV_STATE_FOCUSED);
    }
    /* No field is focused on open, so nothing is a paste target yet - a
     * macro button tapped first would otherwise land in whichever line was
     * focused the last time this window was used. */
    s_edit_focus = NULL;
    edit_macro_labels_refresh();
    edit_sends_refresh();
    lv_obj_add_flag(s_edit_kb, LV_OBJ_FLAG_HIDDEN);
    edit_layout(false);
    lv_obj_scroll_to_y(s_edit_list, 0, LV_ANIM_OFF);
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
        // T142: "Keying:" in red claims the radio is transmitting. A sim send
        // never reaches CAT (see above) - the bezel already warns the whole
        // screen that nothing here is real, so this line must not repeat the
        // opposite claim for the ~500 ms before the next poll tick overwrites it.
        if (cw_sim) {
            lv_label_set_text_fmt(s_lbl_status, "Simulated:\n%s", exp);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        } else {
            lv_label_set_text_fmt(s_lbl_status, "Keying:\n%s", exp);
            lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_RED), 0);
        }
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
// for the right button never counts toward the long-press - 600 ms used to be
// unusable for exactly that reason before the drag-correct highlight existed.
// 2026-11-02: 2000 ms read as "ultra-long" to the operator once the highlight
// made hunting free; 800 ms (near ota_modal's 700 ms precedent) trades back a
// little of that accident margin for a press that feels normal.
#define MEM_LONG_MS   800
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
    const char *m = active_cw_msg(&s, idx);
    if (!m[0]) { edit_open(idx); return; }   // empty slot: edit it
    send_cw_text(m);
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

/* ⭐ DIRECT NUMERIC WPM ENTRY (Michael KZ4LY, 2026-11-03): "going all the way
 * to 0 to let my tuner work, and then click a lot of times to get back to a
 * reasonable speed, is inconvenient." Tap the WPM label (now clickable) for
 * a small popup, clamped to CW_WPM_MIN..CW_WPM_MAX on confirm.
 *
 * ⛔ NOT lv_keyboard(), AFTER TRYING IT. LVGL's own keyboard widget defaults
 * to LV_PCT(100) x LV_PCT(50) docked to the bottom of its parent, and even
 * explicitly resized it still carries LVGL's stock theme - flat grey keys,
 * default font, nothing like the rest of this page. Steffen OZ1LAV,
 * 2026-11-03, after seeing it sized correctly but still wrong: "it is
 * totally off in style, colour, font size - like EVERYTHING is off...
 * compare it to the macro edit window, the on-screen keyb i designed."
 *
 * So this is hand-built from the same lv_btn recipe as the macro editor's
 * own buttons right above (0x2a2f37 fill, UI_COLOR_BORDER, radius 8,
 * montserrat_28, CW-blue digits) - no LVGL stock widget, no theme
 * mismatch possible. A plain char buffer backs the display instead of a
 * textarea, since nothing here needs text-input focus/cursor machinery for
 * two digits. */
static char s_wpm_buf[3] = {0};   /* up to 2 digits + NUL */
static lv_obj_t *s_wpm_disp = NULL;

static void wpm_disp_refresh(void)
{
    if (s_wpm_disp) lv_label_set_text(s_wpm_disp, s_wpm_buf[0] ? s_wpm_buf : "--");
}

static void wpm_popup_close(void)
{
    if (s_wpm_panel) lv_obj_add_flag(s_wpm_panel, LV_OBJ_FLAG_HIDDEN);
}

static void wpm_popup_apply(void)
{
    int v = s_wpm_buf[0] ? atoi(s_wpm_buf) : 0;
    if (v < CW_WPM_MIN) v = CW_WPM_MIN;
    if (v > CW_WPM_MAX) v = CW_WPM_MAX;
    cat_request_keyer_wpm((uint8_t)v);
    if (s_lbl_wpm) lv_label_set_text_fmt(s_lbl_wpm, "%d WPM", v);
    wpm_popup_close();
}

static void wpm_digit_cb(lv_event_t *e)
{
    size_t len = strlen(s_wpm_buf);
    if (len >= 2) return;   // CW_WPM_MAX is two digits - a third can never be valid
    char d = (char)(intptr_t)lv_event_get_user_data(e);
    s_wpm_buf[len] = d;
    s_wpm_buf[len + 1] = '\0';
    wpm_disp_refresh();
}

static void wpm_backspace_cb(lv_event_t *e)
{
    (void)e;
    size_t len = strlen(s_wpm_buf);
    if (len > 0) s_wpm_buf[len - 1] = '\0';
    wpm_disp_refresh();
}

static void wpm_tick_cb(lv_event_t *e) { (void)e; wpm_popup_apply(); }

static void wpm_popup_backdrop_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_user_data(e)) wpm_popup_apply();
}

/* One small helper so all nine digit/backspace/OK/Cancel keys share exactly
 * the macro editor's own button recipe - same colours, same font, same
 * radius, by construction rather than by copying numbers into four places. */
static lv_obj_t *wpm_key_make(lv_obj_t *parent, int x, int y, int w, int h,
                              const char *txt, uint32_t bg, uint32_t fg)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl);
    return b;
}

static void wpm_popup_open(void)
{
    if (!s_wpm_panel) {
        s_wpm_panel = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_wpm_panel, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_wpm_panel, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_wpm_panel, LV_OPA_60, 0);
        lv_obj_set_style_border_width(s_wpm_panel, 0, 0);
        lv_obj_set_style_radius(s_wpm_panel, 0, 0);
        lv_obj_set_style_pad_all(s_wpm_panel, 0, 0);
        lv_obj_clear_flag(s_wpm_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_wpm_panel, wpm_popup_backdrop_cb, LV_EVENT_CLICKED, s_wpm_panel);

        // Box, digit grid and Cancel/OK sized from the same numbers the
        // macro editor already uses (56 px key height, 8 px gap) - a 3-wide
        // keypad plus a bottom row, not a copy of that window's own layout.
        int box_w = 266, box_h = 412;
        lv_obj_t *box = lv_obj_create(s_wpm_panel);
        lv_obj_set_size(box, box_w, box_h);
        lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_pad_all(box, 12, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        s_wpm_disp = lv_label_create(box);
        lv_obj_set_size(s_wpm_disp, box_w - 24, 50);
        lv_obj_set_pos(s_wpm_disp, 0, 0);
        lv_obj_set_style_text_color(s_wpm_disp, lv_color_hex(0xFFA040), 0);  // the page's own WPM-label colour
        lv_obj_set_style_text_font(s_wpm_disp, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_align(s_wpm_disp, LV_TEXT_ALIGN_CENTER, 0);

        // Row 4 is tick / 0 / backspace - no separate Cancel/OK row. A tap
        // outside the pad (the backdrop) applies the entered value too, so
        // the tick is a shortcut for that, not the only way to confirm.
        static const char *const DIGIT_ROWS[3][3] = {
            { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }
        };
        int cell = 74, gap = 10, grid_top = 62;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                const char *t = DIGIT_ROWS[r][c];
                lv_obj_t *k = wpm_key_make(box, c * (cell + gap), grid_top + r * (cell + gap),
                                           cell, cell, t, 0x2a2f37, UI_COLOR_MODE_CW_TEXT);
                lv_obj_add_event_cb(k, wpm_digit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t[0]);
            }
        }
        int row4_y = grid_top + 3 * (cell + gap);
        lv_obj_t *tick = wpm_key_make(box, 0, row4_y, cell, cell, LV_SYMBOL_OK,
                                      0x2e8b3a, 0xffffff);
        lv_obj_add_event_cb(tick, wpm_tick_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *zero = wpm_key_make(box, cell + gap, row4_y, cell, cell, "0",
                                      0x2a2f37, UI_COLOR_MODE_CW_TEXT);
        lv_obj_add_event_cb(zero, wpm_digit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)'0');
        lv_obj_t *bksp = wpm_key_make(box, (cell + gap) * 2, row4_y, cell, cell, LV_SYMBOL_BACKSPACE,
                                      0x2a2f37, 0xffffff);
        lv_obj_add_event_cb(bksp, wpm_backspace_cb, LV_EVENT_CLICKED, NULL);
    }

    int cur = cat_get_keyer_wpm();
    if (cur <= 0) cur = CW_WPM_FALLBACK;
    char curbuf[12];
    snprintf(curbuf, sizeof(curbuf), "%d", cur);
    strncpy(s_wpm_buf, curbuf, sizeof(s_wpm_buf) - 1);   // CW_WPM_MAX is two digits; a wider readback just truncates display, never crashes
    s_wpm_buf[sizeof(s_wpm_buf) - 1] = '\0';
    wpm_disp_refresh();

    lv_obj_clear_flag(s_wpm_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wpm_panel);
}

static void wpm_label_clicked_cb(lv_event_t *e) { (void)e; wpm_popup_open(); }

/* Set a ticker label only if the text actually differs - see the call sites.
 * lv_label_set_text() always copies and invalidates, so an unchanged row costs
 * a repaint for nothing. */
static void tk_set(lv_obj_t *lbl, const char *txt)
{
    if (!lbl) return;
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(lbl, txt);
}

// ---- 500 ms refresh: ticker text, WPM readback, TX status -------------------
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_shown) return;

    /* Draw cw_decode.c's grid - the same one the panadapter and the browser
     * draw. Three disjoint layers per row; see where the labels are declared.
     * The boundary between `now` and `prev` IS the cursor, which is why no
     * cursor glyph is needed (Samuel W7STF's problem, the operator's answer). */
    {
        const int rows = cw_decode_rows();
        const int cols = cw_decode_cols();
        char    disp[CW_GRID_CELLS + 1];
        uint8_t txmask[CW_GRID_CELLS] = {0};
        cw_decode_line(disp, sizeof(disp));
        cw_decode_line_tx(txmask, sizeof(txmask));
        int col = cw_decode_line_col();
        if (col < 0) col = 0;
        if (col > rows * cols) col = rows * cols;

        for (int r = 0; r < CW_LINE_ROWS; r++) {
            if (r >= rows) {
                /* Rows the operator chose not to have (Bryan N0LUF's 2-or-4
                 * setting) are hidden, not blanked - a blank label still
                 * reserves its line. */
                if (s_tk_now[r])  lv_obj_add_flag(s_tk_now[r],  LV_OBJ_FLAG_HIDDEN);
                if (s_tk_prev[r]) lv_obj_add_flag(s_tk_prev[r], LV_OBJ_FLAG_HIDDEN);
                if (s_tk_tx[r])   lv_obj_add_flag(s_tk_tx[r],   LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            char now_s[CW_LINE_COLS + 1], prev_s[CW_LINE_COLS + 1], tx_s[CW_LINE_COLS + 1];
            for (int i = 0; i < cols; i++) {
                const int  g  = r * cols + i;
                const char c  = disp[g] ? disp[g] : ' ';
                /* TX wins the cell and is blanked from the other two, so the
                 * three layers stay disjoint. */
                const bool tx = (g < CW_GRID_CELLS) && txmask[g];
                now_s[i]  = (!tx && g <  col) ? c : ' ';
                prev_s[i] = (!tx && g >= col) ? c : ' ';
                tx_s[i]   = ( tx)             ? c : ' ';
            }
            now_s[cols] = prev_s[cols] = tx_s[cols] = '\0';
            /* ⛔ ONLY WHEN IT CHANGED, and this is measured rather than tidy.
             * The first cut set all twelve labels every 500 ms whether the
             * grid had moved or not - the old single-label code only painted
             * `if (dirty)` and I dropped that guard with the label. On a quiet
             * band the ticker is unchanged for most ticks, so that was twelve
             * needless invalidations a second holding the display lock, and
             * "ui_update_frequency: freq label lock timeout" went from 1 per
             * 151-278 s on this page to 1 per 53 s. Comparing against the
             * label's own stored text needs no cache of our own. */
            tk_set(s_tk_now[r],  now_s);
            tk_set(s_tk_prev[r], prev_s);
            tk_set(s_tk_tx[r],   tx_s);
        }
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
    // park), then the ACTIONS (NEXT, memories), status, then the macro set
    // toggle at the very bottom.
    //
    // Macro set toggle (decided 2026-11-02, see
    // memory/project_cw_macro_set_selector_decided.md): originally placed at
    // the TOP, between the header and WPM. Moved to the BOTTOM 2026-11-03
    // (operator): "clearance space above them and then comes all the
    // macros" - a toggle sitting right above six buttons that fire a
    // transmission risked a mis-hit landing on the wrong macro SET instead
    // of a wrong macro. Now it is the last thing in the pane, anchored to
    // MID_H (see toggle_y below) with an 8 px gap below the status line's
    // fixed height - the pane has no more slack than that; verify on the
    // glass same as the top placement needed.
#define SET_ROW_H    30

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
    // Tap the number itself for direct numeric entry - see wpm_popup_open().
    lv_obj_add_flag(s_lbl_wpm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_wpm, wpm_label_clicked_cb, LV_EVENT_CLICKED, NULL);

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

    // Status line. Fixed height (2 lines of montserrat_20, line_height 22 ->
    // 44 px) rather than letting LONG_WRAP grow it: the toggle row below is
    // anchored to the pane's bottom edge, so this must never be able to push
    // into it - a message that somehow ran past 2 lines clips instead of
    // overlapping the toggle.
    s_lbl_status = lv_label_create(s_pane);
    lv_label_set_text(s_lbl_status, "");
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, PANE_W - 20);
    lv_obj_set_height(s_lbl_status, 44);
    lv_obj_set_pos(s_lbl_status, 10, y + 4);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);

    // Macro set toggle (General/Contest) - moved to the very bottom of the
    // pane 2026-11-03, see the note above the WPM row for why. Anchored to
    // MID_H so it always sits flush with the pane's bottom edge regardless
    // of future changes above it. With the status line's fixed 44 px height
    // this leaves an 8 px gap above the toggle - the pane's entire remaining
    // slack; the macro buttons themselves end 56 px further up still.
    {
        int toggle_y = MID_H - SET_ROW_H;
        int halfw = (PANE_W - 30) / 2;   // 10 margin each side, 10 gap between
        s_btn_set_a = make_pane_btn(s_pane, 10, toggle_y, halfw, SET_ROW_H, 0x2a2f37, 0x555555);
        lv_obj_add_event_cb(s_btn_set_a, set_toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
        // "POTA" renamed to "General" 2026-11-02 (operator): the set is not
        // POTA-specific - M2's default text just happens to say "CQ POTA",
        // which the operator can still edit - and POTA is one of several
        // activation programmes (SOTA, IOTA, WWFF...), so the label should
        // not single one out.
        s_lbl_set_a = lv_label_create(s_btn_set_a);
        lv_label_set_text(s_lbl_set_a, "General");
        lv_obj_set_style_text_color(s_lbl_set_a, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(s_lbl_set_a, &lv_font_montserrat_20, 0);
        lv_obj_center(s_lbl_set_a);

        s_btn_set_b = make_pane_btn(s_pane, 10 + halfw + 10, toggle_y, halfw, SET_ROW_H, 0x2a2f37, 0x555555);
        lv_obj_add_event_cb(s_btn_set_b, set_toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
        s_lbl_set_b = lv_label_create(s_btn_set_b);
        lv_label_set_text(s_lbl_set_b, "Contest");
        lv_obj_set_style_text_color(s_lbl_set_b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(s_lbl_set_b, &lv_font_montserrat_20, 0);
        lv_obj_center(s_lbl_set_b);
    }

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

    /* Row pitch MEASURED from the font, not guessed - the same arithmetic
     * ui.c's row_px uses. Sizing a box from the rows around it instead of from
     * the text it holds is what clipped the drawer captions on 2026-11-02. */
    const int tk_row_px = (int)lv_font_get_line_height(&qmx_mono_25) + 2;
    for (int r = 0; r < CW_LINE_ROWS; r++) {
        struct { lv_obj_t **obj; uint32_t colour; } layer[] = {
            { &s_tk_now[r],  0x30E0E0 },            /* this pass, cyan */
            { &s_tk_prev[r], 0x30E060 },            /* previous pass, green */
            { &s_tk_tx[r],   UI_COLOR_TX_ACTIVE },  /* our own keying, orange */
        };
        for (size_t i = 0; i < sizeof(layer) / sizeof(layer[0]); i++) {
            lv_obj_t *l = lv_label_create(s_ticker);
            *layer[i].obj = l;
            lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(l, MID_W - PANE_W - 16);
            lv_obj_set_style_text_font(l, &qmx_mono_25, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(layer[i].colour), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(l, 0, 0);
            lv_label_set_text(l, "");
            lv_obj_set_pos(l, 0, r * tk_row_px);
        }
    }

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
    refresh_set_toggle();
    refresh_mem_labels();
    refresh_next_label();
    cat_query_keyer_wpm();
    cat_cw_poll_set_active(true);
    if (!s_timer) s_timer = lv_timer_create(tick_cb, 500, NULL);
    /* ⛔ THE GRID IS NARROWER HERE, and the decoder has to be told before the
     * first character lands. This ticker sits to the right of a PANE_W pane, so
     * it is MID_W - PANE_W = 960 px, which is CW_TICKER_COLS of qmx_mono_25 at
     * its 15 px advance - against the panadapter's 72 across the full 1280.
     * cw_decode_set_cols() is the wrap modulus, so a mismatch does not clip, it
     * GARBLES: every row would start mid-word. Restored in _hide(). */
    cw_decode_set_cols(CW_TICKER_COLS);
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
    /* Hand the full width back, or the panadapter draws a 64-column grid in a
     * 72-column frame for the rest of the session. */
    cw_decode_set_cols(CW_LINE_COLS);
    cat_cw_poll_set_active(false);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    edit_close();
    park_close();
}
