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

/* ---- THE CARPET FLOWS, IT DOES NOT REDRAW ------------------------------
 *
 * The first version cleared the buffer at the start of every capture and
 * filled rows 0 -> 175 downward, so the page went black, dripped a picture
 * over 120 s, froze for the decode, and went black again. Two things were
 * wrong with that and the operator named both.
 *
 * 1. TIME RAN THE WRONG WAY. Row 0 was the oldest and new rows pushed
 *    DOWNWARD, while the panadapter waterfall in this same firmware puts the
 *    newest row at the TOP (see the layout block in CLAUDE.md). Two
 *    waterfalls on one device disagreeing about which way time flows is an
 *    inconsistency, not a preference.
 *
 * 2. ONE CYCLE EXACTLY FILLED THE PANE, so there was nowhere for history to
 *    go. WSPR is a mode you read over many cycles - the question is always
 *    "is this station coming back", which a single window cannot answer.
 *
 * So this is a RING of two cycles' worth of rows, newest first, and nothing
 * is ever blanked. The row period stays at one WSPR symbol (0.6827 s)
 * because THE ROW PERIOD IS THE FLOW RATE: averaging symbols together to buy
 * more history would make the carpet advance in visible jerks, which is the
 * exact quality being asked for. It is also what keeps the 8192-point FFT at
 * one bin per WSPR tone spacing. History comes from the ring being deeper
 * than the pane, and the view's nearest-neighbour rowmap squeezes it into
 * the 200 px available - a 110 s trace is ~160 rows, so it survives that
 * easily. */
#define WSPR_WF_CYCLES 2
#define WSPR_WF_HIST_ROWS (WSPR_WF_CYCLES * WSPR_WF_ROWS)   /* 352 */

/* Value written across a whole row to mark a cycle boundary, dashed so it
 * cannot be read as signal - nothing real is uniform across 205 bins. It
 * also marks the ~68 s the receiver is genuinely DEAF while decoding (see
 * the every-other-cycle note in wspr_rx.c): without it the carpet simply
 * stops, which looks identical to a hung display. */
#define WSPR_WF_MARK   100
#define WSPR_WF_MARK_ROWS 2   /* survives the view's 1.76:1 row downsample */

/* Copy the scrolling waterfall in DISPLAY ORDER. `out` must hold
 * WSPR_WF_HIST_ROWS * WSPR_WF_COLS bytes (~72 KB - PSRAM or a static, NEVER
 * a stack local on taskLVGL). Returns false until a row has been produced.
 *
 * ⛔ ROW 0 IS THE NEWEST ROW, and row order runs backwards in time from
 * there, so a view can map display y directly to out row y and get the
 * panadapter's convention for free. Rows never yet written read as black. */
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

