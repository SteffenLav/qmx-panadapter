#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Initialize the render subsystem. Call after dsp_init().
esp_err_t render_init(void);

// Phase 5.10D Stage 2: runtime EMA smoothing setter
void render_set_ema_alpha(float alpha);

// Waterfall scroll rate in ROWS PER SECOND, 1..40, default 10. The
// spectrum/S-meter cadence in render_task is untouched at every setting - this
// only changes how many waterfall rows that same frame is pushed into, since a
// faster or slower FFT rate is not what "rate" means here.
//
// Above 10 the extra rows are duplicates of the one fresh frame (there is no
// newer sample inside the period); below 10 the push is simply skipped on some
// ticks, which is the point - the waterfall is ~21 ms of a ~64 ms frame draw,
// and a skipped push is a whole canvas invalidation that core 0 never has to
// blit. Was a 1..4x multiplier; one axis, one setting, both directions.
void render_set_waterfall_rows_per_s(uint8_t rows);
