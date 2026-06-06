// Memory channels modal - 4x8 grid of frequency/mode/label slots.
// Tap occupied = recall (commit 3). Long-press = save/edit (commit 4-5).
#include "memory_modal.h"
#include "mem_channels.h"
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
#define CELL_H        58
#define CELL_GAP       6
#define COLS           4
#define ROWS           8

static lv_obj_t *s_modal      = NULL;
static lv_obj_t *s_panel      = NULL;
static lv_obj_t *s_grid       = NULL;
static lv_obj_t *s_cell_btn[MEM_SLOTS];
static lv_obj_t *s_cell_lbl[MEM_SLOTS];
static bool      s_open       = false;

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void memory_modal_refresh(void)
{
    for (int i = 0; i < MEM_SLOTS; i++) {
        mem_slot_t slot;
        mem_channels_get(i, &slot);

        lv_obj_t *btn = s_cell_btn[i];
        lv_obj_t *lbl = s_cell_lbl[i];
        if (!btn || !lbl) continue;

        if (slot.occupied) {
            uint32_t mhz = slot.freq_hz / 1000000UL;
            uint32_t khz = (slot.freq_hz % 1000000UL) / 1000UL;
            char buf[40];
            if (slot.label[0]) {
                snprintf(buf, sizeof(buf), "%lu.%03lu\n%s %s",
                         (unsigned long)mhz, (unsigned long)khz,
                         slot.mode, slot.label);
            } else {
                snprintf(buf, sizeof(buf), "%lu.%03lu\n%s",
                         (unsigned long)mhz, (unsigned long)khz, slot.mode);
            }
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a3a5a), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "[%02d]", i + 1);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x606060), 0);
        }
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

    /* Scrollable grid container */
    int grid_w = COLS * CELL_W + (COLS - 1) * CELL_GAP;
    int grid_h = PANEL_H - PAD * 2 - TITLE_H - 8 - 56 - 8;  /* panel - pad - title - gap - close - gap */
    s_grid = lv_obj_create(s_panel);
    lv_obj_set_size(s_grid, grid_w, grid_h);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, TITLE_H + 8);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);

    /* 32 cell buttons */
    for (int i = 0; i < MEM_SLOTS; i++) {
        int col = i % COLS;
        int row = i / COLS;
        int x   = col * (CELL_W + CELL_GAP);
        int y   = row * (CELL_H + CELL_GAP);

        lv_obj_t *btn = lv_btn_create(s_grid);
        lv_obj_set_size(btn, CELL_W, CELL_H);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        s_cell_btn[i] = btn;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(lbl, CELL_W - 8, CELL_H - 8);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x606060), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        s_cell_lbl[i] = lbl;
    }

    /* Close button */
    lv_obj_t *close_btn = lv_btn_create(s_panel);
    lv_obj_set_size(close_btn, grid_w, 48);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(close_lbl);

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
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    ESP_LOGI(TAG, "shown");
}
