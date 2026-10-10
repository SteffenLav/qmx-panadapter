#ifndef _INCLUDE_JS8_FTX_H_
#define _INCLUDE_JS8_FTX_H_

#include <stdbool.h>
#include <stddef.h>

/* JS8 free-text TRANSMIT run (J8).
 *
 * A free-text message does not fit in one frame, and a JS8 frame is one slot,
 * so sending "RIG IS A QMX RUNNING 5 WATTS INTO A DIPOLE" means keying the
 * radio three times over 45 seconds. This module owns that sequence: it
 * encodes the whole message up front, then arms one frame per slot and
 * re-arms as each burst finishes.
 *
 * ⛔ ENCODED UP FRONT, ALL OF IT, before the first frame is armed. The same
 * reason ft8_tx_build_request() encodes before arming: a message that cannot
 * be encoded must fail in front of the operator, not halfway through a
 * transmission with the radio already keyed and three frames on the air.
 *
 * ⚠ THE AIR TIME IS THE OPERATOR'S TO AGREE TO. js8_ftx_plan() answers "how
 * many frames and how many seconds" WITHOUT transmitting, so a composer can
 * show the cost before anything is armed. Use it.
 */

/* A message longer than this is refused rather than truncated. 12 frames is
 * three minutes of transmission at JS8 Normal - already a long time to hold a
 * frequency, and well past what anyone sends in one go. */
#define JS8_FTX_MAX_FRAMES  12

/* What `text` would cost, without transmitting anything.
 *
 * Fills *frames with the frame count and *secs with the air time. `normalised`
 * receives what will ACTUALLY be sent - upper-cased, whitespace collapsed,
 * unsendable characters dropped - because that is not always what was typed,
 * and the operator should see the difference before keying.
 *
 * Returns false if nothing in the text can be sent. */
bool js8_ftx_plan(const char *text, int *frames, int *secs,
                  char *normalised, size_t norm_len);

/* Encode `text` and start transmitting it, one frame per slot.
 *
 * `audio_freq_hz` is the TX tone; pass 0 to use the same picker the CQ button
 * uses. Returns false with a presentable reason in *out_err if the message
 * cannot be sent, if the sub-mode is not JS8, or if a transmission is already
 * in progress. */
bool js8_ftx_start(const char *text, int audio_freq_hz,
                   char *out_err, size_t out_err_len);

/* Stop a run. Disarms anything pending; a burst already on the air winds down
 * through ft8_tx_request_abort() so the radio is never left keyed. */
void js8_ftx_cancel(void);

/* True while a run has frames left to send. *sent and *total are filled when
 * non-NULL (*sent counts frames already transmitted). */
bool js8_ftx_active(int *sent, int *total);

/* Drive the run. Arms the next frame once the previous burst has finished.
 *
 * ⛔ CALL THIS FROM THE LVGL TASK, about once a second - it is the same
 * deferral every other TX path in this project uses, because ft8_tx_arm()
 * blocks briefly and must not run on the HTTP task. Calling it faster does
 * no harm; the next frame is armed when TX goes idle, not on a timer. */
void js8_ftx_tick(void);

/* One line for the status surfaces, e.g. "free text 2/3". Writes "" when no
 * run is active. */
void js8_ftx_status(char *out, size_t out_len);

#endif /* _INCLUDE_JS8_FTX_H_ */
