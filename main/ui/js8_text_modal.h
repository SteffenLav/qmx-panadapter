#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// JS8 free-text composer. Opened from the "Free text" button on the JS8 view.
//
// ⭐ WHY THIS EXISTS AT ALL. JS8 free-text transmit shipped working but with no
// surface: the only way to send a message was POST /api/cmd {"action":"js8_text"}.
// The operator's verdict was the right one - "how can users be happy for an
// api?" - so this is the box to type in.
//
// All THREE keyboards reach it, and none of that is new code here: the snap-on
// keyboard and a BLE keyboard both arrive through ui_kbd_note_focus(), and
// ui_osk_show() puts up the on-screen keyboard only when no BLE keyboard is
// connected. See the long comment at ui_osk_show() in ui.c.

// Build the modal (idempotent). Optional - show() builds lazily anyway.
void js8_text_modal_init(void);

// Show the composer, empty and ready to type.
void js8_text_modal_show(void);

#ifdef __cplusplus
}
#endif
