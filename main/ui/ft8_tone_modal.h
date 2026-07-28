// TX tone picker - move our own FT8 audio offset mid-session.
//
// Requested by Roy KI0ER (2026-07-27): the tone was chosen silently by
// ft8_find_clear_tone_hz() and never displayed OR adjustable, so there was no
// way to get off a frequency that turned out to be busy. WSJT-X lets you
// retune your own TX for exactly this reason - the partner tracks our SLOT,
// not our tone. Opened from the "TX <n> Hz" chip on the FT8 screen.
//
// The move itself lives in ft8_qso_set_tx_tone_hz(), which refuses while a
// burst is ACTIVE ("mid-QSO yes, mid-burst no").

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Build the (hidden) modal once. Safe to call repeatedly; call from ui_init.
void ft8_tone_modal_init(void);

// Show it, seeded with the session's current tone.
void ft8_tone_modal_show(void);

#ifdef __cplusplus
}
#endif
