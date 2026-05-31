#pragma once
#include "esp_err.h"

// Initialize the waterfall renderer (allocates row scratch).
esp_err_t render_waterfall_init(void);

// Called by the render task each tick AFTER the spectrum line.
// Pulls latest spectrum, maps dB->colors via LUT, pushes one row to UI.
// The spectrum argument is the same scratch buffer the spectrum line used,
// to avoid a second dsp_get_spectrum() call (saves a mutex acquire).
void render_waterfall_tick(const float *spectrum, int n_bins);

// Switch the active colour map. idx range: 0=Thermal 1=Viridis 2=Turbo 3=Grayscale.
// Rebuilds the LUT in-place; visible on the next render tick.
void render_waterfall_set_colormap(uint8_t idx);
