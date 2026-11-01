#pragma once

/* Montserrat Medium with Latin-1, for text that can carry æøå and friends -
 * station names and ICY programme text. See font_ui_latin_24.c for why the
 * built-in montserrat fonts cannot, and for the regenerate line.
 *
 * ⛔ NO LV_SYMBOL GLYPHS in these. A label that shows LV_SYMBOL_* must use a
 * built-in lv_font_montserrat_*. */

#include "lvgl.h"

LV_FONT_DECLARE(font_ui_latin_20);
LV_FONT_DECLARE(font_ui_latin_24);
LV_FONT_DECLARE(font_ui_latin_28);
