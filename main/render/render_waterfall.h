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

// Re-seed the per-bin noise-floor tracker from the next tick's spectrum.
// Call when a stale floor (e.g. captured before the QMX was streaming)
// would otherwise take many seconds to decay up to the real noise level.
void render_waterfall_floor_reset(void);

// Live waterfall colorisation controls (Waterfall drawer). Each takes effect
// on the next render tick, so a slider drag is visible immediately.
//  - black level: dB above the floor that maps to LUT black (0..30, default 9)
//  - contrast span: dB that fill the colour ramp to red (10..80, default 45)
//  - floor blend: 0 = global floor only, 1 = full per-bin adaptive floor
void render_waterfall_set_black_level(float db);
void render_waterfall_set_contrast_db(float db);
void render_waterfall_set_floor_blend(float blend);
