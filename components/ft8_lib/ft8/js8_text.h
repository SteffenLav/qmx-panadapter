#ifndef _INCLUDE_JS8_TEXT_H_
#define _INCLUDE_JS8_TEXT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "js8_message.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* JS8 frame <-> the text the rest of this firmware already speaks.
 *
 * WHY THIS LAYER EXISTS. Everything above the codec - the decode list, the
 * filters, the pileup, worked-before, the ADIF log and the whole QSO ladder in
 * ft8_qso.c - is driven by one string per decode, in FT8's shape:
 *
 *     "<to> <from> <rest>"      and      "CQ <call> <grid>"
 *
 * ft8_qso.c reads that with split_msg3() and decides from `rest`: "RR73", "73",
 * something starting with 'R' and a sign (a roger plus report), or otherwise a
 * plain report. That is ~3900 lines of behaviour - retries, timeouts,
 * grey-listing, hound mode, Field Day, manual overrides - and none of it is
 * about FT8's wire format.
 *
 * So JS8 joins at the TEXT seam rather than inside the ladder: a received frame
 * is rendered into that same shape, and an outgoing message is parsed back out
 * of it. The protocol facts live here, in one file, with a host harness.
 *
 * ⚠ THE LADDER IS SHORTER IN JS8, and this file does not hide that. FT8 opens a
 * pounce with "<them> <me> <grid>" and only reports on the next pass; JS8 has no
 * grid step in a Directed frame, so the opening message IS the report. A grid
 * `rest` therefore has no Directed form - js8_text_to_frame() refuses it, and
 * ft8_qso.c has to send a report instead. Refusing is deliberate: inventing a
 * frame for it would transmit something no JS8Call station would answer.
 *
 * ⛔ Nothing here has been seen by another station. The mapping is read off
 * JS8Call 2.3.1's varicode.cpp vocabulary and its own message window wording;
 * only a real JS8Call station settles whether it is right. Never a second Tab5.
 */

/* Longest rendering: 2 callsigns (15 each) + 2 spaces + a command word + a
 * number, with room to spare. FTX_MAX_MESSAGE_LENGTH is 35, and a caller
 * passing an ftx_message text buffer must therefore pass its own size. */
#define JS8_TEXT_MAX 48

/* Render a received frame as "<to> <from> <rest>" or "CQ <call> <grid>".
 *
 * Mapping, by what a plain QSO needs:
 *   Heartbeat, is_cq      -> "CQ <call> <grid>"      (grid omitted when absent)
 *   Heartbeat, HB         -> "HB <call> <grid>"
 *   Directed SNR  + num   -> "<to> <from> <-07|+02>"
 *   Directed RR   + num   -> "<to> <from> R-07"      (FT8's roger-with-report)
 *   Directed RR   no num  -> "<to> <from> RR73"
 *   Directed 73           -> "<to> <from> 73"
 *   Directed anything else-> "<to> <from> <CMD>"     (js8_cmd_text spelling)
 *
 * Returns false for a frame type that is not implemented (Compound and
 * CompoundDirected) or whose callsigns do not unpack - a received frame we
 * cannot spell must not become a row saying something else.
 */
bool js8_frame_to_text(const uint8_t frame[JS8_FRAME_BYTES], char* out, size_t out_len);

/* The reverse, for transmitting: parse the same shapes back into a frame.
 * `itype` is always 0 (a single-frame Normal transmission); it is an output so
 * callers do not hard-code it at the js8_encode() call.
 *
 * Returns false when the text has no JS8 form. The cases that matter:
 *   - a grid as `rest` ("<them> <me> JO65"): no Directed grid step exists
 *   - a callsign that will not pack into 28 bits
 *   - a report outside -30..+31 (packNum's range; it clamps, we refuse)
 */
bool js8_text_to_frame(const char* text, uint8_t frame[JS8_FRAME_BYTES], uint8_t* itype);

/* True when `rest` (split_msg3's third field) is a 4-character Maidenhead
 * locator rather than a report or a token. Exposed because ft8_qso.c has to
 * know BEFORE building a message that the FT8 ladder's opening move has no JS8
 * equivalent, rather than discovering it from a failed encode. */
bool js8_text_rest_is_grid(const char* rest);

/* Clamp and format a report the way JS8 carries it: packNum covers -30..+31,
 * which is wider than FT8's -24..+15, so the FT8 formatter would throw away
 * real dB at both ends. Writes "-07" / "+02".  */
void js8_fmt_report(int snr_db, char* out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_JS8_TEXT_H_
