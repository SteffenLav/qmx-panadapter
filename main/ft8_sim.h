#pragma once

// FT8 simulation mode (practice/testing) - phantom stations to work without
// any real over-the-air contact and, crucially, WITHOUT EVER KEYING A REAL,
// POSSIBLY-CONNECTED QMX. Toggled from the FT8 settings drawer (FT8 mode
// only - see ui.c DRAWER_SEC_SIMMODE), persisted as qmx_settings_t.sim_mode_en.
//
// Design: a small set of fixed phantom callsigns periodically "call CQ" by
// injecting a synthesized-and-decoded message into the normal FT8 decode
// list (ft8_screen_record_decode) - real audio synthesis + the real on-device
// STFT/LDPC decode pipeline (ft8_synth_and_decode(), the same one validated
// by ft8_arrl_fd_e2e_selftest()), so what shows up went through the same
// code real RF would. Tap one to pounce, or call CQ yourself and a phantom
// will answer, exactly like a real contact - because from ft8_qso.c's point
// of view, it IS a real contact: this module never touches ft8_qso.c's
// internals, it only ever feeds the same decode-list input a real RX would.
//
// The other half of the safety story lives in ft8_tx.c: ft8_tx_run() and
// ft8_tx_arm() both check qmx_settings_t.sim_mode_en directly and skip every
// cat_* call when it's set, so a completed simulated QSO never sent a single
// byte to the radio - this module doesn't need its own interlock for that.
//
// A completed simulated QSO logs to the real ADIF file via the normal
// ft8_qso.c WAIT_DONE path (no special-casing) - that's intentional (lets
// you test the logging/upload path too), but means you may want to clear the
// ADIF log afterward if you don't want practice contacts mixed with real
// ones (see adif_log_clear()).

#ifdef __cplusplus
extern "C" {
#endif

// One-time init: spawns the (always-running, low-priority) simulation task.
// The task is a no-op whenever qmx_settings_t.sim_mode_en is false, so it's
// safe/cheap to call this once at boot regardless of the saved setting.
void ft8_sim_init(void);

#ifdef __cplusplus
}
#endif
