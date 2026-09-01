// Activation session editor - see activation_modal.h.
//
// Two things about this screen are deliberate and worth keeping:
//
// 1. STOPPING is as prominent as starting. A forgotten activation is the
//    failure mode that actually happens: the operator drives home and every
//    subsequent QSO is logged as if still in the park, which quietly corrupts
//    the log for POTA, the chasers, and LoTW alike. So when a session is
//    running the primary button is a red "Stop activation", not a save.
//
// 2. The contact count is shown, because it is the number an activator is
//    counting in the field (POTA wants 10). It comes from the log itself
//    rather than a counter we keep, so it survives a reboot and cannot drift
//    out of step with what was actually written.

#include "activation_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "storage/settings.h"
#include "adif/adif_log.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "activation";

static lv_obj_t *s_modal   = NULL;
static lv_obj_t *s_panel   = NULL;
static lv_obj_t *s_ta_ref  = NULL;
static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_btn_pota = NULL, *s_btn_sota = NULL;
static lv_obj_t *s_lbl_count = NULL;
static lv_obj_t *s_btn_go = NULL, *s_lbl_go = NULL;
static uint8_t   s_sel_type = 1;    // editing state: 1 POTA, 2 SOTA

bool activation_is_running(void) { return settings_get_activation_type() != 0; }

bool activation_describe(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    const char *sig = settings_activation_sig_name();
    char ref[16];
    if (!sig || !settings_get_activation_ref(ref, sizeof(ref))) return false;
    snprintf(out, out_sz, "%s %s", sig, ref);
    return true;
}

static void modal_close(void)
{
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void style_type_btn(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(on ? UI_COLOR_PRIMARY : 0x333a42), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(on ? 0xFFFFFF : 0x555555), 0);
    lv_obj_set_style_border_width(btn, on ? 3 : 2, 0);
}

// Reflects the LIVE session, not the edit state: the count and the primary
// button describe what is actually running right now.
static void refresh(void)
{
    style_type_btn(s_btn_pota, s_sel_type == 1);
    style_type_btn(s_btn_sota, s_sel_type == 2);

    char running[32];
    bool on = activation_describe(running, sizeof(running));

    if (s_lbl_count) {
        if (on) {
            char ref[16];
            settings_get_activation_ref(ref, sizeof(ref));
            int n = adif_log_count_activation(ref);
            char b[96];
            // POTA's threshold is 10; SOTA's is 4. Showing the target turns a
            // bare number into an answer to "can I go home yet".
            int target = (settings_get_activation_type() == 2) ? 4 : 10;
            snprintf(b, sizeof(b), "Running: %s\n%d contact%s logged (need %d)",
                     running, n, n == 1 ? "" : "s", target);
            lv_label_set_text(s_lbl_count, b);
            lv_obj_set_style_text_color(s_lbl_count,
                                        lv_color_hex(n >= target ? 0x60D060 : 0xFFC864), 0);
        } else {
            lv_label_set_text(s_lbl_count, "No activation running.\nQSOs log normally.");
            lv_obj_set_style_text_color(s_lbl_count, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        }
    }

    if (s_lbl_go && s_btn_go) {
        lv_label_set_text(s_lbl_go, on ? "Stop activation" : "Start activation");
        lv_obj_set_style_bg_color(s_btn_go, lv_color_hex(on ? 0x962020 : 0x2e8b3a), 0);
        lv_obj_set_style_border_color(s_btn_go, lv_color_hex(on ? 0xc04040 : 0x4caf50), 0);
    }
}

static void type_btn_cb(lv_event_t *e)
{
    s_sel_type = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    refresh();
}

static void go_btn_cb(lv_event_t *e)
{
    (void)e;
    if (activation_is_running()) {
        char was[32];
        activation_describe(was, sizeof(was));
        settings_set_activation(0, NULL);
        ESP_LOGI(TAG, "activation stopped (was %s)", was);
        ui_toast("Activation stopped");
        refresh();
        return;
    }
    const char *ref = s_ta_ref ? lv_textarea_get_text(s_ta_ref) : NULL;
    if (!ref || !ref[0]) {
        ui_toast("Enter the park or summit reference first");
        return;
    }
    settings_set_activation(s_sel_type, ref);
    if (!activation_is_running()) {          // rejected (empty after trimming)
        ui_toast("That reference is not usable");
        return;
    }
    char now[32];
    activation_describe(now, sizeof(now));
    ESP_LOGI(TAG, "activation started: %s", now);
    char t[64];
    snprintf(t, sizeof(t), "Activating %s", now);
    ui_toast(t);
    refresh();
}

static void cancel_btn_cb(lv_event_t *e) { (void)e; modal_close(); }

static void ta_focused_cb(lv_event_t *e)
{
    if (!s_keyboard) return;
    lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(e));
    ui_osk_show(s_keyboard);
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int w, int h,
                          lv_align_t align, int x, int y,
                          lv_event_cb_t cb, void *ud, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, align, x, y);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
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

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 880, 420);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Activation");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    s_btn_pota = make_btn(s_panel, "POTA", 180, 60, LV_ALIGN_TOP_LEFT, 0, 52,
                          type_btn_cb, (void *)(uintptr_t)1, NULL);
    s_btn_sota = make_btn(s_panel, "SOTA", 180, 60, LV_ALIGN_TOP_LEFT, 196, 52,
                          type_btn_cb, (void *)(uintptr_t)2, NULL);

    s_ta_ref = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_ref, 420, 60);
    lv_obj_align(s_ta_ref, LV_ALIGN_TOP_LEFT, 400, 52);
    lv_textarea_set_one_line(s_ta_ref, true);
    lv_textarea_set_max_length(s_ta_ref, 15);
    lv_textarea_set_placeholder_text(s_ta_ref, "DL-0123  /  OZ/SJ-001");
    lv_obj_set_style_text_font(s_ta_ref, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_ref);
    lv_obj_add_event_cb(s_ta_ref, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    s_lbl_count = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_count, LV_ALIGN_TOP_LEFT, 0, 136);
    lv_label_set_text(s_lbl_count, "");

    lv_obj_t *cancel_btn = make_btn(s_panel, "Close", 200, 72,
                                    LV_ALIGN_BOTTOM_LEFT, 40, 0, cancel_btn_cb, NULL, NULL);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x3a4149), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x5a6169), 0);

    s_btn_go = make_btn(s_panel, "Start activation", 340, 72,
                        LV_ALIGN_BOTTOM_RIGHT, -40, 0, go_btn_cb, NULL, &s_lbl_go);

    // Physical keyboard: Enter -> the primary action, Esc -> close.
    ui_kbd_set_buttons(s_btn_go, cancel_btn);

    s_keyboard = lv_keyboard_create(s_modal);
    ui_theme_style_keyboard(s_keyboard);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_theme_keyboard_attach_caps_cycle_upper(s_keyboard);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Activation modal built");
}

void activation_modal_init(void) { modal_build(); }

void activation_modal_show(void)
{
    modal_build();
    // Pre-fill from the running session so stopping and restarting the same
    // reference does not mean re-typing it, and so the type buttons agree with
    // what is actually running.
    char ref[16];
    if (settings_get_activation_ref(ref, sizeof(ref))) {
        lv_textarea_set_text(s_ta_ref, ref);
        s_sel_type = settings_get_activation_type();
    }
    refresh();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}
