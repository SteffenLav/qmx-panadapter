// ft8_test.h - Synthetic FT8 round-trip self-test.
// One-shot diagnostic: encode -> synthesize 8-FSK audio -> monitor -> decode.
// Logs PASS/FAIL plus per-stage timing. Called once from app_main at boot.

#ifndef FT8_TEST_H
#define FT8_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ft8/message.h"

#ifdef __cplusplus
extern "C" {
#endif

void ft8_self_test(void);

// One-shot ARRL Field Day message encode/decode round-trip check (pure bit
// math, no audio/radio involved). Logs PASS/FAIL per case at boot so a bad
// flash is caught immediately rather than discovered live on the air.
void ft8_arrl_fd_selftest(void);

// One-shot end-to-end ARRL Field Day check: encode a message, synthesize its
// actual GFSK audio waveform (ft8_lib's own synthesis, heap-allocated), feed
// it through the REAL on-device STFT/candidate-search/LDPC decode pipeline
// (the same code path live RF goes through), and confirm it decodes back to
// the original message. Logs PASS/FAIL. The closest available proof that a
// real over-the-air Field Day exchange would decode correctly, short of an
// actual second station.
void ft8_arrl_fd_e2e_selftest(void);

// General-purpose version of the same encode->GFSK-audio->STFT->LDPC->decode
// pipeline used by ft8_arrl_fd_e2e_selftest(), exported for ft8_sim.c's
// phantom-station simulator: takes an already-encoded message (any type -
// standard, ARRL FD, free text, ...) and a base audio tone, synthesizes its
// real waveform, runs it through the actual on-device receive pipeline, and
// returns the decoded text/SNR/score exactly as a genuine RX would. Runs
// synchronously and allocates from PSRAM - call it from a task with a large
// stack (>=32 KB), same constraint as the self-test (see ft8_test.c's
// comment on why this can't run on the default "main" task stack).
// Returns false if encoding/allocation failed or no candidate decoded.
bool ft8_synth_and_decode(const ftx_message_t *msg, float tone_hz,
                          char *out_text, size_t out_len,
                          int *out_snr_db, int *out_score);

// One-shot check that ft8_synth_and_decode() also round-trips a plain
// standard "CQ <call> <grid>" message (the FT8 simulation mode's phantom CQ
// path), not just ARRL FD. Logs PASS/FAIL.
void ft8_sim_synth_selftest(void);

// Returns the clock timing error (ms) measured from the strongest successfully
// decoded FT8 candidate in the last slot that decoded anything. Positive = system
// clock is fast. Returns false if no valid measurement is available yet.
bool ft8_get_last_timing_ms(int *out_ms);

// Increments each time ft8_get_last_timing_ms's value is refreshed from a new
// decode. UI code can poll this to detect a new sync event (e.g. to flash the
// SS box) without needing a callback/notification mechanism.
uint32_t ft8_get_timing_seq(void);

// ---- Operating sub-mode (FT8 vs FT4) ------------------------------------
// FT4 and FT8 share the QMX's USB/DiGi data mode and the same ft8_lib
// primitives - they differ only in slot length (15 s vs 7.5 s), symbol rate,
// and the monitor's protocol setting. This runtime flag is set from the FT8
// screen's preset dropdown (the FT4-headed column) and is the hook the slot
// engine reads.
//
// NOTE: the 7.5 s slot rework is NOT done yet. Selecting FT4 currently sets the
// dial frequency + on-screen MODE label and stores this flag, but the
// capture/decode/TX loop in ft8_test.c still runs FT8 timing - so FT4 signals
// will not decode/transmit correctly until that follow-up lands. Wiring the
// flag now is deliberate: it gives the engine work a single, already-plumbed
// point to branch on. See the TODO at the monitor_config in ft8_task().
typedef enum {
    FT8_OP_MODE_FT8 = 0,
    FT8_OP_MODE_FT4 = 1,
} ft8_op_mode_t;

void          ft8_op_mode_set(ft8_op_mode_t m);
ft8_op_mode_t ft8_op_mode_get(void);

// Slot period in ms for the current sub-mode (15000 FT8 / 7500 FT4). Single
// source of truth for anything outside ft8_test.c that needs slot timing -
// the countdown bar/label in ft8_screen_view.c, in particular - so it can
// never drift out of sync with the slot engine's own period.
int ft8_op_mode_slot_ms(void);

#ifdef __cplusplus
}
#endif

#endif // FT8_TEST_H
