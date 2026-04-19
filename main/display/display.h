#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#define DISPLAY_H_RES   720
#define DISPLAY_V_RES   1280

esp_err_t display_init(lv_display_t **out_disp);

// Thread-safe LVGL access (wrappers around BSP)
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);