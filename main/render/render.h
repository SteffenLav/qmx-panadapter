#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Initialize the render subsystem. Call after dsp_init().
esp_err_t render_init(void);

// Phase 5.10D Stage 2: runtime EMA smoothing setter
void render_set_ema_alpha(float alpha);

// Waterfall scroll speed, in whole multiples of the normal 10 rows/s (the
// spectrum/S-meter cadence in render_task is untouched either way - this
// only changes how many EXTRA rows the waterfall gets pushed per render
// period, all from the same frame, since a faster FFT rate is not what
// "speed" means here). 1 = normal. Clamped to [1,4]; 4x was the old
// FT8-sync-lines diagnostic's own ceiling (called it "2x"/"3x speed" - it
// pushed one extra tick beyond the diagnostic's own reasoning, this is the
// same mechanism generalised into an operator-facing drawer setting).
void render_set_waterfall_speed_mult(uint8_t mult);
