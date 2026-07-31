#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ft8_tx.h"
#include "storage/settings.h"   // ft8_filters_t

// v0.13.0: FT8 QSO state machine — auto search-and-pounce + CQ-run.
//
// Driven from two call sites:
//   ft8_qso_start()       – LVGL task (core 0): arm TX1, enter WAIT_RPT (pounce)
//   ft8_qso_start_cq()    – LVGL task (core 0): arm CQ, enter CQ (run)
//   ft8_qso_advance()     – ft8_decode_task (core 1): after each RX slot, scan
//                           decodes, decide the next message, time out / retry
//   ft8_qso_on_tx_complete() – ft8_task (core 1): right after a burst ends,
//                           re-arm the current outgoing message for the next
//                           matching slot (CQ loop cadence + exchange retries)
//
// Two roles share one machine:
//   POUNCE  (we answered their CQ): TX1 grid → WAIT_RPT → R-rpt → WAIT_RR73 → 73
//   CQ-RUN  (they answered our CQ): CQ → answer → report → WAIT_ROGER → RR73
//
// The "current outgoing message" is re-armed every TX slot until the expected
// reply is heard (progress) or QSO_TIMEOUT_SLOTS consecutive RX slots pass with
// none (timeout). This is what keeps us patiently re-sending instead of going
// silent after one transmission.

typedef enum {
    FT8_QSO_IDLE = 0,
    FT8_QSO_CQ,          // CQ loop: re-arm CQ every TX slot until answered or cancelled
    FT8_QSO_WAIT_RPT,    // POUNCE: TX1 (grid) sent; listening for their signal report
    FT8_QSO_WAIT_ROGER,  // CQ-RUN: our report sent; listening for their R<report>
    FT8_QSO_WAIT_RR73,   // POUNCE: R<report> sent; listening for RR73/73
    FT8_QSO_WAIT_DONE,   // final (73 or RR73) sent; wrapping up
    FT8_QSO_DONE,        // QSO complete  (shown for one slot, then auto-IDLE)
    FT8_QSO_TIMEOUT,     // no response within timeout  (sticky until next start)
} ft8_qso_state_t;

void ft8_qso_init(void);

// Start a continuous CQ loop: arm the CQ request immediately and re-arm it
// after every TX slot. When a station answers, automatically send them a
// signal report and run the exchange to completion. If the worked station
// stops responding the machine returns to calling CQ (it does not give up
// the frequency). Stops only when the operator cancels via ft8_qso_abort().
bool ft8_qso_start_cq(const ft8_tx_request_t *cq_req, char *err, size_t err_len);

// Arm TX1 and enter the QSO machine (pounce — we are answering their CQ).
// tx1_req: the pre-built, pre-encoded TX1 request produced by ft8_tx_build_request().
//   target_call and audio_freq_hz are read from it for the rest of the exchange.
// Returns false + err string if the arm is refused.
bool ft8_qso_start(const ft8_tx_request_t *tx1_req, char *err, size_t err_len);

// Called from the decode task after each successful RX slot.
// slot_sec: UTC second the RX slot started (used to match decoded messages
// to this slot and not stale ones from previous slots).
void ft8_qso_advance(int64_t slot_sec);

// Called by ft8_task immediately after ft8_tx_run() returns. Re-arms the
// current outgoing message for the next matching slot, so the re-arm happens
// at T+12.7 s (TX end), not at T+19 s (after decode) which would be too late
// for the capture task's slot-boundary check. Drives both the CQ loop cadence
// and the exchange retry cadence.
void ft8_qso_on_tx_complete(void);

// Abort QSO and disarm any pending TX. No-op when IDLE.
void ft8_qso_abort(void);

// Manually override the next outgoing message during an active exchange.
//   FT8_TX_KIND_REPLY    → re-arm the current message unchanged (re-send)
//   FT8_TX_KIND_ROGER_RPT → force-send RR73 and advance to WAIT_DONE
//   FT8_TX_KIND_73        → force-send 73 and advance to WAIT_DONE
// Only valid during WAIT_RPT / WAIT_ROGER / WAIT_RR73. Returns false + err
// if there is no active exchange or the arm is refused.
bool ft8_qso_override_next(ft8_tx_kind_t kind, char *err, size_t err_len);

ft8_qso_state_t ft8_qso_get_state(void);
void            ft8_qso_get_target(char *buf, size_t len);

// CQ auto-stop progress: CQ bursts already transmitted in the current CQ
// sequence, or -1 when no CQ run is in progress. The armed/on-air call is
// therefore number (sent+1). The limit itself is the cq_max_calls setting.
int             ft8_qso_get_cq_calls_sent(void);

// True while a QSO exchange (CQ loop or any WAIT_* state) is active - i.e.
// ft8_qso_start()/ft8_tx_arm() of an unrelated message would clobber it.
// *target_buf (if non-NULL) is filled with the call being worked, or empty
// while just calling CQ with no answer yet. Used by the TX confirmation
// modal to refuse a competing Auto Pounce / Transmit tap instead of silently
// overwriting the in-progress exchange.
bool ft8_qso_is_busy(char *target_buf, size_t len);
// Extra field of the current outgoing message (grid / report / R-report / RR73 / 73).
// Empty string when IDLE or no message is armed.
void            ft8_qso_get_cur_extra(char *buf, size_t len);

// True while a CQ-originated session is active (calling CQ or working the
// station that answered). The decode-list UI uses this to hide other stations'
// CQ rows so replies to us stand out. Always false for pounce sessions.
bool ft8_qso_cq_filter_active(void);

// True when we're mid pounce-exchange and know exactly which audio tone the
// partner's next message will arrive on (their own tone — they reply on the
// same AF the whole exchange, standard FT8 convention). *freq_hz_out is set
// to that tone. This is THEIR tone, never ours: for a pounce we deliberately
// transmit on a clear slot away from them, so the two are different numbers -
// returning ours here was a real bug, see ft8_qso.c. False for CQ-run (the
// caller's tone differs from ours and isn't tracked after the initial answer),
// outside WAIT_RPT/WAIT_ROGER/WAIT_RR73, and when their tone isn't known —
// those callers should fall back to no frequency hint.
// Used by ft8_test.c to decode the message we're actually waiting for FIRST,
// so a busy band can't push our own QSO's reply past the reply-on-immediate-
// slot window (FT8_REPLY_TX_WINDOW_MS).
bool ft8_qso_get_priority_freq(int *freq_hz_out);

// OUR outgoing audio tone (Hz) for the running CQ or exchange; 0 when IDLE.
// Unlike ft8_tx_get_tone_hz() this is valid BETWEEN bursts too (the session's
// tone, not just an armed request's), which is what the FT8 screen's tone chip
// displays. Compare ft8_qso_get_priority_freq() for the partner's tone.
int ft8_qso_get_tx_tone_hz(void);

// Move our outgoing audio tone mid-session (Roy KI0ER: WSJT-X lets you retune
// your own TX and the partner doesn't care, because they track our SLOT, not
// our tone - only audio_freq_hz changes here, parity is preserved). Re-arms the
// current message at the new tone. Clamped to FT8_TX_TONE_MIN/MAX_HZ.
// Fails, with a reason in *err, when a burst is ACTIVE (never mid-burst - the
// change would half-apply) or when nothing is running to move.
bool ft8_qso_set_tx_tone_hz(int hz, char *err, size_t err_len);

// Format a coarse SNR (dB) into an FT8 report token ("-07", "+02"), clamped
// to the standard -24..+15 range. Same convention the QSO machine itself
// uses for cqrun_answer()/skip-TX1 reports.
void ft8_qso_fmt_report(int snr_db, char *out, size_t len);

// Build the correct NEXT manual-Transmit message for a tapped decode-list row,
// derived from what that station last sent us relative to our own call (WSJT-X
// double-click semantics): CQ/not-to-us -> TX1 grid; their grid -> our report;
// their report -> R+our report; their R-report -> RR73; their RR73/73 -> 73.
// The report value is OUR measured SNR of them (heard->last_snr_db), not an
// echo. reply_freq_hz is the audio tone for our TX. *is_fresh_grid (optional)
// is set true only for the TX1-grid case (where Auto Pounce still applies).
// Returns false + err on identity/encode failure. LVGL thread only.
bool ft8_qso_build_manual_reply(const ft8_call_t *heard, int reply_freq_hz,
                                ft8_tx_request_t *out, bool *is_fresh_grid,
                                char *err, size_t err_len);

// A manually-built closing message (RR73/73 via the TX modal's Transmit, with
// no machine QSO running) was just armed. Seeds the QSO machine's WAIT_DONE
// state so the existing completion path runs once the burst leaves the air:
// ADIF log entry, pileup removal, "QSO complete" status - the same wrap-up an
// auto QSO gets. Without this a fully-manual exchange was never logged, so
// worked-before knew nothing and the partner's trailing 73 re-entered the
// pileup. No-op if a machine QSO is already in progress (its own path logs).
// LVGL thread only.
void ft8_qso_notify_manual_final(const char *target_call);

// Note the station the operator is working MANUALLY (called on every manual
// Transmit arm). Keeps the partner out of the pileup capture and lets the
// decode list show the amber "working" highlight during a hand-run exchange.
// Expires after ~5 min of no manual activity; cleared on QSO completion.
void ft8_qso_note_manual_target(const char *target_call);

// The station currently "being worked" for UI highlighting: the machine QSO's
// target when an exchange is active, else a fresh manual target (see above).
// Returns false (empty buf) if neither.
bool ft8_qso_get_working_target(char *buf, size_t len);

// True if a decoded message text passes the operator's include/exclude term
// filters (the same ft8_filters_t used by CQ-run auto-reply). Exposed so the
// robot (ft8_robot.c) applies the exact same matching as the manual paths.
// Note: this covers only the term filters; worked-before / plain-CQ toggles
// are enforced by the caller.
bool ft8_filter_match(const char *text, const ft8_filters_t *f);
