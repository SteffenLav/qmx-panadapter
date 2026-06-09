#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ft8_tx.h"

// v0.13.0: FT8 QSO state machine — auto search-and-pounce.
//
// Driven from two call sites:
//   ft8_qso_start()   – LVGL task (core 0): arm TX1, enter WAIT_RPT
//   ft8_qso_advance() – ft8_task  (core 1): scan decodes, arm TX2/TX3, timeout

typedef enum {
    FT8_QSO_IDLE = 0,
    FT8_QSO_WAIT_RPT,    // TX1 armed/fired; listening for their signal report
    FT8_QSO_WAIT_RR73,   // TX2 armed/fired; listening for RR73/73
    FT8_QSO_WAIT_DONE,   // TX3 (73) armed/fired; wrapping up
    FT8_QSO_DONE,        // QSO complete  (shown for one slot, then auto-IDLE)
    FT8_QSO_TIMEOUT,     // no response within timeout  (sticky until next start)
} ft8_qso_state_t;

void ft8_qso_init(void);

// Arm TX1 and enter the QSO machine.
// tx1_req: the pre-built, pre-encoded TX1 request produced by ft8_tx_build_request().
//   target_call and audio_freq_hz are read from it for the rest of the exchange.
//   Passes it to ft8_tx_arm() — includes the Digi-mode pre-flight.
// Returns false + err string if the arm is refused.
bool ft8_qso_start(const ft8_tx_request_t *tx1_req, char *err, size_t err_len);

// Called from ft8_task after each successful RX slot.
// slot_sec: UTC second the RX slot started (used to match decoded messages
// to this slot and not stale ones from previous slots).
void ft8_qso_advance(int64_t slot_sec);

// Abort QSO and disarm any pending TX. No-op when IDLE.
void ft8_qso_abort(void);

ft8_qso_state_t ft8_qso_get_state(void);
void            ft8_qso_get_target(char *buf, size_t len);
