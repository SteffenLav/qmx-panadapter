#pragma once
#include "lvgl.h"
#include "ft8_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

// FT8 TX confirmation modal - the only path to ft8_tx_arm(). Shown after
// the operator taps a heard-station row ("reply") or the "Call CQ" button;
// the request must already be fully built (and therefore validated/encoded)
// via ft8_tx_build_request() - this modal never builds or encodes, only
// confirms and arms.
//
// Structurally a full-screen overlay copied from identity_config.c's
// scaffold (black backdrop + dark panel + red Cancel / green confirm).

// Build the modal at boot (mirrors identity_config_modal_init - built once
// while internal heap is at its boot-time maximum). Call once from
// ft8_screen_init() or similar. Idempotent.
void ft8_tx_modal_init(void);

// Show the confirmation modal for a freshly-built, already-validated
// request. Displays req->display_text, the base audio frequency, the
// required slot parity (reply) or "next slot" (CQ), and a live 1 Hz
// "fires in ~Ns" countdown. Takes its own copy of *req - the caller's
// copy may go out of scope immediately after this returns.
//
//   Cancel    closes with no side effects (nothing armed, radio untouched)
//   Transmit  calls ft8_tx_arm(); on success closes and the request is now
//             live (visible via the screen's TX-state indicator); on
//             failure (e.g. QMX won't confirm Digi mode) shows the
//             returned reason inline and stays open for retry/cancel
void ft8_tx_modal_show(const ft8_tx_request_t *req);

// Hide the modal immediately (e.g. screen teardown while it's open).
// No side effects - equivalent to the user tapping Cancel.
void ft8_tx_modal_hide(void);

#ifdef __cplusplus
}
#endif
