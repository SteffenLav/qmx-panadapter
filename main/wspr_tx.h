#pragma once

// WSPR TX: single-burst engine, CAT-driven via TA; (same technique as
// ft8_tx.c - see that file's header for the CAT sequence rationale: the
// QMX does its own DDS synthesis + envelope shaping, we just feed it tone
// frequencies over CAT). Phase 0 of docs/wspr-scope.md confirmed TA;
// accepts the decimal-Hz precision WSPR's 1.4648 Hz tone spacing needs;
// docs/wspr-phase1-status.md / wspr_proto.c / wspr_fano.c built and
// proved (against a real captured WSPR recording) the message encoding
// this module feeds into the CAT burst.
//
// WSPR has no QSO exchange (see docs/wspr-scope.md) - a beacon
// transmission is one self-contained message, no reply, no state
// machine. So this is deliberately much simpler than ft8_tx.c: arm one
// message, it fires on the next even UTC minute boundary (WSPR's own
// slot convention), one ~110.6 s burst, done.
//
// Unlike FT8's burst (which runs inside ft8_task's existing 15 s slot
// loop - see ft8_tx.h), WSPR's ~110.6 s doesn't fit inside any existing
// task's cadence, and there is no WSPR slot-loop task yet (that's
// deliberately out of scope here - see docs/wspr-scope.md's Phase 2
// plan). wspr_tx_arm() therefore spawns its own short-lived worker task
// (psram_task_create() - background/non-realtime, per this project's own
// rule) that waits for the boundary, runs the burst, and exits.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WSPR_TX_IDLE = 0,   // nothing queued
    WSPR_TX_ARMED,      // queued, worker task waiting for the next even minute
    WSPR_TX_ACTIVE,     // CAT burst in progress - radio keyed up right now
} wspr_tx_state_t;

// A fully-built, ready-to-arm WSPR transmission. Always produced by
// wspr_tx_build_request(), which does the actual protocol encode up
// front - a malformed callsign/grid is reported immediately, never
// discovered mid-burst.
typedef struct {
    char    callsign[7];
    char    grid[5];
    int     power_dbm;
    int     audio_freq_hz;      // base AF tone; the 4 FSK tones are this + tone*1.4648 Hz
    uint8_t tones[162];         // pre-encoded channel-order tone indices (0-3)
} wspr_tx_request_t;

// Usable WSPR audio passband on the QMX (Hz) - mirrors FT8_TX_TONE_MIN/MAX_HZ's
// reasoning (ft8_tx.h): below ~200 Hz the QMX audio path attenuates, and WSPR's
// own convention centers activity around 1400-1600 Hz within a standard SSB
// receive passband (see docs/wspr-scope.md's Phase 3 note). No scan/clear-slot
// picker for a first cut - WSPR has no reply logic and no per-station clash
// concern the way FT8 does, so a fixed default is enough to start with.
#define WSPR_TX_DEFAULT_FREQ_HZ   1500
#define WSPR_TX_TONE_MIN_HZ        200
#define WSPR_TX_TONE_MAX_HZ       2800

// A WSPR transmission starts ONE SECOND into the even UTC minute, not at it.
// Measured, not recalled: the five real stations in WSJT's own reference
// capture (test/wav_reference/wspr/150426_0918.wav, recorded from the even
// minute) begin at 1.109 / 1.515 / 1.621 / 1.813 / 2.133 s - a floor at ~1.1 s
// with each station's own clock error stacked on top. See wspr_tx_worker_task().
#define WSPR_TX_START_OFFSET_MS   1000

// One-time module init (mutex creation). Call once at boot. Idempotent.
void wspr_tx_init(void);

// Encode *now* (wspr_pack_message + wspr_convolve_encode + wspr_interleave +
// wspr_symbols_to_tones - all already proven, see wspr_proto.c/wspr_fano.c),
// so a malformed callsign/grid is reported before the operator can even try
// to arm - never discovered mid-burst. `grid` may be a 6-char Maidenhead
// locator (e.g. settings' my_grid); only the first 4 characters are used
// (WSPR's own grid field is 4 characters). On success fills *out_req and
// returns true; on failure returns false and writes a short reason to
// out_err.
bool wspr_tx_build_request(const char *callsign, const char *grid,
                            int power_dbm, int audio_freq_hz,
                            wspr_tx_request_t *out_req,
                            char *out_err, size_t out_err_len);

// Run the Digi-mode pre-flight (mirrors ft8_tx_arm()'s reasoning: check/
// switch happens here, with up to ~110 s of lead time before the next even
// minute, never at burst time where any delay would shift the start off
// WSPR's own slot boundary) and arm *req. Spawns the worker task that
// waits for the next even UTC minute and runs the burst automatically -
// the caller does not need to call anything else. Refuses - returns false
// and fills out_err - if a burst is already ARMED or ACTIVE, or the QMX
// won't confirm Digi mode within the pre-flight window.
bool wspr_tx_arm(const wspr_tx_request_t *req, char *out_err, size_t out_err_len);

// Cancel an ARMED (not yet started) request; the worker task exits without
// transmitting. No-op if nothing is armed, or a burst is already ACTIVE
// (can't un-arm something already on-air).
void wspr_tx_disarm(void);

// Ask an ACTIVE burst to wind down early: stop sending tone symbols at the
// next opportunity (checked once per ~683 ms symbol) and jump straight to
// the TA0; / settle / RX; tail, so the radio is never left keyed up. No-op
// if nothing is currently transmitting.
void wspr_tx_request_abort(void);

// Snapshot current state for the UI. Mutex-guarded; safe to call from any
// task/core at any rate.
//   text        if non-NULL, receives display text of the armed/active
//               request ("" when IDLE)
//   text_len    size of the text buffer
//   secs_until  if non-NULL, receives seconds-until-fire when ARMED
//               (seconds to the next even UTC minute), or 0 when IDLE/ACTIVE
wspr_tx_state_t wspr_tx_get_status(char *text, size_t text_len, int *secs_until);

// Seconds until the next even UTC minute boundary (0, 2, 4, ... 58) -
// WSPR's own slot convention. Uses the system clock (see time_sync/
// time_sync.c for how that's kept accurate) - if the clock is wrong, the
// burst will fire at the wrong wall-clock time but the CAT sequence itself
// is unaffected.
int wspr_tx_seconds_until_next_slot(void);

/* Last MEASURED burst output (PC;/SW; read while keyed), not the declared
 * figure. false until a burst has reported one. */
bool wspr_tx_get_last_power_swr(float *power_w, float *swr);

// True if this build has WSPR_TX_SEND_LIVE=1 (radio actually keyed on a
// burst), false if it's still the dry-run default. Exposed so callers
// (e.g. the "wspr_tx_test" dev API action) can report the real state
// instead of assuming - the compile-time macro itself isn't visible
// outside wspr_tx.c.
bool wspr_tx_send_live_build(void);

#ifdef __cplusplus
}
#endif
