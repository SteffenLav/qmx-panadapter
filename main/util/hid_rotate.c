/* See hid_rotate.h for the derivation and why it is portable. */
#include "hid_rotate.h"

void hid_rotate_to_indev(hid_rot_t rot, int32_t scr_w, int32_t scr_h,
                         int32_t lx, int32_t ly,
                         int32_t *out_x, int32_t *out_y)
{
    int32_t x, y;

    if (rot == HID_ROT_270) {
        /* LVGL applies BOTH of its blocks for 270. Composed, using raw panel
         * dims hor_res = scr_h and ver_res = scr_w, the forward map is
         *     out.x = in.y
         *     out.y = scr_h - 1 - in.x
         * so the inverse that lands (lx, ly) is: */
        x = (scr_h - 1) - ly;
        y = lx;
    } else {
        /* Forward for 90 is
         *     out.x = scr_w - 1 - in.y
         *     out.y = in.x
         * ⚠ Note this one needs the screen WIDTH where 270 needs the HEIGHT.
         * Using the wrong one still cancels along one axis, which is exactly
         * how the earlier "invisible wall at x = 1280 - 720 = 560" happened. */
        x = ly;
        y = (scr_w - 1) - lx;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}
