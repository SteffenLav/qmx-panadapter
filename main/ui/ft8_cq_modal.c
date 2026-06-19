// CQ message preset editor modal. Long-press "Call CQ" to open.
// Three editable message fields, each with a radio button to pick the active
// one; a short Call CQ tap transmits the selected message. Persisted to NVS.
//
// Structurally based on identity_config.c (modal + text areas + on-screen
// keyboard), with a single-select radio column and three fields.

#include "ft8_cq_modal.h"
#include "ui_theme.h"
#include "ft8_screen_view.h"
#include "settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "ft8_cq_modal";

#define N_CQ 3

static lv_obj_t *s_modal     = NULL;
static lv_obj_t *s_keyboard  = NULL;
static lv_obj_t *s_ta[N_CQ]    = { NULL, NULL, NULL };
static lv_obj_t *s_radio[N_CQ] = { NULL, NULL, NULL };
static lv_obj_t *s_add_lbl   = NULL;  // "+ <call> <grid>" quick-insert button label
static int       s_sel       = 0;
static bool      s_open      = false;

static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

// Build "CQ <call> <grid>" from stored identity (or "CQ" if unset).
static void build_default_cq(char *out, size_t len)
{
    qmx_settings_t s;
    settings_load_all(&s);
    if (s.my_callsign[0] && s.my_grid[0]) {
        snprintf(out, len, "CQ %s %s", s.my_callsign, s.my_grid);
    } else {
        snprintf(out, len, "CQ");
    }
}

void ft8_cq_get_active_text(char *out, size_t len)
{
    if (!out || !len) return;
    qmx_settings_t s;
    settings_load_all(&s);
    uint8_t sel = (s.cq_sel <= 2) ? s.cq_sel : 0;
    if (s.cq_msg[sel][0]) {
        strncpy(out, s.cq_msg[sel], len - 1);
        out[len - 1] = '\0';
    } else {
        build_default_cq(out, len);  // empty slot -> default CQ
    }
}

// Reflect single-selection in the radio column.
static void apply_radio_state(void)
{
    for (int i = 0; i < N_CQ; i++) {
        if (!s_radio[i]) continue;
        if (i == s_sel) lv_obj_add_state(s_radio[i], LV_STATE_CHECKED);
        else            lv_obj_remove_state(s_radio[i], LV_STATE_CHECKED);
    }
}

static void radio_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel = idx;
    apply_radio_state();
}

static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    // Editing a field implies you want to use it: select its radio too.
    // Also only one field shows the blinking cursor at a time.
    for (int i = 0; i < N_CQ; i++) {
        if (s_ta[i] == ta) {
            s_sel = i; apply_radio_state();
        } else {
            lv_obj_remove_state(s_ta[i], LV_STATE_FOCUSED);
        }
    }
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

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

// Append "<call> <grid>" (from saved identity) to the active field, with a
// leading space only if needed. Every standard CQ ends with this fixed tail,
// so the user just types the prefix ("CQ DX ") and taps this.
static void add_suffix_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < 0 || s_sel >= N_CQ || !s_ta[s_sel]) return;
    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.my_callsign[0] || !s.my_grid[0]) return;  // nothing to add

    lv_obj_t *ta = s_ta[s_sel];
    const char *cur = lv_textarea_get_text(ta);
    size_t len = cur ? strlen(cur) : 0;
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    if (len > 0 && cur[len - 1] != ' ') lv_textarea_add_text(ta, " ");
    char tail[24];
    snprintf(tail, sizeof(tail), "%s %s", s.my_callsign, s.my_grid);
    lv_textarea_add_text(ta, tail);
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < N_CQ; i++) {
        char buf[28] = {0};
        const char *raw = s_ta[i] ? lv_textarea_get_text(s_ta[i]) : NULL;
        if (raw) { strncpy(buf, raw, sizeof(buf) - 1); to_upper_inplace(buf); }
        settings_set_cq_msg((uint8_t)i, buf);
    }
    settings_set_cq_sel((uint8_t)s_sel);
    ESP_LOGI(TAG, "saved CQ presets, active=%d", s_sel);
    modal_close();
    // Refresh the Call CQ button label to the newly-selected message.
    ft8_screen_view_refresh_cq_label();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
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
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, 1040, 380);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "CQ Messages  (check the active)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Square themed checkbox indicator — same style as filter modal / drawer.
    static lv_style_t style_ind;
    static lv_style_t style_ind_chk;
    static bool styles_inited = false;
    if (!styles_inited) {
        lv_style_init(&style_ind);
        lv_style_set_bg_color(&style_ind, lv_color_hex(UI_COLOR_SURFACE_RAISED));
        lv_style_set_border_color(&style_ind, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&style_ind, 2);
        lv_style_set_pad_all(&style_ind, 8);
        lv_style_init(&style_ind_chk);
        lv_style_set_bg_color(&style_ind_chk, lv_color_hex(UI_COLOR_PRIMARY));
        lv_style_set_border_color(&style_ind_chk, lv_color_hex(UI_COLOR_PRIMARY_BORDER));
        styles_inited = true;
    }

    for (int i = 0; i < N_CQ; i++) {
        int y = 56 + i * 72;

        // Text area first, so the checkbox can be vertically centred against it.
        s_ta[i] = lv_textarea_create(panel);
        lv_obj_set_size(s_ta[i], 900, 60);
        lv_obj_align(s_ta[i], LV_ALIGN_TOP_LEFT, 80, y);
        lv_textarea_set_one_line(s_ta[i], true);
        // 22 chars covers the longest valid standard CQ ("CQ <4ch> <call> <grid>");
        // the ft8_lib encoder is the real gate and rejects anything unpackable.
        lv_textarea_set_max_length(s_ta[i], 22);
        lv_textarea_set_placeholder_text(s_ta[i], "e.g. CQ DX OZ1LAV JO65FR");
        lv_obj_set_style_text_font(s_ta[i], &lv_font_montserrat_24, 0);
        ui_theme_style_textarea(s_ta[i]);
        lv_obj_add_event_cb(s_ta[i], ta_focused_cb, LV_EVENT_FOCUSED, NULL);

        s_radio[i] = lv_checkbox_create(panel);
        lv_checkbox_set_text(s_radio[i], "");
        lv_obj_add_style(s_radio[i], &style_ind,     LV_PART_INDICATOR);
        lv_obj_add_style(s_radio[i], &style_ind_chk, LV_PART_INDICATOR | LV_STATE_CHECKED);
        // Centre the checkbox on the text field's vertical midline.
        lv_obj_align_to(s_radio[i], s_ta[i], LV_ALIGN_OUT_LEFT_MID, -16, 0);
        lv_obj_add_event_cb(s_radio[i], radio_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 220, 64);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 40, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    // Quick-insert: append "<call> <grid>" to the active field. Label is set
    // dynamically in show() from the saved identity.
    lv_obj_t *add_btn = lv_btn_create(panel);
    lv_obj_set_size(add_btn, 380, 64);
    lv_obj_align(add_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(add_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(add_btn, 8, 0);
    lv_obj_add_event_cb(add_btn, add_suffix_cb, LV_EVENT_CLICKED, NULL);
    s_add_lbl = lv_label_create(add_btn);
    lv_label_set_text(s_add_lbl, "+ Call Grid");
    lv_obj_set_style_text_color(s_add_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_add_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_add_lbl);

    lv_obj_t *save_btn = lv_btn_create(panel);
    lv_obj_set_size(save_btn, 220, 64);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -40, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(save_lbl);

    // Physical keyboard: Enter -> Save, Esc -> Cancel.
    ui_kbd_set_buttons(save_btn, cancel_btn);

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
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    // Start in ABC (caps-lock) — CQ messages are always uppercase.
    ui_theme_keyboard_attach_caps_cycle_upper(s_keyboard);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "CQ modal built");
}

void ft8_cq_modal_init(void)
{
    modal_build();
}

void ft8_cq_modal_show(void)
{
    modal_build();
    if (s_open) return;

    qmx_settings_t s;
    settings_load_all(&s);
    s_sel = (s.cq_sel <= 2) ? s.cq_sel : 0;

    // Quick-insert button shows the actual call+grid (or a hint if unset).
    if (s_add_lbl) {
        char lbl[32];
        if (s.my_callsign[0] && s.my_grid[0]) {
            snprintf(lbl, sizeof(lbl), "+ %s %s", s.my_callsign, s.my_grid);
        } else {
            snprintf(lbl, sizeof(lbl), "+ Call Grid (set ID)");
        }
        lv_label_set_text(s_add_lbl, lbl);
    }

    for (int i = 0; i < N_CQ; i++) {
        if (i == 0 && !s.cq_msg[0][0]) {
            char def[28];
            build_default_cq(def, sizeof(def));
            lv_textarea_set_text(s_ta[0], def);
        } else {
            lv_textarea_set_text(s_ta[i], s.cq_msg[i]);
        }
    }
    apply_radio_state();
    ui_theme_focus_textarea(s_ta[0]);

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    ESP_LOGI(TAG, "CQ modal shown (active=%d)", s_sel);
}
