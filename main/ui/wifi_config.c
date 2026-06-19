// WiFi configuration modal - full-screen overlay for entering SSID/password.
// On Save: persists to NVS via panadapter_wifi_reconnect() and triggers a
// disconnect/reconnect cycle. On Cancel: closes without changes.

#include "wifi_config.h"
#include "ui_theme.h"
#include "wifi.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_config";

// Modal state - lazily created on first show.
static lv_obj_t *s_modal       = NULL;  // root full-screen overlay
static lv_obj_t *s_panel       = NULL;  // centred dialog panel
static lv_obj_t *s_ta_ssid       = NULL;
static lv_obj_t *s_ta_pass       = NULL;
static lv_obj_t *s_cb_show_pass  = NULL;  // "Show password" checkbox
static lv_obj_t *s_keyboard      = NULL;
static bool      s_modal_open  = false;
static void    (*s_on_close)(void) = NULL;  // one-shot, fired when the modal closes

static void modal_close(void)
{
    if (!s_modal || !s_modal_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_modal_open = false;
    ESP_LOGI(TAG, "Modal closed");
    if (s_on_close) {
        void (*cb)(void) = s_on_close;
        s_on_close = NULL;   // fire once, whether closed via Save or Cancel
        cb();
    }
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(s_ta_ssid);
    const char *pass = lv_textarea_get_text(s_ta_pass);
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "SSID empty, ignoring Save");
        return;
    }
    ESP_LOGI(TAG, "Save: SSID='%s' (pass %d chars)", ssid, (int)strlen(pass));
    panadapter_wifi_reconnect(ssid, pass);
    modal_close();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void show_pass_cb(lv_event_t *e)
{
    bool show = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_ta_pass, !show);
}

// Show keyboard on textarea focus, attach to the focused textarea.
static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (!s_keyboard) return;

    // Only one field shows the blinking cursor at a time.
    lv_obj_t *other = (ta == s_ta_ssid) ? s_ta_pass : s_ta_ssid;
    lv_obj_remove_state(other, LV_STATE_FOCUSED);

    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

// Hide keyboard when user presses LV_KEY_ESC or LV_KEY_ENTER on the keyboard.
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

// Build the modal once. Hidden initially.
static void modal_build(void)
{
    if (s_modal) return;

    lv_obj_t *scr = lv_screen_active();

    // Full-screen dimmed overlay (modal backdrop)
    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    // Centred dialog panel - taller now to fit larger fonts
    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 880, 420);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title - larger, brighter
    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "WiFi Configuration");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // SSID label - larger
    lv_obj_t *ssid_lbl = lv_label_create(s_panel);
    lv_label_set_text(ssid_lbl, "SSID");
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 0, 56);

    // SSID textarea - larger font + taller
    s_ta_ssid = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_ssid, 820, 60);
    lv_obj_align(s_ta_ssid, LV_ALIGN_TOP_LEFT, 0, 86);
    lv_textarea_set_one_line(s_ta_ssid, true);
    lv_textarea_set_max_length(s_ta_ssid, 32);
    lv_textarea_set_placeholder_text(s_ta_ssid, "Network name");
    lv_obj_set_style_text_font(s_ta_ssid, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_ssid);
    lv_obj_add_event_cb(s_ta_ssid, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // Password label - larger
    lv_obj_t *pass_lbl = lv_label_create(s_panel);
    lv_label_set_text(pass_lbl, "Password");
    lv_obj_set_style_text_color(pass_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(pass_lbl, LV_ALIGN_TOP_LEFT, 0, 160);

    // Password textarea - full width, masked
    s_ta_pass = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_pass, 820, 60);
    lv_obj_align(s_ta_pass, LV_ALIGN_TOP_LEFT, 0, 190);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_max_length(s_ta_pass, 64);
    lv_textarea_set_placeholder_text(s_ta_pass, "WPA2 password (leave empty for open network)");
    lv_obj_set_style_text_font(s_ta_pass, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_pass);
    lv_obj_add_event_cb(s_ta_pass, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // "Show password" checkbox below the password field.
    static lv_style_t s_ind, s_ind_chk;
    static bool show_styles_inited = false;
    if (!show_styles_inited) {
        lv_style_init(&s_ind);
        lv_style_set_bg_color(&s_ind, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_border_color(&s_ind, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&s_ind, 2);
        lv_style_set_pad_all(&s_ind, 6);
        lv_style_init(&s_ind_chk);
        lv_style_set_bg_color(&s_ind_chk, lv_color_hex(UI_COLOR_PRIMARY));
        lv_style_set_border_color(&s_ind_chk, lv_color_hex(UI_COLOR_PRIMARY_BORDER));
        show_styles_inited = true;
    }
    s_cb_show_pass = lv_checkbox_create(s_panel);
    lv_checkbox_set_text(s_cb_show_pass, "Show password");
    lv_obj_add_style(s_cb_show_pass, &s_ind,     LV_PART_INDICATOR);
    lv_obj_add_style(s_cb_show_pass, &s_ind_chk, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_cb_show_pass, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_cb_show_pass, &lv_font_montserrat_24, 0);
    lv_obj_align(s_cb_show_pass, LV_ALIGN_TOP_LEFT, 0, 262);
    lv_obj_add_event_cb(s_cb_show_pass, show_pass_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Cancel button - bigger, red-tinted for "destructive" semantics
    lv_obj_t *cancel_btn = lv_btn_create(s_panel);
    lv_obj_set_size(cancel_btn, 240, 72);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 80, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    // Save button - bigger, brighter green
    lv_obj_t *save_btn = lv_btn_create(s_panel);
    lv_obj_set_size(save_btn, 240, 72);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -80, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_border_color(save_btn, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_border_width(save_btn, 2, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(save_lbl);

    // Physical keyboard: Enter -> Save, Esc -> Cancel.
    ui_kbd_set_buttons(save_btn, cancel_btn);

    // Keyboard - child of the modal (so it sits above the backdrop but is
    // not clipped by the dialog panel). Hidden until a textarea is focused.
    s_keyboard = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_white());
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(0x505050));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_keyboard, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_keyboard);
    ui_theme_keyboard_attach_caps_cycle(s_keyboard);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "Modal built");
}

void wifi_config_modal_init(void)
{
    modal_build();
}

void wifi_config_modal_show_then(void (*on_close)(void))
{
    s_on_close = on_close;
    wifi_config_modal_show();
}

void wifi_config_modal_show(void)
{
    modal_build();  // no-op if already built (idempotent via s_modal guard)
    if (s_modal_open) return;

    // Pre-fill SSID from current settings; leave password blank.
    qmx_settings_t s;
    settings_load_all(&s);
    lv_textarea_set_text(s_ta_ssid, s.wifi_ssid);
    lv_textarea_set_text(s_ta_pass, "");
    lv_textarea_set_password_mode(s_ta_pass, true);
    if (s_cb_show_pass) lv_obj_remove_state(s_cb_show_pass, LV_STATE_CHECKED);
    ui_theme_focus_textarea(s_ta_ssid);

    // Make sure keyboard starts hidden every time.
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_modal_open = true;
    ESP_LOGI(TAG, "Modal shown (current SSID='%s')", s.wifi_ssid);
}
