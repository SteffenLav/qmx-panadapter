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

// What the loop is doing right now, for /api/wspr and any future UI:
// "idle" / "waiting for the slot" / "capturing 62/120 s" / "decoding 3/8".
const char *wspr_rx_status(void);
