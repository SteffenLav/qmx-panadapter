#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the Operator Identity modal at boot. Call once from ui_init()
// when internal heap is at its boot-time maximum (~199 KB free). See
// wifi_config_modal_init() for the rationale.
void identity_config_modal_init(void);

// Show the Operator Identity modal as a full-screen overlay.
// User enters callsign and Maidenhead grid, then Save persists both
// to NVS via settings_set_my_callsign/grid. Cancel closes without
// changes. Safe to call repeatedly; no-op if already open.
void identity_config_modal_show(void);

#ifdef __cplusplus
}
#endif
