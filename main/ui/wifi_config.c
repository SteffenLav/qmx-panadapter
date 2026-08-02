// WiFi configuration modal - full-screen overlay for entering SSID/password.
// On Save: persists to NVS via panadapter_wifi_reconnect() and triggers a
// disconnect/reconnect cycle. On Cancel: closes without changes.

#include "wifi_config.h"
#include "ui_theme.h"
#include "wifi.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_config";

// Modal state - lazily created on first show.
static lv_obj_t *s_modal       = NULL;  // root full-screen overlay
static lv_obj_t *s_panel       = NULL;  // centred dialog panel
static lv_obj_t *s_ta_ssid       = NULL;
static lv_obj_t *s_ta_pass       = NULL;
static lv_obj_t *s_eye_btn       = NULL;  // password show/hide eye-icon button
static lv_obj_t *s_eye_lbl       = NULL;  // its icon label
static bool      s_pass_shown    = false;
static lv_obj_t *s_wifi_btn      = NULL;  // WiFi on/off icon button (replaces the old
                                          // drawer "WiFi initiated" checkbox)
static lv_obj_t *s_wifi_icon_lbl = NULL;  // the WiFi glyph itself
static lv_obj_t *s_wifi_slash    = NULL;  // diagonal red line, shown only when off -
                                          // LVGL has no built-in "WiFi with a slash"
                                          // glyph, so this mirrors the same
                                          // draw-a-line-over-the-icon technique already
                                          // used for the bottom-bar battery-absent
                                          // indicator (see s_bot_batt_slash in ui.c)
static bool      s_wifi_on       = false;
static lv_obj_t *s_keyboard      = NULL;
static bool      s_modal_open  = false;
static void    (*s_on_close)(void) = NULL;  // one-shot, fired when the modal closes

static void scan_list_hide(void);  // fwd decl (defined with the SSID picker below)

static void modal_close(void)
{
    if (!s_modal || !s_modal_open) return;
    scan_list_hide();
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
    ESP_LOGI(TAG, "Save: SSID='%s' (pass %d chars), wifi=%s",
             ssid, (int)strlen(pass), s_wifi_on ? "on" : "off");
    // The on/off icon in this modal is the authority on whether WiFi should be
    // up. If it's ON, save + connect. If it's OFF, only persist the credentials
    // (the icon's live toggle already put the radio in the off state) — otherwise
    // Save would force WiFi back on right after the user turned it off.
    if (s_wifi_on) {
        panadapter_wifi_reconnect(ssid, pass);
    } else {
        panadapter_wifi_update_credentials(ssid, pass);
    }
    modal_close();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

// Eye-icon button: toggle password visibility. Open eye = currently shown,
// crossed eye = currently masked.
static void eye_btn_cb(lv_event_t *e)
{
    (void)e;
    s_pass_shown = !s_pass_shown;
    lv_textarea_set_password_mode(s_ta_pass, !s_pass_shown);
    if (s_eye_lbl) lv_label_set_text(s_eye_lbl, s_pass_shown ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

// WiFi on/off icon button. Writes the same "wifi_enabled" setting the old
// drawer checkbox wrote, but now via panadapter_wifi_set_enabled(), which
// both persists the boot preference AND applies the change LIVE (turning the
// radio on connects immediately; turning it off stops it and suppresses
// auto-reconnect) — the old drawer checkbox was boot-time-only, which made
// toggling it back on appear to do nothing until a reboot.
static void wifi_toggle_btn_cb(lv_event_t *e)
{
    (void)e;
    s_wifi_on = !s_wifi_on;
    panadapter_wifi_set_enabled(s_wifi_on);   // persists + applies live
    if (s_wifi_slash) {
        if (s_wifi_on) lv_obj_add_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "WiFi toggled: %s", s_wifi_on ? "ON" : "OFF");
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

// ---- SSID scan picker -------------------------------------------------
// Tapping "Scan" runs a WiFi scan and shows the results as a tappable list;
// picking one fills the SSID field with the exact-case beacon name (so the
// case-sensitivity trap can't bite). Manual entry still works.
// The picker is one bordered "window" (s_scan_panel) holding a title, the
// scrollable AP list, and a red Cancel — so it reads as a single dialog. Panel
// is content-sized (flex column) and stays centred.
static lv_obj_t *s_scan_panel  = NULL;
static lv_obj_t *s_scan_title  = NULL;
static lv_obj_t *s_scan_list   = NULL;   // scrollable AP list inside the panel
static lv_obj_t *s_scan_cancel = NULL;
static lv_timer_t *s_scan_timer = NULL;        // polls for scan results
static lv_timer_t *s_scan_close_timer = NULL;  // one-shot close after a pick
static wifi_scan_ap_t s_aps[24];
static int s_aps_n = 0;

static void scan_list_hide(void)
{
    if (s_scan_timer)       { lv_timer_del(s_scan_timer);       s_scan_timer = NULL; }
    if (s_scan_close_timer) { lv_timer_del(s_scan_close_timer); s_scan_close_timer = NULL; }
    if (s_scan_panel) lv_obj_add_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN);
}

static void scan_close_cb(lv_event_t *e) { (void)e; scan_list_hide(); }

// One-shot: close the picker a moment after a row is picked, so the selection
// highlight is visible first.
static void scan_close_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_scan_close_timer = NULL;
    scan_list_hide();
}

static void scan_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (idx >= 0 && idx < s_aps_n) {
        lv_textarea_set_text(s_ta_ssid, s_aps[idx].ssid);  // exact case from beacon
    }
    // Strong selection confirmation: turn the picked row solid blue with a
    // check mark, then close shortly after so it's clearly seen.
    if (btn) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), 0);
        lv_obj_t *ck = lv_label_create(btn);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(ck, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ck, &lv_font_montserrat_24, 0);
        lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    if (!s_scan_close_timer) s_scan_close_timer = lv_timer_create(scan_close_timer_cb, 260, NULL);
}

// Rebuild the AP list (the title is set separately).
static void scan_list_render(const char *title)
{
    if (s_scan_title) lv_label_set_text(s_scan_title, title);
    lv_obj_clean(s_scan_list);
    for (int i = 0; i < s_aps_n; i++) {
        char label[64];
        snprintf(label, sizeof(label), "%.32s  %ddBm", s_aps[i].ssid, s_aps[i].rssi);
        lv_obj_t *btn = lv_list_add_button(s_scan_list, LV_SYMBOL_WIFI, label);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_24, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT), 0);
        // Pressed feedback while the finger is down.
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, scan_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// 400 ms polls; 50 of them = 20 s. Covers a scan whose SCAN_DONE event is
// lost (esp_hosted RPC drop) - without a cap the panel says "Scanning..."
// forever. Generous on purpose: an all-channel scan plus the firmware's
// one automatic retry legitimately takes up to ~14 s.
#define SCAN_POLL_MAX 50
static int s_scan_polls = 0;

static void scan_poll_cb(lv_timer_t *t)
{
    wifi_scan_state_t st = panadapter_wifi_scan_state();
    if (st == WIFI_SCAN_DONE) {
        s_aps_n = panadapter_wifi_scan_get(s_aps, (int)(sizeof(s_aps) / sizeof(s_aps[0])));
        scan_list_render(s_aps_n ? "Select a network:" : "No networks found");
        lv_timer_del(t);
        s_scan_timer = NULL;
    } else if (st == WIFI_SCAN_FAILED || ++s_scan_polls > SCAN_POLL_MAX) {
        s_aps_n = 0;
        scan_list_render("Scan failed - tap Scan to try again");
        lv_timer_del(t);
        s_scan_timer = NULL;
    }
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    s_aps_n = 0;
    s_scan_polls = 0;
    scan_list_render("Scanning for networks...");
    lv_obj_clear_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scan_panel);
    panadapter_wifi_scan_start();
    if (!s_scan_timer) s_scan_timer = lv_timer_create(scan_poll_cb, 400, NULL);
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
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
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

    // SSID textarea - larger font + taller. Narrowed to make room for the
    // Scan button on its right.
    s_ta_ssid = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_ssid, 640, 60);
    lv_obj_align(s_ta_ssid, LV_ALIGN_TOP_LEFT, 0, 86);
    lv_textarea_set_one_line(s_ta_ssid, true);
    lv_textarea_set_max_length(s_ta_ssid, 32);
    lv_textarea_set_placeholder_text(s_ta_ssid, "Network name");
    lv_obj_set_style_text_font(s_ta_ssid, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_ssid);
    lv_obj_add_event_cb(s_ta_ssid, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // Scan button - opens the SSID picker (avoids typing a case-sensitive SSID).
    lv_obj_t *scan_btn = lv_btn_create(s_panel);
    lv_obj_set_size(scan_btn, 172, 60);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 656, 86);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(scan_btn, 8, 0);
    lv_obj_add_event_cb(scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_WIFI " Scan");
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(scan_lbl);

    // Password label - larger
    lv_obj_t *pass_lbl = lv_label_create(s_panel);
    lv_label_set_text(pass_lbl, "Password");
    lv_obj_set_style_text_color(pass_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(pass_lbl, LV_ALIGN_TOP_LEFT, 0, 160);

    // Password textarea - full width, masked
    s_ta_pass = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_pass, 640, 60);
    lv_obj_align(s_ta_pass, LV_ALIGN_TOP_LEFT, 0, 190);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_max_length(s_ta_pass, 64);
    lv_textarea_set_placeholder_text(s_ta_pass, "WPA2 password (leave empty for open network)");
    lv_obj_set_style_text_font(s_ta_pass, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_pass);
    lv_obj_add_event_cb(s_ta_pass, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // Password show/hide eye-icon button — directly under the Scan button,
    // mirroring the SSID+Scan row above (the password field is shortened to
    // 640 to make room, same as the SSID field).
    s_eye_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_eye_btn, 172, 60);
    lv_obj_align(s_eye_btn, LV_ALIGN_TOP_LEFT, 656, 190);
    lv_obj_set_style_bg_color(s_eye_btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_border_color(s_eye_btn, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_eye_btn, 1, 0);
    lv_obj_set_style_radius(s_eye_btn, 8, 0);
    lv_obj_add_event_cb(s_eye_btn, eye_btn_cb, LV_EVENT_CLICKED, NULL);
    s_eye_lbl = lv_label_create(s_eye_btn);
    lv_label_set_text(s_eye_lbl, LV_SYMBOL_EYE_CLOSE);  // masked by default
    lv_obj_set_style_text_color(s_eye_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_eye_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_eye_lbl);

    // Bottom row: Cancel, Save, and the WiFi on/off toggle share this row now
    // (2026-07-04) - the toggle sits in the Eye button's column (x=656, same
    // 172x60 size), which used to be Save's territory, so Cancel and Save
    // both shifted left to fit all three across the panel width without
    // overlapping.

    // Cancel button - bigger, red-tinted for "destructive" semantics
    lv_obj_t *cancel_btn = lv_btn_create(s_panel);
    lv_obj_set_size(cancel_btn, 240, 72);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 40, 0);
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
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 360, 0);
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

    // WiFi on/off toggle - same column and size as the Eye button above it
    // (x=656, 172x60), bottom-aligned to share the Cancel/Save row.
    s_wifi_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_wifi_btn, 172, 60);
    lv_obj_align(s_wifi_btn, LV_ALIGN_BOTTOM_LEFT, 656, 0);
    lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_border_color(s_wifi_btn, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_wifi_btn, 1, 0);
    lv_obj_set_style_radius(s_wifi_btn, 8, 0);
    lv_obj_add_event_cb(s_wifi_btn, wifi_toggle_btn_cb, LV_EVENT_CLICKED, NULL);
    s_wifi_icon_lbl = lv_label_create(s_wifi_btn);
    lv_label_set_text(s_wifi_icon_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_wifi_icon_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_wifi_icon_lbl);
    // Diagonal red slash drawn over the icon when off - same technique as
    // the bottom-bar battery-absent indicator (ui.c s_bot_batt_slash), since
    // LVGL has no built-in "WiFi with a slash" glyph. Runs top-left ->
    // bottom-right ("\"). Static points array: must persist for the line's
    // lifetime.
    static lv_point_precise_t wifi_slash_pts[2] = { {0, 0}, {34, 34} };
    s_wifi_slash = lv_line_create(s_wifi_btn);
    lv_line_set_points(s_wifi_slash, wifi_slash_pts, 2);
    lv_obj_set_style_line_color(s_wifi_slash, lv_color_hex(0xFF5050), 0);
    lv_obj_set_style_line_width(s_wifi_slash, 5, 0);
    lv_obj_set_style_line_rounded(s_wifi_slash, true, 0);
    lv_obj_center(s_wifi_slash);

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

    // SSID picker "window": one bordered panel (title + scrollable AP list +
    // Cancel), content-sized as a flex column, centred. Same grey-on-dark look
    // as this WiFi window. Hidden until Scan is tapped.
    s_scan_panel = lv_obj_create(s_modal);
    lv_obj_set_width(s_scan_panel, 700);
    lv_obj_set_height(s_scan_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_scan_panel, 672, 0);
    lv_obj_set_align(s_scan_panel, LV_ALIGN_CENTER);  // stays centred as it resizes
    lv_obj_set_style_bg_color(s_scan_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_scan_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_scan_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_scan_panel, 2, 0);
    lv_obj_set_style_radius(s_scan_panel, 10, 0);
    lv_obj_set_style_pad_all(s_scan_panel, 16, 0);
    lv_obj_set_flex_flow(s_scan_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scan_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scan_panel, 12, 0);
    lv_obj_clear_flag(s_scan_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN);

    // Title (white, larger than the rows).
    s_scan_title = lv_label_create(s_scan_panel);
    lv_label_set_text(s_scan_title, "Select a network:");
    lv_obj_set_style_text_color(s_scan_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_scan_title, &lv_font_montserrat_28, 0);

    // Scrollable AP list, transparent so the panel surface shows through;
    // rows are styled grey in scan_list_render().
    s_scan_list = lv_list_create(s_scan_panel);
    lv_obj_set_width(s_scan_list, LV_PCT(100));
    lv_obj_set_height(s_scan_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_scan_list, 480, 0);
    lv_obj_set_style_bg_opa(s_scan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scan_list, 0, 0);
    lv_obj_set_style_pad_all(s_scan_list, 0, 0);

    // Red Cancel, inside the panel.
    s_scan_cancel = lv_btn_create(s_scan_panel);
    lv_obj_set_size(s_scan_cancel, 240, 72);
    lv_obj_set_style_bg_color(s_scan_cancel, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(s_scan_cancel, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(s_scan_cancel, 2, 0);
    lv_obj_set_style_radius(s_scan_cancel, 8, 0);
    lv_obj_add_event_cb(s_scan_cancel, scan_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sc_lbl = lv_label_create(s_scan_cancel);
    lv_label_set_text(sc_lbl, "Cancel");
    lv_obj_set_style_text_color(sc_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(sc_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(sc_lbl);

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

    // Pre-fill SSID AND password from current settings. Pre-filling the
    // password (masked) is what makes credentials "stick": Save reads the
    // field verbatim, so a blank field would overwrite the stored password
    // with an empty string every time the modal is opened and saved (this
    // was the "WiFi forgets my password / reason=210 can't join" bug). To
    // set an open network, clear the field explicitly.
    qmx_settings_t s;
    settings_load_all(&s);
    lv_textarea_set_text(s_ta_ssid, s.wifi_ssid);
    lv_textarea_set_text(s_ta_pass, s.wifi_pass);
    lv_textarea_set_password_mode(s_ta_pass, true);
    s_pass_shown = false;
    if (s_eye_lbl) lv_label_set_text(s_eye_lbl, LV_SYMBOL_EYE_CLOSE);

    // Sync the WiFi on/off icon from the persisted setting every time the
    // modal opens (there's no other UI surface for it now that the drawer
    // checkbox is gone).
    s_wifi_on = s.wifi_enabled;
    if (s_wifi_slash) {
        if (s_wifi_on) lv_obj_add_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
    }
    ui_theme_focus_textarea(s_ta_ssid);

    // Make sure keyboard starts hidden every time.
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_modal_open = true;
    ESP_LOGI(TAG, "Modal shown (current SSID='%s')", s.wifi_ssid);
}
