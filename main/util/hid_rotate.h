#pragma once
/* Landscape cursor position -> the point to hand LVGL's pointer indev.
 *
 * Portable on purpose (no ESP/LVGL deps) so test/hid_rotate_harness.c can link
 * the REAL function and check it against a verbatim copy of LVGL's own forward
 * map. That round-trip is the only verification available: a pointer transform
 * cannot be judged from a log line, and this one has now been wrong twice.
 *
 * WHY IT EXISTS AT ALL. The panel is natively portrait and LVGL software-rotates
 * it to landscape, so LVGL also rotates every indev point it is given
 * (lv_indev.c indev_pointer_proc). hid_cursor.c accumulates the cursor in
 * LANDSCAPE SCREEN space, which is what every hit-test downstream expects - so
 * this hands LVGL the INVERSE, the two cancel, and (lx,ly) reaches the screen
 * unchanged.
 *
 * ⛔ AND THE INVERSE IS DIFFERENT FOR THE TWO ROTATIONS. The "Flip 180" setting
 * (display_set_flipped) moves the display from ROTATION_90 to ROTATION_270 for
 * upside-down mounting. Touch follows automatically because the touch driver is
 * fed raw panel coordinates; the mouse does NOT, because it is fed screen
 * coordinates and this map is the only thing that knows about rotation. Feeding
 * the 90 inverse to a 270 display point-reflects the cursor: it appears at
 * (W-1-lx, H-1-ly) and travels backwards on both axes. Reported from the field,
 * 2026-08-31: "on flip 180 the mouse do not flip".
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HID_ROT_90  = 0,   /* normal landscape          */
    HID_ROT_270 = 1,   /* "Flip 180" - upside down  */
} hid_rot_t;

/* (lx, ly) are LANDSCAPE SCREEN coordinates: 0..scr_w-1 by 0..scr_h-1, i.e.
 * 1280 x 720 here. Pass scr_w/scr_h as LVGL reports them for the ROTATED
 * display (lv_display_get_horizontal_resolution / _vertical_resolution, which
 * return 1280 / 720 for both 90 and 270).
 *
 * Outputs are what to put in lv_indev_data_t.point. Unknown rotations fall back
 * to HID_ROT_90 rather than leaving the outputs undefined - a mispositioned
 * cursor is recoverable, an uninitialised one is not. */
void hid_rotate_to_indev(hid_rot_t rot, int32_t scr_w, int32_t scr_h,
                         int32_t lx, int32_t ly,
                         int32_t *out_x, int32_t *out_y);

#ifdef __cplusplus
}
#endif
