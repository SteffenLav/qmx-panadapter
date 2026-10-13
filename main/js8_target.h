#pragma once

/* WHO THE OPERATOR IS WORKING IN JS8 - selected by tapping a station, and
 * transmitting NOTHING by itself.
 *
 * ⛔ WHY THIS EXISTS AT ALL. In FT8 a row tap means "work this station": the
 * Tab5 opens the TX confirmation modal and the browser's click arms the next
 * message outright (web_reply_drain, ft8_screen_view.c - "the browser already
 * confirmed"). That is right for FT8, where the exchange is a fixed ladder and
 * the next message is never in doubt.
 *
 * It is wrong for JS8, and two operators found it from opposite ends on
 * 2026-10-12/13:
 *
 *   - Ryan KK6DZB, holding a real QSO: "anytime I would click on a call it
 *     would auto start sending the SNR which is not handy in this mode. Works
 *     in FT8 :) but not so much in JS8 Call."
 *   - Gyula HA3HZ, from the far end: four stations answered automatically with
 *     signal reports while he watched, which is how the auto-QSO path reads to
 *     everyone else on the band.
 *
 * So in JS8 a tap SELECTS. The report and the grid go out only when the
 * operator presses SNR or GRID, and free text is addressed to whoever is
 * selected. One piece of state, both surfaces, no copy per surface - the same
 * rule cq_parity had to learn.
 *
 * ⚠ MUTEX, unlike js8_chat's deliberate lock-free store. A torn read there
 * costs one repaint of one row; a torn read HERE is a callsign, and a callsign
 * is what the radio transmits at. The writers are the LVGL task (a tap) and
 * the HTTP task (a click in the browser), so the race is real.
 */

#include <stdbool.h>
#include <stddef.h>

/* Select `call`, or clear the selection with NULL or "". Safe from any task. */
void js8_target_set(const char *call);

/* The selected callsign, "" when none. Always NUL-terminates. */
void js8_target_get(char *out, size_t out_len);

/* True when a station is selected. */
bool js8_target_any(void);

/* True when `call` is the selected station (case-sensitive: callsigns are
 * upper-cased by the decoder and by js8_jsc_normalize both). */
bool js8_target_is(const char *call);

/* The opening of a free-text message, ready for the operator to type after.
 *
 * ⭐ RYAN KK6DZB ASKED FOR EXACTLY THIS, and for both cases: "if you type a
 * message directed to another station the outgoing message always starts with
 * their call followed by yours... If you type a free text message not directed
 * at anyone it starts with your call." Without it the box was raw text and he
 * had to type both callsigns by hand every time - "right now because free text
 * is not inserting call signs it is very difficult to use".
 *
 * So: "THEM US " with a station selected, "US " without one, and "" when the
 * operator has not set a callsign yet. It lives here, next to the selection,
 * because BOTH surfaces pre-fill from it - the Tab5 composer directly and the
 * browser through /api/status, rather than each spelling the rule out again.
 *
 * Always ends with a single space when it is not empty, so the first thing
 * typed is a word and not a run-on. */
void js8_text_prefix(char *out, size_t out_len);
