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

// Feed one key from the Tab5's snap-on keyboard into the radio's menus
// (Don N2VGU). Takes the keyboard bridge's own tokens - a single character, or
// a spelled-out name like "up"/"enter"/"backspace". Returns true if the key was
// consumed, false if this screen is not up or the radio has no use for that key,
// so the caller can fall through to its normal handling.
bool qmx_term_view_key(const char *token);

// DEV ONLY: show/hide the on-screen keyboard (see the .c for why).
void qmx_term_view_set_keyboard(bool shown);
