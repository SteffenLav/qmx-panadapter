/* Drag a panel - see ui_drag.h. T204.
 *
 * ⛔ ABSOLUTE, NOT ADDED UP. The first version added each read's step
 * (lv_indev_get_vect) to lv_obj_get_x(). That getter reads the coordinates
 * of the last LAYOUT, so two moves between redraws read the same old x and
 * one step was lost: on bench1 the window trailed the finger ("much slower
 * than the finger moves", operator 2026-11-16). Now the window goes to where
 * it was at the press plus how far the finger has gone since. */
#include "ui_drag.h"

static lv_point_t s_press_pt;     /* finger at the press */
static int32_t    s_press_x, s_press_y;   /* panel at the press */

static void clamp_and_place(lv_obj_t *o, int32_t x, int32_t y)
{
    lv_obj_t *p = lv_obj_get_parent(o);
    int32_t maxx = lv_obj_get_width(p) - lv_obj_get_width(o);
    int32_t maxy = lv_obj_get_height(p) - lv_obj_get_height(o);
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    lv_obj_set_pos(o, x, y);
}

static void drag_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_press_pt = p;
        s_press_x = lv_obj_get_style_x(o, LV_PART_MAIN);   /* what we set, not the last layout */
        s_press_y = lv_obj_get_style_y(o, LV_PART_MAIN);
        return;
    }
    clamp_and_place(o, s_press_x + (p.x - s_press_pt.x), s_press_y + (p.y - s_press_pt.y));
}

void ui_drag_enable(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_update_layout(obj);
    int32_t x = lv_obj_get_x(obj), y = lv_obj_get_y(obj);
    lv_obj_set_align(obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(obj, x, y);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSING, NULL);
}

void ui_drag_keep_above(lv_obj_t *obj, int32_t y_max)
{
    if (!obj) return;
    int32_t y = lv_obj_get_style_y(obj, LV_PART_MAIN), h = lv_obj_get_height(obj);
    if (y + h > y_max) clamp_and_place(obj, lv_obj_get_style_x(obj, LV_PART_MAIN), y_max - h);
}
