/* Drag a panel with a finger or the mouse (T204, Pete WK8S 2026-11-16: the
 * log window covered the next signal he wanted to work).
 *
 * A press on the panel itself - its background or a label on it - moves it.
 * Its fields and buttons keep their own presses, so typing and tapping are
 * unchanged. It stays inside its parent. LVGL task only. */
#pragma once
#include "lvgl.h"

/* Make obj draggable. Converts its alignment to TOP_LEFT at its current
 * place first: a TOP_MID-aligned panel would read the new x as an offset
 * from the centre and jump. */
void ui_drag_enable(lv_obj_t *obj);

/* Move obj up, if needed, so its bottom edge is at or above y_max (parent
 * coordinates) - for an on-screen keyboard that is about to cover it. */
void ui_drag_keep_above(lv_obj_t *obj, int32_t y_max);
