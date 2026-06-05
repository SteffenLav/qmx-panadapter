#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Show the Operator Identity modal as a full-screen overlay.
// User enters callsign and Maidenhead grid, then Save persists both
// to NVS via settings_set_my_callsign/grid. Cancel closes without
// changes. Safe to call repeatedly; no-op if already open.
void identity_config_modal_show(void);

#ifdef __cplusplus
}
#endif
