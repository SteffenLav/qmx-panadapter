#pragma once
#include "lvgl.h"

// Step 4c.2 v0.10: FT8 screen LVGL view.
//
// Owns the middle-band container shown when ui_mode == UI_MODE_FT8.
// Top bar + bottom bar are shared with panadapter and remain
// visible across mode swaps. ft8_screen.c remains the data layer;
// this file only reads via ft8_screen_get_all().
//
// Refresh model:
//   - ft8_task on core 1 calls request_refresh() at slot end
//   - request_refresh() sets a volatile flag (no LVGL calls)
//   - An LVGL timer at 500 ms (runs on LVGL task / core 0) reads
//     the flag, snapshots the table, sorts by last_utc desc, and
//     rebuilds the lv_list.
//   - A second 1 Hz timer updates UTC clock + slot countdown +
//     dial freq label, independent of decode activity.

void ft8_screen_view_init(lv_obj_t *parent);
void ft8_screen_view_show(void);
void ft8_screen_view_hide(void);

// Returns the FT8 screen's middle-band container, for slide animations
// driven by ui.c. May return NULL before ft8_screen_view_init().
lv_obj_t *ft8_screen_view_get_container(void);

// Safe to call from any task. Just sets a volatile flag.
void ft8_screen_view_request_refresh(void);

// Refresh the "Call CQ" button label to the currently-selected CQ preset.
// Called by the CQ preset modal after a save. LVGL-thread only.
void ft8_screen_view_refresh_cq_label(void);
