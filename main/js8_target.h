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
#include <stdint.h>

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

/* ---- A STATION HAS ASKED US FOR A REPORT -----------------------------------
 *
 * JS8's directed vocabulary has an explicit command for it: `SNR?`, index 0
 * (JS8_CMD_SNR_QUERY). Until now nothing read it, so a station that used the
 * command got SILENCE from us.
 *
 * ⛔ THE REPORT EXCHANGE WAS NEVER THE GAP, and conflating the two cost a
 * wrong answer to Gyula HA3HZ on 2026-10-13. ft8_qso.c has had JS8 branches
 * all along, and his own diagnostic log showed this unit answering four
 * stations unattended with reports. What did not exist was handling of the
 * COMMAND. [[feedback_absent_command_is_not_absent_capability]].
 *
 * ⛔ AND IT STILL DOES NOT ANSWER BY ITSELF. Noting the request transmits
 * nothing: it tells the operator, who presses SNR. An automatic answer here
 * would be a seventh unattended arming site, pointed at whoever asks, and
 * that is a decision for the operator and not a side effect of closing a
 * protocol gap - [[feedback_never_leave_the_radio_transmitting]], and the
 * same reasoning that made a JS8 row tap stop transmitting.
 *
 * The note lapses after JS8_REQUEST_WINDOW_SEC. A report is about conditions
 * now, and a prompt to answer a station last heard ten minutes ago is a
 * prompt to transmit at somebody who has moved on.
 *
 * Writer: the decode task. Readers: the LVGL task and the HTTP task. Same
 * mutex, and for the same reason the selection has one. */
#define JS8_REQUEST_WINDOW_SEC 600

void js8_request_note(const char *call, int freq_hz, int64_t utc);
/* True while a request is live. `age_s` is how long ago it arrived. Any of
 * the out parameters may be NULL. */
bool js8_request_get(char *call, size_t out_len, int *freq_hz, int *age_s);
void js8_request_clear(void);
