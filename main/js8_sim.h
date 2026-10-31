#pragma once

// JS8 simulation mode - phantom stations on the JS8 page, so the screen can be
// exercised without waiting for a band to produce traffic.
//
// ⭐ WHY IT EXISTS. JS8 carries far fewer stations than FT8, and on a quiet
// band there can be nothing to tap for an hour. Every JS8 screen change -
// tap-select, the conversation filter, the macro sheet - needs a station to
// act on, so testing one meant waiting for somebody to transmit. The
// operator's words, 2026-10-31: "there are far too few stations transmitting -
// so it is verry tedious to test it".
//
// ⛔ WHERE IT INJECTS, and what that does NOT prove. ft8_sim.c synthesizes a
// real waveform and runs it through the real decoder, so its SNR is MEASURED.
// This one does not: it builds the real JS8 frame bytes with js8_pack_*() and
// renders them with js8_frame_to_text(), then hands the result to the same two
// sinks the decoder feeds - ft8_screen_record_decode() for the tappable
// station row and js8_chat_add() for the conversation pane. So the message
// codec, the callsign codecs and everything above the decoder are real; the
// GFSK/LDPC layer is skipped and the SNR is announced rather than measured.
//
// That means this simulator CANNOT find a decoder fault, a submode fault or a
// sensitivity regression. It is for the screen and the operator's workflow,
// which is what was tedious. Use the on-air bench for the rest.
//
// SHARES ONE SWITCH with the FT8 and WSPR simulators: qmx_settings_t
// .sim_mode_en, from the FT8 settings drawer. Which simulator runs is decided
// by the protocol - ft8_sim.c idles in JS8, this one idles everywhere else -
// so the operator has one toggle and never a choice to get wrong.
//
// ⛔ NOTHING IS KEYED. ft8_tx.c's ft8_tx_run() and ft8_tx_arm() both check
// sim_mode_en and skip every cat_* call, so a simulated exchange sends no byte
// to the radio. ⚠ Turning sim mode OFF part-way through an exchange has put a
// REAL burst on the air once already - the armed request survives the toggle.
// Stand the exchange down before switching, not during it.
//
// ⚠ A completed simulated QSO logs to the REAL ADIF, exactly as ft8_sim's
// does, because ft8_qso.c is not special-cased. Clear practice contacts out
// afterwards if they are not wanted.

#ifdef __cplusplus
extern "C" {
#endif

// One-time init: spawns the always-running, low-priority simulation task. The
// task is a no-op unless sim mode is on AND the JS8 page is up, so this is
// safe and cheap to call at boot regardless of the saved setting.
void js8_sim_init(void);

#ifdef __cplusplus
}
#endif
