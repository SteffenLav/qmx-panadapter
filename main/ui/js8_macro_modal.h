#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The JS8 macro sheet: the twelve directed commands, on the station that is
// already selected.
//
// WHY A SHEET AND NOT BUTTONS. The JS8 page carries three panes and a TX bar,
// and v1.23.0 removed the Free text button to make room for what was already
// there. Twelve permanent buttons do not fit, and the answer is never a
// smaller font - the readers of this display are mostly over 60.
//
// HOW IT OPENS. Tap a row to select a station, exactly as before. Tap the
// SAME row again and this opens, addressed to that station. The second tap is
// the point: no sheet can appear over a station the operator has not already
// chosen.
//
// WHAT IT SENDS. One directed frame per press, through
// ft8_screen_view_js8_directive(). Nothing here transmits on its own and
// nothing is queued: a press arms one frame for the next slot, and the
// refusal - busy, no callsign, a name too long to carry the command - is
// shown as a toast rather than being swallowed.

// Build the sheet (idempotent). Optional - show() builds lazily.
void js8_macro_modal_init(void);

// Show it for the currently selected station. A no-op when nothing is
// selected: the caller is the row re-tap, which by definition has one.
void js8_macro_modal_show(void);

#ifdef __cplusplus
}
#endif
