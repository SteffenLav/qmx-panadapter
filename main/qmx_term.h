#pragma once
/* A terminal session on the QMX's SECOND serial port.
 *
 * WHY PORT 2. The QMX manual is explicit that closing a terminal session without
 * choosing "Exit terminal" leaves the radio refusing CAT commands. On the shared
 * port that would kill the panadapter. Measured 2026-08-16: with "USB serial
 * ports" set to 2 the radio's second CDC function starts at INTERFACE 5, our USB
 * host opens it, and CAT keeps polling on interface 0 throughout. So a session
 * here cannot take the panadapter down even if it ends badly.
 *
 * The extra port is off by default and is itself settable over CAT
 * (System config|GPS & Ser. ports|USB serial ports), which is what makes this
 * usable on a HEADLESS QMX+ - otherwise enabling the port would require the menu
 * that the terminal exists to reach.
 */
#include <stdbool.h>
#include "util/ansi_term.h"

/* Open port 2 and enter terminal mode (the radio switches on a bare CR).
 * Returns false if the port is not there - almost always because the radio is
 * still set to one serial port. */
bool qmx_term_open(void);

/* Leave terminal mode PROPERLY and close the port. Safe to call when not open.
 *
 * This drives the radio's own "Exit terminal" menu item rather than just
 * dropping the connection, because dropping it is precisely what the manual
 * warns against. It reads the screen to find the item instead of sending a
 * fixed number of cursor keys, so it still works if the menu changes. */
void qmx_term_close(void);

bool qmx_term_is_open(void);

/* Send a keystroke. Names: "up", "down", "left", "right", "enter", "esc",
 * "ctrl-q", or a single printable character. */
bool qmx_term_key(const char *name);

/* The current screen. Returns NULL when no session is open. The pointer stays
 * valid until qmx_term_close(); `seq` (ansi_term_t.dirty_seq) lets a caller skip
 * a redraw when nothing changed. */
const ansi_term_t *qmx_term_screen(void);
