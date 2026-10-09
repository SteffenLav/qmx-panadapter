#pragma once

#include <stdbool.h>

/* The JS8 conversation page as an LVGL overlay.
 *
 * Opens over whatever is on screen, same shape as the GPS page: a full-screen
 * overlay with a Close button and the 80x24 qmx_mono_25 grid. The layout is
 * js8_page.c and is tested on a PC; this file is the LVGL around it.
 */
void js8_page_view_open(void);
void js8_page_view_close(void);
bool js8_page_view_is_open(void);
