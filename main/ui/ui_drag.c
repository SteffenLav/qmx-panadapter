/* Drag a panel - see ui_drag.h. T204. */
#include "ui_drag.h"

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
    lv_point_t v;
    lv_indev_get_vect(in, &v);
    if (v.x == 0 && v.y == 0) return;
    clamp_and_place(o, lv_obj_get_x(o) + v.x, lv_obj_get_y(o) + v.y);
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
    lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSING, NULL);
}

void ui_drag_keep_above(lv_obj_t *obj, int32_t y_max)
{
    if (!obj) return;
    int32_t y = lv_obj_get_y(obj), h = lv_obj_get_height(obj);
    if (y + h > y_max) clamp_and_place(obj, lv_obj_get_x(obj), y_max - h);
}
