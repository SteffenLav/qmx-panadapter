#pragma once
/* The QMX's own menu system, on the Tab5's screen (#147).
 *
 * For a QMX+ with no control panel this is the only way into the radio's menus
 * at all, and unlike the web page it needs no computer - which is the whole
 * point at a POTA site. The session itself lives in qmx_term.c; this is the
 * 80x24 grid drawn with LVGL, plus the keys.
 */
#include <stdbool.h>

void qmx_term_view_open(void);
void qmx_term_view_close(void);
bool qmx_term_view_is_open(void);

// DEV ONLY: show/hide the on-screen keyboard (see the .c for why).
void qmx_term_view_set_keyboard(bool shown);
