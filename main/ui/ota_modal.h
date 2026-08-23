// ota_modal - the firmware-update conversation, in the middle of the screen
// where it can be read (#239).
//
// Replaces a flow that lived entirely in the bottom bar's ~264 px version slot:
// an offer squeezed into ~20 characters, a 700 ms long-press to accept it, a
// progress percentage in the same slot, and then a second long-press to
// restart. Don N2VGU reported the wording as confusing and he was right about
// the cause - there was no room to say what was actually happening.
//
// The bar keeps ANNOUNCING (ambient, never interrupting - an update is not
// urgent, and that rule predates this file). It just no longer has to hold the
// whole conversation. One tap on it opens this.
//
// ⚠ Opening this is deliberately the ONLY thing that tap does, which is what
// lets the gesture be a tap at all: a stray brush costs a dismissible window,
// not a download or a reboot. The 700 ms hold existed purely to make a stray
// brush safe.

#pragma once
#include <stdbool.h>

// Open on whatever state the device is actually in - offer, downloading,
// ready to restart, failed, or already up to date. LVGL thread only.
void ota_modal_show(void);

bool ota_modal_is_open(void);
