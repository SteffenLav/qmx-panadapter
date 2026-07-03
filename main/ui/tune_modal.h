#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the Antenna Tune modal at boot. Call once from ui_init() when
// internal heap is at its boot-time maximum - same fragmentation-cliff
// rationale as wifi_config_modal_init(). The modal is hidden until
// tune_modal_show() unhides it.
void tune_modal_init(void);

// Show the Antenna Tune modal as a full-screen overlay. Caller (the
// drawer's "Antenna Tune" button) closes the settings drawer first.
// QMX SWR Tune mode (MD8;, 1_04+ firmware only) - see
// docs/qmx-1_04-cat-comparison.md for the CAT-level design notes.
void tune_modal_show(void);

#ifdef __cplusplus
}
#endif
