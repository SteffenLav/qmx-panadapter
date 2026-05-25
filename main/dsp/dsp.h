#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// FFT configuration constants (visible to consumers like the renderer in Phase 5)
#define DSP_FFT_SIZE       1024
#define DSP_SAMPLE_RATE_HZ 48000

// Bin frequency width = sample_rate / FFT_SIZE = 46.875 Hz at 48000/1024
// Total span = sample_rate (i.e., -24 kHz to +24 kHz around tuned center)

/**
 * @brief Initialize the DSP subsystem.
 *
 * Phase 4.1: Initialize esp-dsp, allocate FFT working buffers (window,
 *            interleaved real/imag input, magnitude output), prove the
 *            FFT primitive runs end-to-end on a synthetic input.
 * Phase 4.2+: replaces the audio.c stub consumer with a real FFT task.
 *
 * Must be called AFTER audio_init() so the ring buffer exists.
 */
esp_err_t dsp_init(void);

/**
 * @brief Get a snapshot of the latest spectrum.
 *
 * @param[out] dst       Destination array of DSP_FFT_SIZE floats (dB units)
 * @return ESP_OK if a fresh spectrum was copied, ESP_ERR_NOT_FOUND if no
 *         spectrum has been computed yet.
 */
esp_err_t dsp_get_spectrum(float *dst);

// Phase 5.10D: peak dBm in a window centered on the spectrum center bin
// (which is the VFO). half_width_bins=64 at 48 kHz/1024 ~ ±3 kHz.
// Returns peak via *peak_dbm. ESP_ERR_NOT_FOUND if no spectrum yet.
esp_err_t dsp_get_peak_dbm_around_vfo(int half_width_bins, float *peak_dbm);
