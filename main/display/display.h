#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

// Phase 6.2: landscape orientation (rotated via LVGL software rotation)
// Panel is natively 720x1280 portrait, but we view it as 1280x720 landscape.
#define DISPLAY_H_RES   1280
#define DISPLAY_V_RES   720

esp_err_t display_init(lv_display_t **out_disp);

// Thread-safe LVGL access (wrappers around BSP)
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// Set LCD backlight brightness, 0..100 %.
void display_set_brightness(int percent);

// Fades the backlight 0->target_percent over 500ms. display_init() starts the
// backlight at 0 (not on) specifically so this can be called once, right
// after ui_init() returns, to reveal the app with a quick controlled fade
// instead of an instant white-panel flash followed by the real UI snapping
// in on top of it. Pass the user's saved brightness setting as the target -
// do not apply that setting anywhere earlier in boot (see ui_init()'s
// comment on why).
void display_fade_in_backlight(int target_percent);

// Flip the landscape view 180 degrees for upside-down mounting (false = normal,
// true = flipped). Persisted by the caller; safe to call at boot or at runtime.
void display_set_flipped(bool flipped);
bool display_is_flipped(void);
