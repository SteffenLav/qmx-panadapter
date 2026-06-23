// FT8 CQ-run reply filter settings modal. Opened via the "Filter" button
// above "Call CQ". Lets the operator narrow the auto-reply target picker
// by matching the *whole* decoded message text (callsign, POTA/SOTA tags,
// grid, etc.) — not just the remote callsign.
//
// Structurally based on ft8_cq_modal.c (modal + textarea + on-screen
// keyboard), extended with plain checkboxes (not exclusive radios — several
// filters can be active at once).

#include "ft8_filter_modal.h"
#include "ft8_time_modal.h"
#include "ui_theme.h"
#include "settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "ft8_filter_modal";

static lv_obj_t *s_modal    = NULL;
static lv_obj_t *s_keyboard = NULL;

static lv_obj_t *s_cb_incl[2]  = { NULL, NULL };
static lv_obj_t *s_ta_incl[2]  = { NULL, NULL };
static lv_obj_t *s_cb_excl[2]  = { NULL, NULL };
static lv_obj_t *s_ta_excl[2]  = { NULL, NULL };
static lv_obj_t *s_cb_worked_before = NULL;
static lv_obj_t *s_cb_plain_cq      = NULL;
static lv_obj_t *s_cb_incl_cq_only  = NULL;
static lv_obj_t *s_cb_robot         = NULL;  // auto-answer enable
static lv_obj_t *s_dd_robot_pri     = NULL;  // priority dropdown

static bool      s_open = false;

static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
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

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    ft8_filters_t f = {0};

    for (int i = 0; i < 2; i++) {
        f.incl_en[i] = lv_obj_has_state(s_cb_incl[i], LV_STATE_CHECKED);
        const char *raw = lv_textarea_get_text(s_ta_incl[i]);
        strncpy(f.incl_text[i], raw, sizeof(f.incl_text[i]) - 1);
        to_upper_inplace(f.incl_text[i]);

        f.excl_en[i] = lv_obj_has_state(s_cb_excl[i], LV_STATE_CHECKED);
        raw = lv_textarea_get_text(s_ta_excl[i]);
        strncpy(f.excl_text[i], raw, sizeof(f.excl_text[i]) - 1);
        to_upper_inplace(f.excl_text[i]);
    }
    f.excl_worked_before = lv_obj_has_state(s_cb_worked_before, LV_STATE_CHECKED);
    f.excl_plain_cq      = lv_obj_has_state(s_cb_plain_cq, LV_STATE_CHECKED);
    f.incl_cq_only       = lv_obj_has_state(s_cb_incl_cq_only, LV_STATE_CHECKED);
    f.robot_en           = lv_obj_has_state(s_cb_robot, LV_STATE_CHECKED);
    f.robot_priority     = (uint8_t)lv_dropdown_get_selected(s_dd_robot_pri);

    settings_set_ft8_filters(&f);
    ESP_LOGI(TAG, "saved filters: incl=[%d:'%s' %d:'%s'] excl=[%d:'%s' %d:'%s'] wb=%d cq=%d",
             f.incl_en[0], f.incl_text[0], f.incl_en[1], f.incl_text[1],
             f.excl_en[0], f.excl_text[0], f.excl_en[1], f.excl_text[1],
             f.excl_worked_before, f.excl_plain_cq);
    modal_close();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void sync_time_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
    ft8_time_modal_show();
}

// Plain (non-exclusive) checkbox with a themed square indicator. Extra
// padding on the indicator gives a bigger touch target (matches the CQ
// preset modal's radio buttons).
static lv_obj_t *make_checkbox(lv_obj_t *parent, const char *text)
{
    static lv_style_t style_ind;
    static bool        style_inited = false;
    if (!style_inited) {
        lv_style_init(&style_ind);
        lv_style_set_bg_color(&style_ind, lv_color_hex(UI_COLOR_SURFACE_RAISED));
        lv_style_set_border_color(&style_ind, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&style_ind, 2);
        lv_style_set_pad_all(&style_ind, 8);
        style_inited = true;
    }
    static lv_style_t style_ind_checked;
    static bool        style_checked_inited = false;
    if (!style_checked_inited) {
        lv_style_init(&style_ind_checked);
        lv_style_set_bg_color(&style_ind_checked, lv_color_hex(UI_COLOR_PRIMARY));
        lv_style_set_border_color(&style_ind_checked, lv_color_hex(UI_COLOR_PRIMARY_BORDER));
        style_checked_inited = true;
    }

    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, text);
    lv_obj_set_style_text_color(cb, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_add_style(cb, &style_ind, LV_PART_INDICATOR);
    lv_obj_add_style(cb, &style_ind_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
    return cb;
}

// One include/exclude row: a bare (textless) checkbox left of a one-line
// textarea, vertically centred against it.
static void make_filter_row(lv_obj_t *panel, int y, lv_obj_t **cb_out, lv_obj_t **ta_out,
                             const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_size(ta, 750, 56);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 60, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, FT8_FILTER_TEXT_LEN - 1);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(ta);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // Extra gap so a finger on the checkbox can't also clip the textarea.
    lv_obj_t *cb = make_checkbox(panel, "");
    lv_obj_align_to(cb, ta, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    *cb_out = cb;
    *ta_out = ta;
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
    lv_obj_set_size(panel, 1040, 540);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // --- Include section ---------------------------------------------
    lv_obj_t *incl_lbl = lv_label_create(panel);
    lv_label_set_text(incl_lbl, "Include decodes matching ANY of the following:");
    lv_obj_set_style_text_color(incl_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(incl_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(incl_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    make_filter_row(panel, 38, &s_cb_incl[0], &s_ta_incl[0], "e.g. POTA SOTA");
    make_filter_row(panel, 100, &s_cb_incl[1], &s_ta_incl[1], "e.g. /P, /M");

    // --- Exclude section ---------------------------------------------
    lv_obj_t *excl_lbl = lv_label_create(panel);
    lv_label_set_text(excl_lbl, "Exclude decodes matching ANY of the following:");
    lv_obj_set_style_text_color(excl_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(excl_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(excl_lbl, LV_ALIGN_TOP_LEFT, 0, 172);

    make_filter_row(panel, 206, &s_cb_excl[0], &s_ta_excl[0], "e.g. R7 UA9");
    make_filter_row(panel, 268, &s_cb_excl[1], &s_ta_excl[1], "e.g. JA, VK");

    // --- Other filters, side by side to save a line --------------------
    s_cb_worked_before = make_checkbox(panel, "Exclude worked-before (v0.16.0)");
    lv_obj_set_style_text_font(s_cb_worked_before, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_cb_worked_before, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(s_cb_worked_before, LV_ALIGN_TOP_LEFT, 0, 330);

    s_cb_plain_cq = make_checkbox(panel, "Exclude plain CQ callers");
    lv_obj_set_style_text_font(s_cb_plain_cq, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_cb_plain_cq, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(s_cb_plain_cq, LV_ALIGN_TOP_LEFT, 460, 330);

    s_cb_incl_cq_only = make_checkbox(panel, "Show only CQ callers");
    lv_obj_set_style_text_font(s_cb_incl_cq_only, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_cb_incl_cq_only, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(s_cb_incl_cq_only, LV_ALIGN_TOP_LEFT, 0, 374);

    // --- Robot (auto-answer) row -------------------------------------
    s_cb_robot = make_checkbox(panel, "Auto-answer CQ (robot)");
    lv_obj_set_style_text_font(s_cb_robot, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_cb_robot, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(s_cb_robot, LV_ALIGN_TOP_LEFT, 0, 426);

    lv_obj_t *pri_lbl = lv_label_create(panel);
    lv_label_set_text(pri_lbl, "Priority:");
    lv_obj_set_style_text_font(pri_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(pri_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(pri_lbl, LV_ALIGN_TOP_LEFT, 420, 432);

    // Order MUST match ft8_robot_priority_t (STRONGEST=0, WEAKEST=1, DISTANT=2).
    s_dd_robot_pri = lv_dropdown_create(panel);
    lv_dropdown_set_options(s_dd_robot_pri, "Strongest\nWeakest\nMost distant");
    lv_obj_set_width(s_dd_robot_pri, 280);
    lv_obj_align(s_dd_robot_pri, LV_ALIGN_TOP_LEFT, 540, 422);
    lv_obj_set_style_text_font(s_dd_robot_pri, &lv_font_montserrat_24, 0);

    // --- Save / Cancel / Sync Time on the right edge, evenly distributed.
    // Panel inner h = 540-40 = 500 px. Three buttons h=64: total 192 px.
    // Remaining 308 px / 4 gaps = 77 px each → y = 77 / 218 / 359.
    {
        struct { const char *lbl; uint32_t col; lv_event_cb_t cb; } btns[3] = {
            { "Save",      0x2e8b3a, save_btn_cb      },
            { "Cancel",    0x962020, cancel_btn_cb     },
            { "Sync Time", UI_COLOR_PRIMARY, sync_time_btn_cb },
        };
        lv_obj_t *save_b = NULL, *cancel_b = NULL;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = lv_btn_create(panel);
            lv_obj_set_size(b, 180, 64);
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 77 + i * (64 + 77));
            lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].col), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, btns[i].lbl);
            lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
            lv_obj_center(l);
            if (i == 0) save_b = b;
            else if (i == 1) cancel_b = b;
        }
        // Physical keyboard: Enter -> Save, Esc -> Cancel.
        ui_kbd_set_buttons(save_b, cancel_b);
    }

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
    // Start in ABC (caps-lock) — filter terms are always uppercase callsigns/tags.
    ui_theme_keyboard_attach_caps_cycle_upper(s_keyboard);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "filter modal built");
}

static void apply_checkbox_state(lv_obj_t *cb, bool checked)
{
    if (checked) lv_obj_add_state(cb, LV_STATE_CHECKED);
    else lv_obj_clear_state(cb, LV_STATE_CHECKED);
}

void ft8_filter_modal_init(void)
{
    modal_build();
}

void ft8_filter_modal_show(void)
{
    modal_build();
    if (s_open) return;

    qmx_settings_t s;
    settings_load_all(&s);
    const ft8_filters_t *f = &s.ft8_filters;

    for (int i = 0; i < 2; i++) {
        apply_checkbox_state(s_cb_incl[i], f->incl_en[i]);
        lv_textarea_set_text(s_ta_incl[i], f->incl_text[i]);
        apply_checkbox_state(s_cb_excl[i], f->excl_en[i]);
        lv_textarea_set_text(s_ta_excl[i], f->excl_text[i]);
    }
    apply_checkbox_state(s_cb_worked_before, f->excl_worked_before);
    apply_checkbox_state(s_cb_plain_cq, f->excl_plain_cq);
    apply_checkbox_state(s_cb_incl_cq_only, f->incl_cq_only);
    apply_checkbox_state(s_cb_robot, f->robot_en);
    lv_dropdown_set_selected(s_dd_robot_pri,
                             f->robot_priority <= FT8_ROBOT_PRI_DISTANT ? f->robot_priority : 0);

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    ESP_LOGI(TAG, "filter modal shown");
}
