#pragma once

// v0.12.0: Manual FT8 TX core (Reply + Call CQ).
//
// Message-agnostic TX engine driven entirely over CAT via the QMX's TA;
// ("Transmit Audio") command, which sets the transmitted audio tone
// frequency directly in decimal Hz - the radio does its own DDS synthesis
// (Si5351/MS5351M) and Blackmann-Harris envelope shaping. We never
// synthesize PCM audio or touch the USB UAC playback path; the whole job
// reduces to encoding the message to 79 tone indices (ft8_lib already does
// this for RX) and walking through them with one TA<freq>; CAT command
// every 160 ms, bracketed by TX; / TA0; / RX;. See
// docs/qmx-reference/SOURCES.md for the distilled CAT sequence and the
// reasoning for why this is simpler (and more precise) than the
// conventional WSJT-X PC-streams-audio workflow.
//
// Runs entirely inside the existing ft8_task slot loop (ft8_test.c) - no
// new FreeRTOS task, no new stack/PSRAM allocation. Each 15 s slot is
// either RX (decode, exactly as before) or, if a matching TX request is
// armed, a ~12.7 s CAT burst (this module). TX and RX structurally cannot
// overlap: a slot is one or the other, which is also *correct* - while
// keyed up, the QMX's USB audio captures its own TX output, not the air.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "ft8/constants.h"     // FT8_NN (79), FT4_NN (105), ftx_protocol_t
#include "ui/ft8_screen.h"     // FT8_CALL_MAX_LEN, ft8_call_t

#ifdef __cplusplus
extern "C" {
#endif

// Largest tones[] needed by either protocol (FT4_NN=105 > FT8_NN=79).
#define FT8_TX_MAX_NN  FT4_NN

typedef enum {
    FT8_TX_IDLE = 0,    // nothing queued; ft8_task runs RX every slot
    FT8_TX_ARMED,       // queued, waiting for a slot whose parity matches
    FT8_TX_ACTIVE,      // CAT burst in progress - radio keyed up right now
} ft8_tx_state_t;

typedef enum {
    FT8_TX_KIND_REPLY = 0,  // <their_call> <my_call> <my_grid>
    FT8_TX_KIND_CQ,         // CQ <my_call> <my_grid>
    FT8_TX_KIND_ROGER_RPT,  // <their_call> <my_call> R<snr>   (QSO TX2)
    FT8_TX_KIND_73,         // <their_call> <my_call> 73        (QSO TX3)
} ft8_tx_kind_t;

// A fully-built, ready-to-arm TX request. Always produced by
// ft8_tx_build_request(), which does the identity check and the actual
// ft8_lib encode up front - so a malformed callsign or missing identity
// is reported in the confirmation UI, never discovered mid-burst.
typedef struct {
    ft8_tx_kind_t kind;
    char     target_call[FT8_CALL_MAX_LEN];  // remote call (REPLY); empty for CQ
    int      audio_freq_hz;                  // base AF tone, Hz
    bool     want_even_slot;                 // required slot parity when use_parity=true
    bool     use_parity;                     // if true, only fire when slot parity ==
                                              // want_even_slot; always true for REPLY,
                                              // optional for CQ (user TX preference)
    // Protocol this request was encoded for, captured at build time (from the
    // FT8/FT4 sub-mode active right then) so a later mode flip between build
    // and burst can never desync the tones[] encoding from the cadence used
    // to send them - ft8_tx_run() always uses req->protocol, never re-reads
    // the live ft8_op_mode_get() mid-burst.
    ftx_protocol_t protocol;
    uint8_t  tones[FT8_TX_MAX_NN];           // pre-encoded tone indices: 0..7 (FT8) / 0..3 (FT4)
    char     display_text[32];               // e.g. "K1ABC OZ1LAV JO45ab"
    char     extra_field[16];                // for ROGER_RPT: "R-10"; for 73: "73";
                                              // Field Day: "R 32A WCF" / "16A EMA"
} ft8_tx_request_t;

// Default base audio tone (Hz) for an originated CQ call - there's no
// "heard" frequency to anchor to, so we use the de-facto standard FT8
// sub-band centre most operators / WSJT-X default to. Trivial to make
// user-adjustable later; a #define is plenty for the v0.12.0 MVP.
#define FT8_TX_CQ_DEFAULT_FREQ_HZ   1500

// One-time module init (mutex creation). Call once at boot, e.g. from
// app_main alongside ft8_screen_init(). Idempotent.
void ft8_tx_init(void);

// Validate operator identity and encode the message *now*, so problems
// surface in the confirmation UI before the user can even arm a TX - never
// mid-burst. On success, fills *out_req (incl. pre-encoded tones[] and a
// human-readable display_text) and returns true. On failure returns false
// and writes a short, user-presentable reason to out_err.
//
//   kind                  FT8_TX_KIND_REPLY or FT8_TX_KIND_CQ
//   target_call           remote callsign (REPLY only; pass NULL/"" for CQ)
//   target_audio_freq_hz  base AF tone in Hz - for a reply, this is the
//                         heard station's ft8_call_t.last_freq verbatim (no
//                         IF-offset correction needed, see ft8_tx.c); for a
//                         CQ, pass FT8_TX_CQ_DEFAULT_FREQ_HZ
//   target_last_utc       slot-start second the target was last heard in,
//                         i.e. ft8_call_t.last_utc (REPLY only - used to
//                         derive the required reply parity; ignored for CQ)
// extra: for ROGER_RPT pass "R-10" etc; for 73 pass "73"; pass NULL for
// REPLY/CQ (uses my_grid). Existing callers that don't use the new kinds
// may pass NULL safely.
bool ft8_tx_build_request(ft8_tx_kind_t kind,
                          const char *target_call,
                          int target_audio_freq_hz,
                          int64_t target_last_utc,
                          const char *extra,
                          ft8_tx_request_t *out_req,
                          char *out_err, size_t out_err_len);

// Build a CQ-kind request from an arbitrary full message string (e.g. a
// user-edited CQ preset like "CQ DX OZ1LAV JO65FR"). Uses the general
// ft8_lib encoder, which accepts standard messages and <=13-char free text.
// display_text is the verbatim message. On failure returns false + out_err.
bool ft8_tx_build_request_text(const char *message_text,
                               int audio_freq_hz,
                               ft8_tx_request_t *out_req,
                               char *out_err, size_t out_err_len);

// Build an ARRL Field Day exchange request (kind must be REPLY or ROGER_RPT -
// the two messages that carry an explicit class+section instead of a
// grid/report). class_section is the literal text to encode, e.g. "16A EMA"
// (our own exchange, no R) or "R 3A NNJ" (acknowledging the other station's
// exchange). Always uses ftx_message_encode_arrl_fd - never falls back to
// grid. Same semantics/return value as ft8_tx_build_request() otherwise.
bool ft8_tx_build_request_fd(ft8_tx_kind_t kind,
                             const char *target_call,
                             int target_audio_freq_hz,
                             int64_t target_last_utc,
                             const char *class_section,
                             ft8_tx_request_t *out_req,
                             char *out_err, size_t out_err_len);

// Run the (blocking, ~1 s worst case) Digi-mode pre-flight and arm *req
// for transmission on its next matching slot. Replaces any request that
// is currently ARMED (most recent confirm wins). Refuses - returns false
// and fills out_err - only if a burst is already ACTIVE, or the QMX won't
// confirm Digi mode within the pre-flight window.
//
// This briefly blocks the calling task (mirrors the existing precedent at
// ui.c's bw_preset_cb, which already calls cat_set_mode()+vTaskDelay()
// synchronously from an LV_EVENT_CLICKED handler) - intentional: FT8
// transmissions must start exactly on the slot boundary, so the mode
// check/switch happens here, with seconds of lead time, never at burst
// time where any delay would desync every receiving decoder.
bool ft8_tx_arm(const ft8_tx_request_t *req, char *out_err, size_t out_err_len);

// Cancel an ARMED (not yet started) request. No-op if nothing is armed,
// or a burst is already ACTIVE (can't un-arm something already on-air).
void ft8_tx_disarm(void);

// Ask an ACTIVE burst to wind down early: stop sending tone symbols at the
// next opportunity (checked once per ~160 ms symbol) and jump straight to
// the TA0; / settle / RX; tail - so the radio is never left keyed up. No-op
// if nothing is currently transmitting.
void ft8_tx_request_abort(void);

// Snapshot current state for the UI. Mutex-guarded; safe to call from any
// task/core at any rate (e.g. a 1 Hz refresh timer).
//   text        if non-NULL, receives the display text of the armed/active
//               request ("" when IDLE)
//   text_len    size of the text buffer
//   secs_until  if non-NULL, receives seconds-until-fire when ARMED, or 0
//               when IDLE/ACTIVE
ft8_tx_state_t ft8_tx_get_status(char *text, size_t text_len, int *secs_until);

// Returns the power (W) and SWR from the most recent TX burst's PC;/SW;
// query, and how many seconds ago that burst completed (-1.0f if no burst
// has completed yet). *power_w and *swr are -1.0f if no reading was ever
// taken, or if that particular value was unavailable.
float ft8_tx_get_last_power_swr(float *power_w, float *swr);

// Scan the current heard-station table and return the audio frequency (Hz) of
// the nearest unoccupied 50-Hz bin to FT8_TX_CQ_DEFAULT_FREQ_HZ (1500 Hz).
// Safe to call from any task; takes the ft8_screen mutex internally via
// ft8_screen_get_all().  Returns FT8_TX_CQ_DEFAULT_FREQ_HZ when the table is
// empty (nothing decoded yet) or when every bin is occupied (very busy band).
// Heap-allocates a temporary snapshot buffer; returns the default on OOM.
int ft8_find_clear_tone_hz(void);

// Same scan as ft8_find_clear_tone_hz(), but centred on an arbitrary
// audio frequency instead of the fixed 1500 Hz CQ default - used to hop a
// CQ that's been clashed onto its original tone to the nearest still-clear
// slot, minimizing how far it moves. Returns center_hz unchanged when the
// table is empty, on OOM, or when every bin is occupied.
int ft8_find_clear_tone_hz_near(int center_hz);

// Returns true if the currently ARMED or ACTIVE request's tone frequency
// clashes with another heard station's bin (±1 guard slot).
// For a REPLY request, the intended target station is excluded from the check
// (they are expected to be at that frequency). Returns false when IDLE or if
// the decode table is empty.  Safe to call from any task at 1 Hz or less.
bool ft8_tx_is_clashing(void);

// ---- Consumed only by ft8_task's slot loop (ft8_test.c) --------------------

// True if a request is currently ARMED and its required parity matches this
// slot (a CQ request matches any slot - it fires on the very next boundary
// after arming, same as an operator just keying up whenever they get to the
// radio). On true, *out receives a private copy of the request and the
// engine transitions ARMED -> ACTIVE; the caller must then invoke
// ft8_tx_run(out) to actually execute the burst for this slot.
//
// slot_start_ms is the UTC slot boundary in MILLISECONDS (ft8_test.c's
// boundary_ms - an exact multiple of the active protocol's slot period),
// NOT seconds. Parity is derived from this using the ARMED request's own
// protocol period (15000 ms FT8 / 7500 ms FT4) - passing whole seconds here
// would silently break FT4 parity (see the long comment at this function's
// definition for why).
bool ft8_tx_should_run_this_slot(int64_t slot_start_ms, ft8_tx_request_t *out);

// Run one full CAT burst synchronously (blocks the calling task throughout:
// ~12.7 s for FT8, ~5 s for FT4). Re-verifies Digi mode first (a cheap
// cached-string read, not a CAT round trip - aborts cleanly *before* keying
// up if it has drifted since arming, rather than risk a late corrective
// mode-switch desyncing the burst start). Pauses the CAT poll loop for the
// duration, sends TX; / Nx TA<freq>; / TA0; / RX; (N = req->protocol's
// symbol count, 79 FT8 / 105 FT4), resumes polling, and unconditionally
// returns the engine to IDLE with the radio back in RX - whether the burst
// completed normally, was aborted via ft8_tx_request_abort(), hit a send
// error, or never started due to a mode-check failure.
//
// FT4 SAFETY: the 48 ms/symbol CAT cadence (vs FT8's 160 ms) has not been
// verified on real QMX hardware - whether the radio's CAT parser/DDS keep up
// is an open question. ft8_tx_run() and ft8_tx_arm() therefore force the
// existing FT8-simulation-mode interlock (see ft8_sim.h) for ANY request
// whose protocol is FT4, regardless of the operator's actual sim-mode
// setting: no cat_* call of any kind reaches a connected QMX. Lift this only
// after the cadence has been confirmed safe on-air.
void ft8_tx_run(const ft8_tx_request_t *req);

#ifdef __cplusplus
}
#endif
