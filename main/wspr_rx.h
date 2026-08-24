#pragma once
#include <stdbool.h>

/* WSPR receive slot loop.
 *
 * Captures the even-minute window from the live IQ stream, decodes it with the
 * decoder proven in docs/wspr-phase1-status.md, and files what it finds in
 * wspr_spots.
 *
 * ⚠ IT DECODES EVERY OTHER CYCLE, BY CONSTRUCTION - and that is a real
 * limitation, not a rough edge to be polished later without thought. A cycle is
 * 120 s; the capture fills all of it and the decode measured 64 s on this
 * silicon, so a single sequential task is still decoding when the next window
 * opens and cannot arm it. The fix is a second buffer so the decode runs
 * against the previous window while the next one fills - the same ping-pong
 * ft8_test.c already uses - and it is deliberately NOT done in this first cut,
 * because a receiver that works at half rate is worth more than a concurrent
 * one that is wrong. See wspr_rx.c's own note for the memory arithmetic.
 *
 * Entering this mode sets ui_mode to UI_MODE_WSPR, which diverts the DSP's IQ
 * chain into the capture pre-ring exactly as FT8 mode does. On this branch
 * there is no Tab5 WSPR screen yet, so the panadapter's spectrum and waterfall
 * FREEZE while it runs - expected, and the reason /api/wspr reports the loop's
 * state so the browser can say so out loud.
 */

// Start / stop the loop. Starting sets UI_MODE_WSPR; stopping restores
// UI_MODE_PANADAPTER. Both are safe to call repeatedly.
bool wspr_rx_start(void);
void wspr_rx_stop(void);

bool wspr_rx_running(void);

/* ---- the per-cycle waterfall ----
 *
 * A LIVE panadapter is not possible on this page and that is structural, not a
 * shortcut: while a capture is armed the DSP diverts the IQ into the capture
 * pre-ring instead of the panadapter FFT, and a capture fills 120 s of every
 * 120 s cycle. A live spectrum would therefore be frozen for exactly the time
 * it matters.
 *
 * So the waterfall is built FROM THE CAPTURED WINDOW after each cycle, which is
 * what WSJT-X shows for WSPR anyway. One row per symbol period and 1.4648 Hz
 * bins - an 8192-point FFT at 12 kHz gives exactly one bin per WSPR tone
 * spacing - so each transmission reads as a clean vertical trace.
 */
#define WSPR_WF_ROWS   176            /* symbol periods in a 120 s window */
#define WSPR_WF_LO_HZ  1350.0f
#define WSPR_WF_HI_HZ  1650.0f
#define WSPR_WF_COLS   205            /* (1650-1350) / 1.4648 */

/* Copy the most recent cycle's waterfall. `out` must hold
 * WSPR_WF_ROWS * WSPR_WF_COLS bytes (~36 KB - PSRAM or a static, NEVER a
 * stack local on taskLVGL). Returns false until a cycle has produced one.
 * Row 0 is the START of the window. */
bool wspr_rx_get_waterfall(uint8_t *out);

/* Bumped every time a new waterfall lands, so a UI can repaint only on change
 * instead of every tick. */
uint32_t wspr_rx_waterfall_seq(void);

// What the loop is doing right now, for /api/wspr and any future UI:
// "idle" / "waiting for the slot" / "capturing 62/120 s" / "decoding 3/8".
const char *wspr_rx_status(void);
/* Flip guard ENFORCEMENT at runtime (dev action "wspr_guards"). Both guards
 * are measured either way; this only changes which one acts. Deliberately not
 * an NVS setting: it is an experiment knob for choosing between the two on
 * real signals, not a user preference, and it should not survive silently. */
void wspr_rx_set_guards(int enforce_near, double near_hz,
                        int enforce_slow, unsigned int slow_cycles);

