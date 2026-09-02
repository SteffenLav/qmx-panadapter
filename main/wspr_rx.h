#pragma once

#include <stdbool.h>

/* ⭐ IS THE WSPR PAGE REACHABLE AT ALL?
 *
 * WSPR rides the main track before it is finished, so the release carries the
 * code - and the OTA path that delivers it can be exercised - while nobody
 * meets a half-built mode by accident. Every gate in the firmware asks THIS
 * function rather than reading the setting itself, so the swipe cycle, the web
 * screen switch, /api/wspr and the RX loop cannot drift apart about what "off"
 * means. One of them disagreeing is how a feature ends up half-reachable.
 *
 * ⚠ OFF DOES NOT MEAN FREE. .bss is allocated whether the code runs or not.
 * WSPR's internal-RAM share was dealt with separately; see the
 * EXT_RAM_BSS_ATTR note in wspr_rx.c. Do not reason from this flag to memory.
 */
bool wspr_feature_enabled(void);


/* WSPR receive slot loop.
 *
 * Captures the even-minute window from the live IQ stream, decodes it with the
 * decoder proven in docs/wspr-phase1-status.md, and files what it finds in
 * wspr_spots.
 *
 * ⭐ IT DECODES EVERY CYCLE. It did not always: the first cut captured for the
 * whole 120 s and then decoded sequentially, and the decode measured 64 s on
 * this silicon, so the task was still working when the next window opened and
 * could not arm it - half rate, by construction, and documented here as a real
 * limitation rather than a rough edge. The decoder has since been made several
 * times faster and the capture now runs against the next window while the
 * previous one is decoded. Confirmed on the bench 2026-08-28, where /api/wspr
 * reported "captur. 35/120 s | dec 20/20" - capturing and decoding at once -
 * and the operator had already tested both cycles independently.
 *
 * Entering this mode sets ui_mode to UI_MODE_WSPR, which diverts the DSP's IQ
 * chain into the capture pre-ring exactly as FT8 mode does. The panadapter's
 * spectrum and waterfall are unavailable while it runs - the receiver has the
 * IQ stream for the whole cycle - which is why /api/wspr reports the loop's
 * state so the browser can say so out loud.
 */

// Start / stop the loop. Starting sets UI_MODE_WSPR; stopping restores
// UI_MODE_PANADAPTER. Both are safe to call repeatedly.
bool wspr_rx_start(void);
void wspr_rx_stop(void);

/* Drop the waterfall noise floor. ⛔ CALL THIS ON EVERY BAND CHANGE: the
 * floor is a rolling estimate of THIS band's noise, and carrying it across a
 * hop paints the new band against the old one - which came out as a
 * saturated red block at the top of the carpet after a 20 -> 30 m hop. */
void wspr_rx_wf_floor_reset(void);

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
/* ONE cycle fills the pane. Two was tried first and the arithmetic killed it:
 * 352 rows in a 200 px pane makes each row 0.57 px, so at WSPR's 1.47 rows/s
 * the carpet crawled at 0.83 px/s and took ~6 minutes to fill from black.
 * At one cycle each row is 1.14 px and it moves at 1.67 px/s - twice as fast -
 * and the pane is full after a single 120 s capture. History lives in the
 * decode list, which is the right place for it: the list says WHAT was heard,
 * the carpet shows the band NOW. */
#define WSPR_WF_CYCLES 1
#define WSPR_WF_HIST_ROWS (WSPR_WF_CYCLES * WSPR_WF_ROWS)   /* 176 */

/* Value written across a whole row to mark a cycle boundary, dashed so it
 * cannot be read as signal - nothing real is uniform across 205 bins. It
 * also marks the ~68 s the receiver is genuinely DEAF while decoding (see
 * the every-other-cycle note in wspr_rx.c): without it the carpet simply
 * stops, which looks identical to a hung display. */
/* ⛔ A RESERVED SENTINEL, not a palette value. Picking a "light green" out of
 * the signal ramp would not actually distinguish it, because a strong signal
 * passes through green on its way to red. So wf_byte() is clamped to 0..254
 * and 255 means MARKER, which the view renders as an explicit light green no
 * signal can produce. Losing the top ramp value costs nothing visible. */
#define WSPR_WF_MARK   255
/* At one cycle the view UPSAMPLES (176 rows into 200 px), so every source
 * row is drawn at least once and a single marker row could not be skipped.
 * Kept at 2 anyway: it makes the line readable rather than hairline, and it
 * stays correct if WSPR_WF_CYCLES is ever raised again - at which point the
 * map downsamples and a 1-row marker WOULD vanish on some cycles. */
#define WSPR_WF_MARK_ROWS 2

/* Copy the scrolling waterfall in DISPLAY ORDER. `out` must hold
 * WSPR_WF_HIST_ROWS * WSPR_WF_COLS bytes (~36 KB - PSRAM or a static, NEVER
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
/* ---- CAPTURE DUMP -------------------------------------------------------
 *
 * Ask for the next `cycles` captured windows to be written to the SD card as
 * WAV, so the SAME audio the on-device decoder saw can be run through real
 * wsprd on a PC. That comparison is the only thing that separates the two
 * explanations for a bright trace that does not decode: our sensitivity floor
 * is short (~-22.7 dB against wsprd's ~-29), or the trace was never WSPR. The
 * waterfall cannot answer it, because the display saturates anything 16 dB
 * over the median and so draws QRM and a strong signal identically.
 *
 * SD rather than HTTP because a window is 2.88 MB and this link tops out
 * around 211 KB per transfer. Bounded because each file is 2.88 MB: a
 * mistyped 100 would be 288 MB and a full card.
 *
 * Returns the number actually armed (0 if no card is mounted), so a caller
 * can tell "armed" from "there is nowhere to write". */
#define WSPR_DUMP_MAX_CYCLES 20
int wspr_rx_request_dump(int cycles);

/* Cycles still to be written, for status reporting. */
int wspr_rx_dump_pending(void);

void wspr_rx_set_guards(int enforce_near, double near_hz,
                        int enforce_slow, unsigned int slow_cycles);

/* ---- CYCLE HISTORY -------------------------------------------------------
 *
 * How many stations each of the last cycles produced, oldest first. This is the
 * one thing a WSPR monitor can say that a snapshot cannot: whether the band is
 * opening or closing. "Heard 3 stations" is a moment; this is the trend.
 *
 * Deliberately a small fixed array of counts rather than anything derived from
 * the spot store - that ring saturates at 256 spots and then forgets its oldest
 * cycles, which would silently truncate exactly the history this exists to
 * show. (The same saturation once froze the decode list for five hours; see
 * wspr_spots_seq.)
 */
#define WSPR_CYCLE_HISTORY 40

/* Writes up to `max` counts, OLDEST first, and returns how many were written.
 * If the caller wants fewer than are held it gets the NEWEST ones. */
int wspr_rx_cycle_history(uint8_t *out, int max);

/* ---- transmit schedule ----------------------------------------------------
 *
 * Seconds until the next cycle that will actually TRANSMIT, or -1 when none is
 * scheduled (transmit off, duty 0, or the schedule has just been overtaken and
 * the RX loop has not re-rolled it yet).
 *
 * ⭐ This is a real countdown, not a countdown to the next opportunity. The
 * duty-cycle roll is taken IN ADVANCE for exactly this reason - see the block
 * comment on roll_next_tx_cycle() in wspr_rx.c. The button used to count down
 * to the next slot and start again every time the roll lost, which the operator
 * read, correctly, as no countdown at all.
 *
 * Safe from any task: one read of an int64 and some arithmetic. */
int wspr_rx_seconds_to_next_tx(void);

/* Re-roll the schedule after the operator changes whether or how often we
 * transmit. Call it from the TX on/off control and from any path that writes
 * wspr_duty_pct, or the countdown keeps describing the previous setting until
 * the next cycle boundary.
 *
 * ⚠ Takes the two values rather than reading the settings, because both callers
 * are UI tasks and settings_load_all() is a multi-kilobyte stack allocation -
 * the bug class that has boot-looped this board four times. */
void wspr_rx_tx_schedule_reset(bool tx_en, uint8_t duty_pct);
