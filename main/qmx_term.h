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
 * fixed number of cursor keys, so it still works if the menu changes.
 *
 * A session also closes ITSELF after two minutes with no activity. That is not
 * housekeeping: a browser tab can close, WiFi can drop, a laptop can go flat,
 * and the watchdog is then the only thing left that can walk the radio out of
 * terminal mode. Every screen read and keystroke counts as activity. */
void qmx_term_close(void);

bool qmx_term_is_open(void);

/* Send a keystroke. Names: "up", "down", "left", "right", "enter", "esc",
 * "ctrl-q", "bksp" (0x08 BS), "del" (0x7F DEL), or a single printable character.
 *
 * BS and DEL are deliberately separate: a terminal app can want either, the QMX
 * manual does not say which, and Randy N4OPI found neither of them working when
 * only BS was offered. Do not collapse them until it is known which one the
 * radio actually acts on. */
bool qmx_term_key(const char *name);

/* Borrow the screen for reading. Returns NULL when no session is open (or the
 * lock could not be had), otherwise a pointer that stays valid until the
 * matching unlock - which the caller MUST make, promptly, because the USB RX
 * callback blocks on the same lock while it is held.
 *
 * Read it in place rather than copying: ansi_term_t is ~4 KB, and a local that
 * size is a stack-protection fault on most tasks on this board. `dirty_seq`
 * lets a UI skip a redraw when nothing has changed. */
const ansi_term_t *qmx_term_lock_screen(void);
void qmx_term_unlock_screen(void);
