#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// First-boot onboarding. Builds a small "Connect to your WiFi?" prompt at boot
// (heap is at its maximum then) and, a moment after boot, runs the one-time
// flow if it hasn't been done yet:
//   1. If WiFi isn't configured, ask "Connect to your WiFi?"
//        Yes!           -> open the WiFi modal
//        Don't ask again-> skip it, never ask again
//   2. Then offer the callsign/grid modal if those aren't set yet.
// A persisted flag (settings onboarded) makes the whole thing happen at most
// once. Call once from ui_init().
void onboarding_init(void);

#ifdef __cplusplus
}
#endif
