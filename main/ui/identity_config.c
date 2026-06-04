// Operator identity modal - full-screen overlay for entering callsign
// and Maidenhead grid. On Save: persists to NVS via the settings layer.
// On Cancel: closes without changes.
//
// Structurally a copy of wifi_config.c with two text fields instead of
// SSID/password. Generalisation into a shared 2-field text modal is a
// future refactor (memory channels v2 also wants it).

#include "identity_config.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "identity_config";

// Modal state - lazily created on first show.
static lv_obj_t *s_modal       = NULL;
static lv_obj_t *s_panel       = NULL;
static lv_obj_t *s_ta_call     = NULL;
static lv_obj_t *s_ta_grid     = NULL;
static lv_obj_t *s_keyboard    = NULL;
static bool      s_modal_open  = false;

static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static void modal_close(void)
{
    if (!s_modal || !s_modal_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_modal_open = false;
    ESP_LOGI(TAG, "Modal closed");
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *call_raw = lv_textarea_get_text(s_ta_call);
    const char *grid_raw = lv_textarea_get_text(s_ta_grid);

    char call[16] = {0};
    char grid[8]  = {0};
    if (call_raw) { strncpy(call, call_raw, sizeof(call) - 1); to_upper_inplace(call); }
    if (grid_raw) { strncpy(grid, grid_raw, sizeof(grid) - 1); to_upper_inplace(grid); }

    ESP_LOGI(TAG, "Save: call='%s' grid='%s'", call, grid);
    settings_set_my_callsign(call);
    settings_set_my_grid(grid);
    modal_close();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!s_keyboard) return;
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void modal_build(void)
{
    if (s_modal) return;

    lv_obj_t *scr = lv_screen_active();

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

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Operator Identity");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *call_lbl = lv_label_create(s_panel);
    lv_label_set_text(call_lbl, "Callsign");
    lv_obj_set_style_text_color(call_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(call_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(call_lbl, LV_ALIGN_TOP_LEFT, 0, 56);

    s_ta_call = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_call, 820, 60);
    lv_obj_align(s_ta_call, LV_ALIGN_TOP_LEFT, 0, 86);
    lv_textarea_set_one_line(s_ta_call, true);
    lv_textarea_set_max_length(s_ta_call, 15);
    lv_textarea_set_placeholder_text(s_ta_call, "e.g. OZ1LAV");
    lv_obj_set_style_text_font(s_ta_call, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(s_ta_call, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *grid_lbl = lv_label_create(s_panel);
    lv_label_set_text(grid_lbl, "Maidenhead grid (4 or 6 chars)");
    lv_obj_set_style_text_color(grid_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(grid_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(grid_lbl, LV_ALIGN_TOP_LEFT, 0, 160);

    s_ta_grid = lv_textarea_create(s_panel);
    lv_obj_set_size(s_ta_grid, 820, 60);
    lv_obj_align(s_ta_grid, LV_ALIGN_TOP_LEFT, 0, 190);
    lv_textarea_set_one_line(s_ta_grid, true);
    lv_textarea_set_max_length(s_ta_grid, 6);
    lv_textarea_set_placeholder_text(s_ta_grid, "e.g. JO45 or JO45ab");
    lv_obj_set_style_text_font(s_ta_grid, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(s_ta_grid, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

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

    s_keyboard = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(0x303030));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_white());
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(0x505050));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_keyboard, &style_kb_btn, LV_PART_ITEMS);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    // UPPER mode by default - callsigns and grids are uppercase.
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_24, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "Modal built");
}

void identity_config_modal_show(void)
{
    modal_build();
    if (s_modal_open) return;

    qmx_settings_t s;
    settings_load_all(&s);
    lv_textarea_set_text(s_ta_call, s.my_callsign);
    lv_textarea_set_text(s_ta_grid, s.my_grid);

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_modal_open = true;
    ESP_LOGI(TAG, "Modal shown (call='%s' grid='%s')", s.my_callsign, s.my_grid);
}
