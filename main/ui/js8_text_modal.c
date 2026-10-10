// JS8 free-text composer.
//
// Structurally based on ft8_filter_modal.c (modal + textarea + on-screen
// keyboard), reduced to one field, because that is all this needs.
//
// ⭐ WHY IT EXISTS. JS8 free-text transmit shipped working but with no surface:
// the only way to send a message was POST /api/cmd {"action":"js8_text"}. The
// operator's verdict was the right one - "how can users be happy for an api?"
//
// ⭐ ALL THREE KEYBOARDS REACH IT, and none of that is new code here. The
// snap-on keyboard and a BLE keyboard both arrive through ui_kbd_note_focus(),
// and ui_osk_show() puts up the on-screen keyboard only when no BLE keyboard is
// connected - see the long comment at ui_osk_show() in ui.c.
//
// ⭐ THE AIR TIME IS SHOWN BEFORE THE RADIO IS KEYED. js8_ftx_plan() answers
// "how many frames, how many seconds", and also returns the NORMALISED text -
// upper-cased, whitespace collapsed, unsendable characters dropped. The
// operator sees exactly what will go out, from the encoder itself rather than a
// second copy of its rules here. A 12-frame message is three minutes of holding
// a frequency and is not something to discover afterwards.

// ⛔ EVERY COLOUR AND EVERY FONT COMES FROM ui_theme.h. The first cut used bare
// hex (0x101010 panel, 0x909090 plan text) and LVGL's default 14 px font for
// the title, the field and the buttons, and it was rejected on sight. The
// readers of this display are mostly over 60: 14 px body text on a 5" panel at
// arm's length is not readable. montserrat_24 for body, 28 for the title, and
// the panel/button geometry follows from that, not the other way round.
//
#include "js8_text_modal.h"
#include "ft8_screen_view.h"
#include "js8_ftx.h"
#include "ui_theme.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "js8_text_modal";

static lv_obj_t *s_modal    = NULL;
static lv_obj_t *s_panel    = NULL;
static lv_obj_t *s_ta       = NULL;
static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_plan_lbl = NULL;
static lv_obj_t *s_btn_send = NULL;
static bool      s_open     = false;

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

/* Re-plan on every edit. A local call - no network, no radio - so it is cheap
 * enough per keystroke and there is nothing to debounce. */
static void plan_refresh(void)
{
    if (!s_plan_lbl || !s_ta) return;
    const char *txt = lv_textarea_get_text(s_ta);
    int frames = 0, secs = 0;
    char norm[256];

    if (!txt || !txt[0]) {
        lv_label_set_text(s_plan_lbl, "");
        if (s_btn_send) lv_obj_add_state(s_btn_send, LV_STATE_DISABLED);
        return;
    }
    if (!js8_ftx_plan(txt, &frames, &secs, norm, sizeof(norm))) {
        lv_label_set_text(s_plan_lbl, "Nothing sendable in that.");
        if (s_btn_send) lv_obj_add_state(s_btn_send, LV_STATE_DISABLED);
        return;
    }
    if (s_btn_send) lv_obj_clear_state(s_btn_send, LV_STATE_DISABLED);

    char line[320];
    if (secs >= 60)
        snprintf(line, sizeof(line), "%d frame%s - %d s on air (%d min %d s)\n%s",
                 frames, frames == 1 ? "" : "s", secs, secs / 60, secs % 60, norm);
    else
        snprintf(line, sizeof(line), "%d frame%s - %d s on air\n%s",
                 frames, frames == 1 ? "" : "s", secs, norm);
    lv_label_set_text(s_plan_lbl, line);
}

static void ta_changed_cb(lv_event_t *e)
{
    (void)e;
    plan_refresh();
}

/* ⛔ WIRED TO BOTH FOCUSED AND CLICKED, and that is not belt-and-braces - see
 * the comment at ui_osk_show() in ui.c. LVGL sends FOCUSED only when an object
 * GAINS focus, so tapping the same field a second time sends nothing and the
 * keyboard never comes back. Samuel W7STF lost two dialogs to exactly that. */
static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!s_keyboard) return;
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, ta);
    ui_osk_show(s_keyboard);
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ta) return;
    const char *txt = lv_textarea_get_text(s_ta);
    if (!txt || !txt[0]) return;

    /* Hand to the view, which defers to the LVGL task exactly as the web action
     * does - js8_ftx_start() encodes and arms, and ft8_tx_arm() blocks briefly,
     * so neither may run from a button callback. */
    ft8_screen_view_request_freetext(txt);
    ESP_LOGI(TAG, "free text queued from the composer (%d chars)", (int)strlen(txt));
    modal_close();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

/* ⛔ Stops a run that is ALREADY GOING OUT, and is deliberately reachable while
 * transmitting: a run holds the frequency for up to three minutes. */
static void stop_cb(lv_event_t *e)
{
    (void)e;
    js8_ftx_cancel();
    ui_toast("JS8 free text cancelled");
    modal_close();
}

/* The keyboard's own tick and cross. READY is the tick - the operator has
 * finished typing, and in a one-field dialog that means send. */
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)       send_cb(NULL);
    else if (code == LV_EVENT_CANCEL) modal_close();
}

static void modal_build(void)
{
    if (s_modal) return;

    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    /* Top-aligned, not centred: the on-screen keyboard owns the bottom 45% of a
     * 720 px screen, and a centred panel ends up behind it. */
    s_panel = lv_obj_create(s_modal);
    /* 900x340, and both numbers follow from the font. At montserrat_24 the
     * three 180 px buttons, the plan label's three wrapped lines and the
     * textarea do not fit the 760x300 the first cut used. */
    lv_obj_set_size(s_panel, 900, 340);
    /* y=70 clears the top bar (band/mode/freq/S-meter); at 24 the title row
     * was drawn behind it. 340 tall ends at 410, above the 280 px keyboard
     * that starts at 440. */
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "JS8 free text");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ta = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta, LV_PCT(100), 72);
    lv_obj_align(s_ta, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_textarea_set_placeholder_text(s_ta, "Message to send...");
    lv_textarea_set_one_line(s_ta, false);
    lv_textarea_set_max_length(s_ta, 160);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_24, 0);
    /* Also registers ui_theme_ta_kbd_focus_cb, which is what records this field
     * as the physical keyboards' target - so ui_kbd_note_focus() is NOT called
     * by hand here. One owner for that bookkeeping. */
    ui_theme_style_textarea(s_ta);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta, ta_focused_cb, LV_EVENT_CLICKED, NULL);  /* see ui_osk_show() */
    lv_obj_add_event_cb(s_ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_plan_lbl = lv_label_create(s_panel);
    lv_label_set_long_mode(s_plan_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_plan_lbl, LV_PCT(100));
    lv_obj_align(s_plan_lbl, LV_ALIGN_TOP_LEFT, 0, 128);
    lv_obj_set_style_text_font(s_plan_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_plan_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_label_set_text(s_plan_lbl, "");

    /* ⛔ Stop TX is NOT the Cancel red. Cancel is UI_COLOR_DANGER in every
     * other modal, and two identical red buttons side by side on a dialog that
     * can key the radio is the wrong thing to hand a hurried operator.
     * UI_COLOR_TX_ACTIVE is the colour transmit already has on this display. */
    struct { const char *lbl; uint32_t col; lv_event_cb_t cb; lv_align_t al; int dx; } btns[3] = {
        { "Send",    UI_COLOR_SUCCESS,   send_cb,   LV_ALIGN_BOTTOM_RIGHT, 0    },
        { "Cancel",  UI_COLOR_DANGER,    cancel_cb, LV_ALIGN_BOTTOM_RIGHT, -196 },
        { "Stop TX", UI_COLOR_TX_ACTIVE, stop_cb,   LV_ALIGN_BOTTOM_LEFT,  0    },
    };
    lv_obj_t *btn_cancel = NULL;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(s_panel);
        lv_obj_set_size(b, 180, 64);
        lv_obj_align(b, btns[i].al, btns[i].dx, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        /* An explicit bg_color survives LV_STATE_DISABLED, so a greyed-out
         * Send would still be green without this. */
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_BT_OFF), LV_STATE_DISABLED);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].lbl);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_STATE_DISABLED);
        lv_obj_center(l);
        if (i == 0)      s_btn_send = b;
        else if (i == 1) btn_cancel = b;
    }
    lv_obj_add_state(s_btn_send, LV_STATE_DISABLED);

    /* Enter sends, Esc closes. The registry picks whichever modal is ACTUALLY
     * on screen, so registering once at build time is correct - see the
     * comment on the registry in ui.c. */
    ui_kbd_set_buttons(s_btn_send, btn_cancel);

    /* ⛔ THE SAME KEYBOARD AS EVERY OTHER MODAL. A bare lv_keyboard_create()
     * gets LVGL's default white keys at the default font, which is what shipped
     * to the bench and was immediately spotted. The project's look is three
     * calls - the key style, ui_theme_style_keyboard() and the 28 px font - and
     * they are copied here from ft8_filter_modal.c rather than reinvented. */
    s_keyboard = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_hex(UI_COLOR_TEXT));
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(UI_COLOR_BORDER));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_keyboard, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_keyboard);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Start in caps: JS8 free text goes on the air upper-cased anyway, so
     * showing lower-case keys would misrepresent what is being typed. */
    ui_theme_keyboard_attach_caps_cycle_upper(s_keyboard);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
}

void js8_text_modal_init(void)
{
    modal_build();
}

void js8_text_modal_show(void)
{
    modal_build();
    if (s_open) return;

    lv_textarea_set_text(s_ta, "");
    plan_refresh();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;

    /* ⛔ NO PRE-FOCUS, AND NO KEYBOARD, UNTIL THE FIELD IS TAPPED. I opened with
     * both - "making them tap the field first is a wasted tap" - which is the
     * exact reasoning the tombstone in ui_theme.h was written to kill. A cursor
     * before the operator has chosen anything cannot mean "type here", and
     * add_state(FOCUSED) paints a cursor without sending LV_EVENT_FOCUSED, so
     * the blinking field need not even be the one receiving keystrokes. The tap
     * raises the keyboard through ta_focused_cb. */
}
