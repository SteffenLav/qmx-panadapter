#pragma once

// FT8 Time Calibration modal.
//
// Three big oblong boxes: [HH] : [MM] : [SS]
//
// HH / MM: pre-filled from current system UTC. Tap to open a 0-9 numpad.
//   HH validated 0-23; MM validated 0-59. ✓ confirms, ⌫ backspaces.
//
// SS: auto-syncs every second from the last FT8 decoded candidate's timing
//   (sub-second precision). Tap the SS box to freeze/unfreeze.
//
// Apply: uses time_sync_apply_correction_ms for sub-second precision when
//   only SS changed; falls back to time_sync_set_manual when HH or MM
//   were edited by the user.
//
// Must be called only from the LVGL task (button callbacks, timers).

void ft8_time_modal_show(void);
