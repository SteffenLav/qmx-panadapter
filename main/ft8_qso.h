#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ft8_tx.h"

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
// Extra field of the current outgoing message (grid / report / R-report / RR73 / 73).
// Empty string when IDLE or no message is armed.
void            ft8_qso_get_cur_extra(char *buf, size_t len);

// True while a CQ-originated session is active (calling CQ or working the
// station that answered). The decode-list UI uses this to hide other stations'
// CQ rows so replies to us stand out. Always false for pounce sessions.
bool ft8_qso_cq_filter_active(void);

// During a pounce exchange (WAIT_RPT / WAIT_RR73), returns true and writes the
// partner's audio tone (Hz) to *freq_hz_out. decode_slot reorders the candidate
// list to decode that tone first, so the reply is found early enough to fire on
// the immediate slot (FT8_REPLY_TX_WINDOW_MS) even on a crowded band. Returns
// false otherwise (idle, or CQ-run where s_freq_hz is our own tone).
bool ft8_qso_get_priority_freq(int *freq_hz_out);
