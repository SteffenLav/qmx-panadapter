#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the hidden 80x80 top-left long-press capture region on the given
// parent (typically lv_screen_active()). Also sets the global long-press time
// on the BSP indev to 1000 ms.
void screenshot_init(lv_obj_t *parent);
lv_obj_t *screenshot_get_btn(void);

#ifdef __cplusplus
}
#endif