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

// Occupancy-strip palette, shared with the FT8 pane's mini strip so the two
// pictures of the same data can never drift apart. Deliberately not the
// waterfall palette: this is a discrete availability map, not signal strength,
// and must not read as one.
#define FT8_TONE_COL_FREE     0x1E6B34   // green: nothing decoded here
#define FT8_TONE_COL_BUSY     0x7A2020   // red: a station or its guard band
#define FT8_TONE_COL_UNKNOWN  0x3A3F46   // grey: nothing heard at all yet
#define FT8_TONE_COL_PICK     0xFFA040   // amber: where WE transmit (matches the TX label)
#define FT8_TONE_COL_PARTNER  0x30C0D0   // cyan: the partner (matches the PWR/SWR line)

// Build the (hidden) modal once. Safe to call repeatedly; call from ui_init.
void ft8_tone_modal_init(void);

// Show it, seeded with the session's current tone.
void ft8_tone_modal_show(void);

#ifdef __cplusplus
}
#endif
