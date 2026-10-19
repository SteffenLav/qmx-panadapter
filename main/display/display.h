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
/* Takes the LVGL/display lock. The task name alone was not enough to act on:
 * "held 90 ms by 'status'" says which thread, not which of the dozen ui_*
 * entry points that thread calls was holding it. __func__ at the call site
 * costs a pointer, so every one of the 54 existing call sites now reports
 * itself with no edit - the macro below rewrites them all.
 *
 * ⛔ display.c must #undef this before defining display_lock_tagged. */
bool display_lock_tagged(uint32_t timeout_ms, const char *site);
#define display_lock(t) display_lock_tagged((t), __func__)
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

// Frames per second x10 since the previous call (LV_EVENT_REFR_READY count).
// The only numeric measure of panadapter smoothness this project has - added
// 2026-08-28 because the operator was being asked to judge 2-point differences
// by eye, which is not something eyes can do.
unsigned display_fps_x10(void);

/* Frame-INTERVAL spread, which is what "jaggy" actually means - see the long
 * comment at disp_refr_ready_cb(). fps alone cannot show it: a 10 s average
 * hides the millisecond spacing. Reading it resets the window. */
typedef struct {
    uint32_t n;        /* intervals measured in this window */
    uint32_t min_us;
    uint32_t mean_us;
    uint32_t max_us;
    uint32_t late;     /* intervals over ~2x nominal */
    uint32_t draw_mean_us; /* REFR_START -> REFR_READY: how long the draw took */
    uint32_t draw_max_us;
} display_frame_spacing_t;
void display_frame_spacing(display_frame_spacing_t *out);

// Thousands of invalidated pixels per second since the previous call. Read
// together with the fps figure: frames say how OFTEN, this says how MUCH.
unsigned display_inval_kpx_per_s(void);

/* Invalidated pixels split by panel, so the total names a culprit instead of
 * just a size. Bands, top to bottom: top bar, spectrum, label bar, waterfall,
 * band plan, bottom bar. See the band table in display.c - it MIRRORS ui.c's
 * layout #defines. */
typedef struct {
    unsigned kpx_per_s[6];
    unsigned reqs[6];   /* requests per band - separates "big" from "often" */
    unsigned events;      /* invalidation requests in the window */
    unsigned fullscreen;  /* of those, requests covering >=90% of the panel */
    unsigned max_px;    /* largest single request; 921600 means full screen */
} display_inval_bands_t;
void display_inval_bands(display_inval_bands_t *out);

