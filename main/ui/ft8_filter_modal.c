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
#include "ft8_screen_view.h"
#include "ft8_test.h"
#include "ui_theme.h"
#include "ui.h"
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
static lv_obj_t *s_cb_skip_tx1      = NULL;  // pounce: skip grid, send report first
static lv_obj_t *s_cb_robot         = NULL;  // auto-answer enable
static lv_obj_t *s_dd_robot_pri     = NULL;  // priority dropdown
static lv_obj_t *s_dd_hound         = NULL;  // Fox/Hound: off / guided / automatic
static lv_obj_t *s_cb_auto_pileup   = NULL;  // auto-work waiting pileup callers on QSO completion
static lv_obj_t *s_cb_greylist      = NULL;  // grey-list stations after repeated failed pounces
static lv_obj_t *s_robot_warn       = NULL;  // "unattended TX" warning - shown while robot OR auto-pileup is checked
static lv_obj_t *s_cb_field_day     = NULL;  // ARRL Field Day exchange mode enable
static lv_obj_t *s_ta_fd_class      = NULL;  // e.g. "16A"
static lv_obj_t *s_ta_fd_section    = NULL;  // e.g. "EMA"
static lv_obj_t *s_sync_time_btn    = NULL;  // "Sync Time" button (FT8 + FT4)

static bool      s_open = false;

static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (!s_keyboard) return;
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

// Field Day class/section row sits near the bottom of the panel - the
// bottom-anchored keyboard would cover it while typing, so these two fields
// pop the keyboard at the TOP of the screen instead (everything else keeps
// the normal bottom keyboard, since those rows are higher up and unaffected).
static void ta_focused_top_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (!s_keyboard) return;
    lv_obj_align(s_keyboard, LV_ALIGN_TOP_MID, 0, 0);
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
    f.skip_tx1           = lv_obj_has_state(s_cb_skip_tx1, LV_STATE_CHECKED);
    f.robot_en           = lv_obj_has_state(s_cb_robot, LV_STATE_CHECKED);
    f.robot_priority     = (uint8_t)lv_dropdown_get_selected(s_dd_robot_pri);
    f.auto_pileup        = lv_obj_has_state(s_cb_auto_pileup, LV_STATE_CHECKED);

    settings_set_ft8_filters(&f);
    settings_set_greylist_en(lv_obj_has_state(s_cb_greylist, LV_STATE_CHECKED));
    settings_set_hound_mode((uint8_t)lv_dropdown_get_selected(s_dd_hound));

    {
        char cls[8], sect[8];
        strncpy(cls, lv_textarea_get_text(s_ta_fd_class), sizeof(cls) - 1);
        cls[sizeof(cls) - 1] = '\0';
        to_upper_inplace(cls);
        strncpy(sect, lv_textarea_get_text(s_ta_fd_section), sizeof(sect) - 1);
        sect[sizeof(sect) - 1] = '\0';
        to_upper_inplace(sect);
        settings_set_field_day_en(lv_obj_has_state(s_cb_field_day, LV_STATE_CHECKED));
        settings_set_fd_class(cls);
        settings_set_fd_section(sect);
        ESP_LOGI(TAG, "saved Field Day: en=%d class='%s' section='%s'",
                 lv_obj_has_state(s_cb_field_day, LV_STATE_CHECKED), cls, sect);
        // The Call CQ button label (and the CQ preset editor's FD hint, if
        // open) shows the live "CQ FD ..." tag - it only reflects the saved
        // setting on its next refresh, so without this it stayed stuck on
        // whatever it showed before this save (e.g. still "CQ FD ..." after
        // unchecking Field Day mode here).
        ft8_screen_view_refresh_cq_label();
    }
    ESP_LOGI(TAG, "saved filters: incl=[%d:'%s' %d:'%s'] excl=[%d:'%s' %d:'%s'] wb=%d cq=%d skip_tx1=%d robot=%d pileup=%d",
             f.incl_en[0], f.incl_text[0], f.incl_en[1], f.incl_text[1],
             f.excl_en[0], f.excl_text[0], f.excl_en[1], f.excl_text[1],
             f.excl_worked_before, f.excl_plain_cq, f.skip_tx1, f.robot_en, f.auto_pileup);
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

// Only nag about unattended TX while one of the auto-TX features (robot or
// auto-work-pileup) is actually enabled - no reason to show it to operators
// who never touch either checkbox. Shared by both checkboxes' handlers.
static void update_unattended_warn(void)
{
    if (!s_robot_warn) return;
    bool on = (s_cb_robot       && lv_obj_has_state(s_cb_robot,       LV_STATE_CHECKED)) ||
              (s_cb_auto_pileup && lv_obj_has_state(s_cb_auto_pileup, LV_STATE_CHECKED));
    if (on) lv_obj_clear_flag(s_robot_warn, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_robot_warn, LV_OBJ_FLAG_HIDDEN);
}

static void robot_checked_cb(lv_event_t *e)
{
    (void)e;
    update_unattended_warn();
}

// LVGL creates a dropdown's option list lazily when it opens, so style it
// here rather than at creation time (matches the existing pattern in ui.c's
// drawer dropdowns) - sized to match the rest of the filter window's text
// instead of the dropdown widget's smaller default list font.
static void robot_pri_dropdown_open_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (!list) return;
    lv_obj_set_style_text_font(list, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(UI_COLOR_BORDER), 0);
    // The bigger montserrat_24 font (above) needs a taller list than the
    // dropdown's own default sizing gives it - the 3rd option ("Most
    // distant") was getting clipped. Grow it 15px.
    lv_obj_set_height(list, lv_obj_get_height(list) + 15);
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

// A textless checkbox plus a separate label object placed beside it.
// Pixel-measured on real hardware: giving lv_checkbox_set_text() actual text
// makes LVGL size that checkbox's indicator larger (41x41) than a textless
// checkbox's indicator (31x32) - a style override on LV_PART_INDICATOR did
// NOT fix this (measured no effect), so instead every checkbox in this modal
// is now textless and uses this same code path as the rows next to the
// textareas (which were already the smaller, correct size) - identical
// construction guarantees identical rendered size, not just a style hint
// that may or may not be honored.
// NOTE: cb must be positioned (lv_obj_align) BEFORE the label is aligned
// relative to it - lv_obj_align_to() resolves against the checkbox's
// position at the time of the call, not wherever it ends up later. Doing
// this in one helper that positions cb itself first avoids the ordering bug
// (label aligning to cb's pre-positioned default, not its final spot).
static lv_obj_t *make_labeled_checkbox(lv_obj_t *panel, const char *text, int x, int y, lv_obj_t **lbl_out)
{
    lv_obj_t *cb = make_checkbox(panel, "");
    lv_obj_align(cb, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_align_to(lbl, cb, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    if (lbl_out) *lbl_out = lbl;
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
    // No text on this one, so font doesn't matter here - make_checkbox()'s
    // fixed indicator size keeps it the same size as the labeled checkboxes
    // below regardless.
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
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(s_modal);
    // Flush top-to-bottom of the 720 px screen: was 768 tall at a +14 offset
    // (bottom edge at 782, overflowing the screen by 62 px) - shrunk to fit
    // exactly by tightening the ARRL Field Day row's gap below (see y=625).
    lv_obj_set_size(panel, 1040, 720);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 0);
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

    // --- Other filters, stacked in one left-aligned column --------------
    // 60 px row spacing (was 44) - big-finger touch clearance around the
    // checkboxes was too tight to safely tap just one (field feedback).
    lv_obj_t *lbl_worked_before, *lbl_plain_cq, *lbl_incl_cq_only, *lbl_skip_tx1, *lbl_robot;

    s_cb_worked_before = make_labeled_checkbox(panel, "Exclude worked-before", 4, 330, &lbl_worked_before);
    lv_obj_set_style_text_color(lbl_worked_before, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    s_cb_plain_cq = make_labeled_checkbox(panel, "Exclude plain CQ callers", 4, 390, &lbl_plain_cq);
    lv_obj_set_style_text_color(lbl_plain_cq, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    s_cb_incl_cq_only = make_labeled_checkbox(panel, "Show only CQ callers", 4, 450, &lbl_incl_cq_only);
    lv_obj_set_style_text_color(lbl_incl_cq_only, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    // Pounce shortcut: first TX is a signal report (skip the grid exchange),
    // going straight into the roger/RR73 wait - see ft8_qso_start(). Does not
    // affect CQ-run, which already answers with a report first by default.
    s_cb_skip_tx1 = make_labeled_checkbox(panel, "Skip TX1 (send report first, no grid)", 4, 510, &lbl_skip_tx1);
    lv_obj_set_style_text_color(lbl_skip_tx1, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    // --- Robot (auto-answer) row — LIVE TX ---------------------------
    // Unattended: when enabled, the robot picks a CQ caller (per the filters
    // above + the chosen priority) and runs a full QSO with no per-exchange
    // confirmation. See ft8_robot.h for the safety/scope notes. The warning
    // label is only shown while the checkbox is checked - no need to nag
    // operators who never turn this on.
    s_cb_robot = make_labeled_checkbox(panel, "Auto-answer CQ with priority:", 4, 570, &lbl_robot);
    lv_obj_set_style_text_color(lbl_robot, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    // Order MUST match ft8_robot_priority_t (STRONGEST=0, WEAKEST=1, DISTANT=2).
    s_dd_robot_pri = lv_dropdown_create(panel);
    lv_dropdown_set_options(s_dd_robot_pri, "Strongest\nWeakest\nMost distant");
    lv_obj_set_width(s_dd_robot_pri, 280);
    lv_obj_align_to(s_dd_robot_pri, lbl_robot, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_set_style_text_font(s_dd_robot_pri, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(s_dd_robot_pri, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_border_color(s_dd_robot_pri, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_text_color(s_dd_robot_pri, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_add_event_cb(s_dd_robot_pri, robot_pri_dropdown_open_cb, LV_EVENT_CLICKED, NULL);

    s_robot_warn = lv_label_create(panel);
    lv_label_set_text(s_robot_warn, LV_SYMBOL_WARNING " Transmits unattended - never leave running unsupervised");
    lv_obj_set_style_text_font(s_robot_warn, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_robot_warn, lv_color_hex(0xFF8800), 0);
    // Right column, in the free space directly above the "Auto-work pileup"
    // row (540,450) - the old spot (4,612) sat right under the robot row and
    // collided with the ARRL Field Day row at y=635 (operator report). Width-
    // bounded so the wrapped text stays clear of the right-edge buttons.
    lv_obj_set_width(s_robot_warn, 460);
    lv_obj_align(s_robot_warn, LV_ALIGN_TOP_LEFT, 540, 372);
    lv_obj_add_event_cb(s_cb_robot, robot_checked_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // --- Auto-work pileup row — LIVE TX (unattended) ------------------
    // When on, finishing a QSO auto-pounces the strongest station still waiting
    // in the pileup (someone who called during a busy exchange) instead of
    // dropping back to CQ, draining the list unattended. Shares the robot's
    // unattended-TX warning below. Placed in the right column so the packed
    // left checkbox stack doesn't need a reflow.
    lv_obj_t *lbl_auto_pileup;
    s_cb_auto_pileup = make_labeled_checkbox(panel, "Auto-work pileup", 540, 450, &lbl_auto_pileup);
    lv_obj_set_style_text_color(lbl_auto_pileup, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_add_event_cb(s_cb_auto_pileup, robot_checked_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Grey-listing: after repeated failed pounces at a station (they never
    // respond - deaf to QRP, or ignoring us) the auto pickers stop re-calling
    // it; its rows recolour and a tap offers "Clear from grey-list". The
    // tracked list is RAM-only (reboot clears it). Roy KI0ER field request.
    lv_obj_t *lbl_greylist;
    s_cb_greylist = make_labeled_checkbox(panel, "Allow grey-listing", 540, 510, &lbl_greylist);
    lv_obj_set_style_text_color(lbl_greylist, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    // --- ARRL Field Day exchange mode ---------------------------------
    // When on, FT8 QSOs (manual and auto) exchange class+section instead of
    // grid/signal report - see CLAUDE.md "FT8 robot" / ft8_qso.c. Class and
    // section are short fixed-format fields, not free text.
    lv_obj_t *lbl_fd;
    s_cb_field_day = make_labeled_checkbox(panel, "ARRL Field Day mode:", 4, 635, &lbl_fd);
    lv_obj_set_style_text_color(lbl_fd, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);

    s_ta_fd_class = lv_textarea_create(panel);
    lv_obj_set_size(s_ta_fd_class, 100, 50);
    lv_obj_align_to(s_ta_fd_class, lbl_fd, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_textarea_set_one_line(s_ta_fd_class, true);
    lv_textarea_set_max_length(s_ta_fd_class, 3);
    lv_textarea_set_placeholder_text(s_ta_fd_class, "16A");
    lv_obj_set_style_text_font(s_ta_fd_class, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_fd_class);
    lv_obj_set_style_text_color(s_ta_fd_class, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_ta_fd_class, ta_focused_top_cb, LV_EVENT_FOCUSED, NULL);

    s_ta_fd_section = lv_textarea_create(panel);
    lv_obj_set_size(s_ta_fd_section, 140, 50);
    lv_obj_align_to(s_ta_fd_section, s_ta_fd_class, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_textarea_set_one_line(s_ta_fd_section, true);
    lv_textarea_set_max_length(s_ta_fd_section, 3);
    lv_textarea_set_placeholder_text(s_ta_fd_section, "EMA");
    lv_obj_set_style_text_font(s_ta_fd_section, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_fd_section);
    lv_obj_set_style_text_color(s_ta_fd_section, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_ta_fd_section, ta_focused_top_cb, LV_EVENT_FOCUSED, NULL);

    // --- Fox/Hound (DXpedition) ---------------------------------------
    // A ladder, not a switch, so it is a dropdown: off / guided / automatic.
    // See ft8_hound.h - it suspends three of our politeness rules (the
    // busy-station hold, the final re-send, the grey-list), which is exactly why
    // it is a mode the operator turns on deliberately rather than something
    // inferred from a low-frequency CQ.
    // Placed on the FIELD DAY row's right-hand side, not at (540,578) where it
    // first went: the "Auto-answer CQ with priority:" label plus its 280 px
    // dropdown reach to about x=712, so the two overlapped on screen (operator
    // caught it immediately - the labels ran into each other). The usable gap
    // here is x 545..840, because the right-edge buttons start at 840
    // (1040 panel - 20 pad - 180 button), hence the short label and a 150 px
    // dropdown with short options.
    // Aligned to the Field Day row and anchored to the SECTION box rather than to
    // absolute coordinates, so it stays level with that row and clear of the box
    // even if either moves. Two absolute-position attempts got this wrong: (540,
    // 578) landed on the priority dropdown, and (545, 648) ran into the "EMA" box.
    //
    // There is room to the right here because the Save/Cancel/Sync Time buttons
    // only occupy y 38..324 - below that the panel's full width is free.
    lv_obj_t *lbl_hound = lv_label_create(panel);
    lv_label_set_text(lbl_hound, "Fox/Hound:");
    lv_obj_set_style_text_font(lbl_hound, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_hound, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    // Order MUST match ft8_hound_mode_t (OFF=0, GUIDED=1, AUTO=2).
    s_dd_hound = lv_dropdown_create(panel);
    lv_dropdown_set_options(s_dd_hound, "Off\nGuided\nAutomatic");
    lv_obj_set_width(s_dd_hound, 200);

    // The DROPDOWN is aligned to the section box, and the LABEL hangs off the
    // dropdown - not the other way round. Aligning the label first centred the
    // dropdown on the LABEL'S text height instead of on the box, which is why it
    // still sat a few pixels low (operator, twice). Box and dropdown centres now
    // coincide by construction.
    lv_obj_align_to(s_dd_hound, s_ta_fd_section, LV_ALIGN_OUT_RIGHT_MID, 190, 0);
    lv_obj_align_to(lbl_hound, s_dd_hound, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_set_style_text_font(s_dd_hound, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(s_dd_hound, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_border_color(s_dd_hound, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_text_color(s_dd_hound, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_add_event_cb(s_dd_hound, robot_pri_dropdown_open_cb, LV_EVENT_CLICKED, NULL);


    // --- Save / Cancel / Sync Time on the right edge, stacked at the top.
    // Same placement as the Waveshare P4 port: Save lines up with the first
    // include text field (y=38) and the three stack down 120 px apart - this
    // frees the lower-right for the unattended-TX warning above the
    // Auto-work-pileup row (was evenly distributed down the whole edge).
    {
        struct { const char *lbl; uint32_t col; lv_event_cb_t cb; } btns[3] = {
            { "Save",      0x2e8b3a, save_btn_cb      },
            { "Cancel",    0x962020, cancel_btn_cb     },
            { "Sync Time", UI_COLOR_PRIMARY, sync_time_btn_cb },
        };
        // Tied to the FOUR text rows rather than evenly spaced (the even 120 px
        // spacing left them looking unrelated to anything).
        //
        // MEASURED FROM THE WIDGETS, not from numbers typed here. Two rounds of
        // hand arithmetic off the row constants were still visibly out by a few
        // pixels - the operator spotted both - because the constants are the
        // textareas' align OFFSETS, and the panel's own padding and the widgets'
        // real heights also count. lv_obj_update_layout() forces LVGL to compute
        // the geometry so we can just read it, which cannot be off by anything.
        //   Save      top edge level with the first row
        //   Cancel    centred on the whole four-row block
        //   Sync Time bottom edge level with the last row
        lv_obj_update_layout(panel);
        const int blk_top = lv_obj_get_y(s_ta_incl[0]);
        const int blk_bot = lv_obj_get_y(s_ta_excl[1]) + lv_obj_get_height(s_ta_excl[1]);
        const int bh      = 64;
        const int btn_y[3] = {
            blk_top,
            (blk_top + blk_bot) / 2 - bh / 2,
            blk_bot - bh,
        };
        lv_obj_t *save_b = NULL, *cancel_b = NULL;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = lv_btn_create(panel);
            lv_obj_set_size(b, 180, 64);
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, btn_y[i]);
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
            else if (i == 2) s_sync_time_btn = b;  // Store "Sync Time" button for mode-specific show/hide
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
    apply_checkbox_state(s_cb_skip_tx1, f->skip_tx1);
    apply_checkbox_state(s_cb_robot, f->robot_en);
    lv_dropdown_set_selected(s_dd_robot_pri,
                             f->robot_priority <= FT8_ROBOT_PRI_DISTANT ? f->robot_priority : 0);
    apply_checkbox_state(s_cb_auto_pileup, f->auto_pileup);
    apply_checkbox_state(s_cb_greylist, s.greylist_en);
    lv_dropdown_set_selected(s_dd_hound, s.hound_mode <= 2 ? s.hound_mode : 0);
    update_unattended_warn();   // shows the warning if robot OR auto-pileup is on
    apply_checkbox_state(s_cb_field_day, s.field_day_en);
    lv_textarea_set_text(s_ta_fd_class, s.fd_class);
    lv_textarea_set_text(s_ta_fd_section, s.fd_section);

    // "Sync Time" used to be hidden in FT4 mode because the timing-offset calc
    // was silently wrong for FT4's block geometry; fixed 2026-06-30
    // (decode_candidate_range() now reads mon->block_size/subblock_size
    // instead of hardcoded FT8 constants), so it's shown in both modes now.
    if (s_sync_time_btn) {
        lv_obj_clear_flag(s_sync_time_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    ESP_LOGI(TAG, "filter modal shown");
}
