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
    if (s_ta) ui_kbd_note_unfocus(s_ta);
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
    ui_kbd_note_focus(ta);
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
    lv_obj_set_size(s_panel, 760, 300);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_radius(s_panel, 8, 0);
    lv_obj_set_style_pad_all(s_panel, 16, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "JS8 free text");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ta = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta, LV_PCT(100), 96);
    lv_obj_align(s_ta, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_textarea_set_placeholder_text(s_ta, "Message to send...");
    lv_textarea_set_one_line(s_ta, false);
    lv_textarea_set_max_length(s_ta, 160);
    lv_obj_add_event_cb(s_ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta, ta_focused_cb, LV_EVENT_CLICKED, NULL);  /* see ui_osk_show() */
    lv_obj_add_event_cb(s_ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_plan_lbl = lv_label_create(s_panel);
    lv_label_set_long_mode(s_plan_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_plan_lbl, LV_PCT(100));
    lv_obj_align(s_plan_lbl, LV_ALIGN_TOP_LEFT, 0, 138);
    lv_obj_set_style_text_color(s_plan_lbl, lv_color_hex(0x909090), 0);
    lv_label_set_text(s_plan_lbl, "");

    s_btn_send = lv_button_create(s_panel);
    lv_obj_set_size(s_btn_send, 150, 48);
    lv_obj_align(s_btn_send, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(s_btn_send, send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(s_btn_send);
    lv_label_set_text(l, "Send");
    lv_obj_center(l);
    lv_obj_add_state(s_btn_send, LV_STATE_DISABLED);

    lv_obj_t *btn_cancel = lv_button_create(s_panel);
    lv_obj_set_size(btn_cancel, 150, 48);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -164, 0);
    lv_obj_add_event_cb(btn_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(btn_cancel);
    lv_label_set_text(l, "Cancel");
    lv_obj_center(l);

    lv_obj_t *btn_stop = lv_button_create(s_panel);
    lv_obj_set_size(btn_stop, 150, 48);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(btn_stop, stop_cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(btn_stop);
    lv_label_set_text(l, "Stop TX");
    lv_obj_center(l);

    /* Enter sends, Esc closes. The registry picks whichever modal is ACTUALLY
     * on screen, so registering once at build time is correct - see the
     * comment on the registry in ui.c. */
    ui_kbd_set_buttons(s_btn_send, btn_cancel);

    s_keyboard = lv_keyboard_create(s_modal);
    lv_obj_set_size(s_keyboard, LV_PCT(100), LV_PCT(45));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
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

    /* Open with the keyboard already up: the one thing the operator came here to
     * do is type, so making them tap the field first is a wasted tap. */
    ui_kbd_note_focus(s_ta);
    lv_keyboard_set_textarea(s_keyboard, s_ta);
    ui_osk_show(s_keyboard);
}
