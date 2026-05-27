#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

// Phase 6.3: native portrait 720x1280.  LVGL screen is 720 wide x 1280 tall.
// DISPLAY_H_RES=1280 = portrait height (= landscape width), used for frequency axis.
// DISPLAY_V_RES=720  = portrait width  (= landscape height), used for bar widths.
#define DISPLAY_H_RES   1280
#define DISPLAY_V_RES   720

esp_err_t display_init(lv_display_t **out_disp);

// Thread-safe LVGL access (wrappers around BSP)
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);
