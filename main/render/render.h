#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Initialize the render subsystem. Call after dsp_init().
esp_err_t render_init(void);

// Phase 5.10D Stage 2: runtime EMA smoothing setter
void render_set_ema_alpha(float alpha);

// Diagnostic "FT8 sync lines" drawer toggle: when on, the waterfall ticks
// twice per render period (2x scroll speed) so the FT8-sync-vs-SNTP slot
// marker lines (drawn in ui.c's ui_push_waterfall_row) separate out and
// scroll past faster, easier to watch in real time. Off = normal 1x.
void render_set_waterfall_2x(bool on);
