// Memory channels modal - 4x8 grid of frequency/mode/label slots.
// Tap occupied = recall. Long-press occupied = Edit/Delete/Cancel.
#include "memory_modal.h"
#include "ui_theme.h"
#include "mem_channels.h"
#include "cat.h"
#include "ui.h"
#include "display.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "mem_modal";

#define PANEL_W     1200
#define PANEL_H      660
#define PAD           16
#define TITLE_H       48
#define CELL_W       282
#define CELL_H        64
#define CELL_GAP       6
#define COLS           4
#define ROWS           8

static lv_obj_t *s_modal       = NULL;
static lv_obj_t *s_panel       = NULL;
static lv_obj_t *s_grid        = NULL;
static lv_obj_t *s_cell_btn[MEM_SLOTS];
static lv_obj_t *s_cell_lbl[MEM_SLOTS];   /* top line: memory name      */
static lv_obj_t *s_cell_lbl2[MEM_SLOTS];  /* bottom line: mode + freq   */
static bool      s_open        = false;

/* Action menu for long-press occupied */
static lv_obj_t *s_action_panel = NULL;
static lv_obj_t *s_action_ta    = NULL;  /* label edit textarea */
static lv_obj_t *s_action_kb    = NULL;
static int       s_action_idx   = -1;

/* Frequency confirmed/edited via the freq keypad before naming a slot */
static uint32_t  s_pending_freq_hz = 0;
static char      s_pending_mode[8] = "";

#define MODAL_SLIDE_TIME_MS 250

static int s_drag_start_y = -1;
#define DRAG_CLOSE_MIN_DY 60

static void modal_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void modal_close_ready_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_modal, 0);
}

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    s_open = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_modal);
    lv_anim_set_exec_cb(&a, modal_anim_y_cb);
    lv_anim_set_values(&a, 0, DISPLAY_V_RES);
    lv_anim_set_time(&a, MODAL_SLIDE_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, modal_close_ready_cb);
    lv_anim_start(&a);
}

// Swipe down (drag down) anywhere on the modal background closes it,
// replacing the Close button.
static void modal_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_drag_start_y = (int)p.y;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_drag_start_y >= 0 &&
            (int)p.y - s_drag_start_y >= DRAG_CLOSE_MIN_DY) {
            modal_close();
        }
        s_drag_start_y = -1;
    }
}

/* Format hz as "12.345.678 Hz" (dot every 3 digits from the right). */
static void format_freq_hz(uint32_t hz, char *out, size_t out_sz)
{
    char digits[12];
    snprintf(digits, sizeof(digits), "%lu", (unsigned long)hz);
    size_t len = strlen(digits);

    int oi = 0;
    for (size_t i = 0; i < len && (size_t)oi < out_sz - 1; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[oi++] = '.';
        if ((size_t)oi < out_sz - 1) out[oi++] = digits[i];
    }
    out[oi] = '\0';
    snprintf(out + oi, out_sz - (size_t)oi, " Hz");
}

static void memory_modal_refresh(void)
{
    for (int i = 0; i < MEM_SLOTS; i++) {
        mem_slot_t slot;
        mem_channels_get(i, &slot);

        lv_obj_t *btn  = s_cell_btn[i];
        lv_obj_t *lbl  = s_cell_lbl[i];
        lv_obj_t *lbl2 = s_cell_lbl2[i];
        if (!btn || !lbl || !lbl2) continue;

        if (slot.occupied) {
            char freq_str[20];
            format_freq_hz(slot.freq_hz, freq_str, sizeof(freq_str));

            char buf2[32];
            snprintf(buf2, sizeof(buf2), "%s   %s", slot.mode, freq_str);

            lv_label_set_text(lbl, slot.label[0] ? slot.label : freq_str);
            lv_label_set_text(lbl2, slot.label[0] ? buf2 : slot.mode);
            lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "[%02d]", i + 1);
            lv_label_set_text(lbl, buf);
            lv_label_set_text(lbl2, "");
            lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        }
    }
}

/* Action menu callbacks */
static void action_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    s_action_idx = -1;
}

static void action_ta_focused_cb(lv_event_t *e)
{
    (void)e;

    if (!s_action_kb) return;
    lv_keyboard_set_textarea(s_action_kb, s_action_ta);
    lv_obj_clear_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_action_kb);
}

static void action_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
}

static void action_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_action_idx < 0) return;
    mem_channels_clear(s_action_idx);
    ESP_LOGI(TAG, "deleted slot %d", s_action_idx);
    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    s_action_idx = -1;
    memory_modal_refresh();
}

static void action_save_cb(lv_event_t *e)
{
    (void)e;
    if (s_action_idx < 0) return;
    mem_slot_t slot = {0};
    mem_channels_get(s_action_idx, &slot);

    if (s_pending_freq_hz == 0) {
        ESP_LOGW(TAG, "save: no freq");
        return;
    }
    slot.freq_hz = s_pending_freq_hz;
    slot.occupied = 1;
    strncpy(slot.mode, s_pending_mode[0] ? s_pending_mode : "???", sizeof(slot.mode) - 1);
    slot.mode[sizeof(slot.mode) - 1] = '\0';

    /* Both new and edit: update label from textarea */
    const char *new_label = lv_textarea_get_text(s_action_ta);
    if (new_label) strncpy(slot.label, new_label, sizeof(slot.label) - 1);

    mem_channels_set(s_action_idx, &slot);
    ESP_LOGI(TAG, "saved slot %d: %lu Hz %s '%s'",
             s_action_idx, (unsigned long)slot.freq_hz, slot.mode, slot.label);

    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    s_action_idx = -1;
    memory_modal_refresh();
}

// cat_set_frequency()/cat_set_mode() share a 200ms TX rate-limiter; firing
// both back-to-back drops the mode write silently. Send the mode on a
// one-shot timer so it lands after the frequency write's rate-limit window.
static char s_recall_mode[8] = "";

static void recall_mode_timer_cb(lv_timer_t *t)
{
    if (s_recall_mode[0]) cat_set_mode(s_recall_mode);
    lv_timer_del(t);
}

static void cell_tap_cb(lv_event_t *e)
{
    if (s_action_idx >= 0) return;  /* suppress tap if long-press action panel is open */
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= MEM_SLOTS) return;

    mem_slot_t slot;
    mem_channels_get(idx, &slot);
    if (!slot.occupied) return;

    ESP_LOGI(TAG, "recall slot %d: %lu Hz %s '%s'",
             idx, (unsigned long)slot.freq_hz, slot.mode, slot.label);

    cat_set_frequency(slot.freq_hz);
    if (slot.mode[0]) {
        strncpy(s_recall_mode, slot.mode, sizeof(s_recall_mode) - 1);
        s_recall_mode[sizeof(s_recall_mode) - 1] = '\0';
        lv_timer_t *t = lv_timer_create(recall_mode_timer_cb, 250, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
    modal_close();
}

static void show_action_panel(int idx)
{
    mem_slot_t slot;
    mem_channels_get(idx, &slot);

    lv_obj_t *ttl = lv_obj_get_child(s_action_panel, 0);

    if (slot.occupied) {
        /* Edit occupied slot */
        char title[32];
        snprintf(title, sizeof(title), "M%02d: %s", idx + 1,
                 slot.label[0] ? slot.label : slot.mode);
        if (ttl) lv_label_set_text(ttl, title);
        lv_textarea_set_text(s_action_ta, slot.label);
    } else {
        /* Save new slot */
        char title[32];
        snprintf(title, sizeof(title), "M%02d: New", idx + 1);
        if (ttl) lv_label_set_text(ttl, title);
        lv_textarea_set_text(s_action_ta, "");
    }

    ui_theme_focus_textarea(s_action_ta);
    lv_obj_clear_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_action_panel);
    lv_obj_clear_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_action_kb, s_action_ta);
    lv_obj_move_foreground(s_action_kb);
}

/* Called after the frequency keypad is confirmed/cancelled. */
static void mem_freq_picker_cb(uint32_t freq_hz, const char *mode, bool accepted)
{
    if (!accepted || s_action_idx < 0) {
        s_action_idx = -1;
        return;
    }
    s_pending_freq_hz = freq_hz;
    strncpy(s_pending_mode, mode && mode[0] ? mode : "???", sizeof(s_pending_mode) - 1);
    s_pending_mode[sizeof(s_pending_mode) - 1] = '\0';
    show_action_panel(s_action_idx);
}

static void cell_longpress_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= MEM_SLOTS) return;

    mem_slot_t slot;
    mem_channels_get(idx, &slot);
    s_action_idx = idx;

    /* Pre-fill the freq keypad: existing slot's freq+mode when editing, or
     * the QMX's current VFO freq+mode when saving a new slot. The user can
     * change either before naming the slot; the chosen freq+mode are
     * carried through to action_save_cb via mem_freq_picker_cb. */
    uint32_t initial_hz;
    const char *mode;
    if (slot.occupied) {
        initial_hz = slot.freq_hz;
        mode = slot.mode;
    } else {
        initial_hz = cat_get_frequency();
        mode = cat_get_mode_str();
    }

    ui_freq_picker_open(initial_hz, mode, mem_freq_picker_cb);
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
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Memory Channels");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    int grid_w = COLS * CELL_W + (COLS - 1) * CELL_GAP;
    int grid_h = PANEL_H - PAD * 2 - TITLE_H - 8;
    s_grid = lv_obj_create(s_panel);
    lv_obj_set_size(s_grid, grid_w, grid_h);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, TITLE_H + 8);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);

    for (int i = 0; i < MEM_SLOTS; i++) {
        int col = i % COLS;
        int row = i / COLS;
        int x   = col * (CELL_W + CELL_GAP);
        int y   = row * (CELL_H + CELL_GAP);

        lv_obj_t *btn = lv_btn_create(s_grid);
        lv_obj_set_size(btn, CELL_W, CELL_H);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cell_tap_cb,      LV_EVENT_CLICKED,     NULL);
        lv_obj_add_event_cb(btn, cell_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
        s_cell_btn[i] = btn;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(lbl, CELL_W - 8, 28);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 2);
        s_cell_lbl[i] = lbl;

        lv_obj_t *lbl2 = lv_label_create(btn);
        lv_label_set_long_mode(lbl2, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(lbl2, CELL_W - 8, 26);
        lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lbl2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_align(lbl2, LV_ALIGN_BOTTOM_MID, 0, -2);
        s_cell_lbl2[i] = lbl2;
    }

    // Slim grip handle at the top of the panel: swipe down to close.
    lv_obj_t *grip = lv_obj_create(s_panel);
    lv_obj_set_size(grip, 120, 10);
    lv_obj_align(grip, LV_ALIGN_TOP_MID, 0, -PAD + 4);
    lv_obj_set_style_bg_color(grip, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_30, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_radius(grip, 5, 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);

    // Swipe down anywhere on the modal background (outside the grid) closes it.
    lv_obj_add_event_cb(s_modal, modal_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_modal, modal_swipe_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_panel, modal_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_panel, modal_swipe_cb, LV_EVENT_RELEASED, NULL);

    /* Action panel for long-press */
    s_action_panel = lv_obj_create(s_panel);
    lv_obj_set_size(s_action_panel, 600, 280);
    lv_obj_align(s_action_panel, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_color(s_action_panel, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_border_color(s_action_panel, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_style_border_width(s_action_panel, 2, 0);
    lv_obj_set_style_radius(s_action_panel, 10, 0);
    lv_obj_set_style_pad_all(s_action_panel, 12, 0);
    lv_obj_clear_flag(s_action_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *act_title = lv_label_create(s_action_panel);
    lv_label_set_text(act_title, "");
    lv_obj_set_style_text_color(act_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(act_title, &lv_font_montserrat_24, 0);
    lv_obj_align(act_title, LV_ALIGN_TOP_MID, 0, 0);

    s_action_ta = lv_textarea_create(s_action_panel);
    lv_obj_set_size(s_action_ta, 560, 60);
    lv_obj_align(s_action_ta, LV_ALIGN_TOP_MID, 0, 32);
    lv_textarea_set_one_line(s_action_ta, true);
    lv_textarea_set_max_length(s_action_ta, 15);
    lv_obj_set_style_text_font(s_action_ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_action_ta);
    lv_obj_add_event_cb(s_action_ta, action_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *act_cancel = lv_btn_create(s_action_panel);
    lv_obj_set_size(act_cancel, 160, 56);
    lv_obj_align(act_cancel, LV_ALIGN_BOTTOM_LEFT, 20, -12);
    lv_obj_set_style_bg_color(act_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(act_cancel, 8, 0);
    lv_obj_add_event_cb(act_cancel, action_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(act_cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *act_delete = lv_btn_create(s_action_panel);
    lv_obj_set_size(act_delete, 160, 56);
    lv_obj_align(act_delete, LV_ALIGN_BOTTOM_LEFT, 190, -12);
    lv_obj_set_style_bg_color(act_delete, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(act_delete, 8, 0);
    lv_obj_add_event_cb(act_delete, action_delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *delete_lbl = lv_label_create(act_delete);
    lv_label_set_text(delete_lbl, "Delete");
    lv_obj_set_style_text_color(delete_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(delete_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(delete_lbl);

    lv_obj_t *act_save = lv_btn_create(s_action_panel);
    lv_obj_set_size(act_save, 160, 56);
    lv_obj_align(act_save, LV_ALIGN_BOTTOM_RIGHT, -20, -12);
    lv_obj_set_style_bg_color(act_save, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(act_save, 8, 0);
    lv_obj_add_event_cb(act_save, action_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(act_save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(save_lbl);

    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);

    s_action_kb = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_main;
    static lv_style_t style_kb_items;
    static bool kb_inited = false;
    if (!kb_inited) {
        /* Match the frequency keypad's look: dark panel, no border on keys,
         * grey key fill, small radius, even gaps between keys. */
        lv_style_init(&style_kb_main);
        lv_style_set_bg_color(&style_kb_main, lv_color_hex(UI_COLOR_SURFACE));
        lv_style_set_bg_opa(&style_kb_main, LV_OPA_COVER);
        lv_style_set_border_color(&style_kb_main, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&style_kb_main, 1);
        lv_style_set_radius(&style_kb_main, 10);
        lv_style_set_pad_all(&style_kb_main, 12);
        lv_style_set_pad_row(&style_kb_main, 8);
        lv_style_set_pad_column(&style_kb_main, 8);

        lv_style_init(&style_kb_items);
        lv_style_set_bg_color(&style_kb_items, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_items, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_items, lv_color_white());
        lv_style_set_border_width(&style_kb_items, 0);
        lv_style_set_radius(&style_kb_items, 6);
        kb_inited = true;
    }
    lv_obj_add_style(s_action_kb, &style_kb_main, 0);
    lv_obj_add_style(s_action_kb, &style_kb_items, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_action_kb);
    lv_obj_set_size(s_action_kb, LV_PCT(100), 280);
    lv_obj_align(s_action_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_action_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    ui_theme_keyboard_attach_caps_cycle(s_action_kb);
    lv_obj_set_style_text_font(s_action_kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_action_kb, action_kb_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_action_kb, action_kb_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "built");
}

void memory_modal_init(void)
{
    modal_build();
}

void memory_modal_show(void)
{
    modal_build();
    if (s_open) return;
    memory_modal_refresh();
    lv_obj_set_y(s_modal, DISPLAY_V_RES);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_modal);
    lv_anim_set_exec_cb(&a, modal_anim_y_cb);
    lv_anim_set_values(&a, DISPLAY_V_RES, 0);
    lv_anim_set_time(&a, MODAL_SLIDE_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "shown");
}
