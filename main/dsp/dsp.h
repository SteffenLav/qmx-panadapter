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

// Phase 5.10D: peak dBm in a window centered on the VFO bin.
// center_bin: index of the VFO bin in the raw (non-fftshifted) FFT array
// (i.e. ui_get_if_bin_shift(DSP_FFT_SIZE), wrapped into [0, DSP_FFT_SIZE)).
// half_width_bins=64 at 48 kHz/1024 ~ ±3 kHz.
// Returns peak via *peak_dbm. ESP_ERR_NOT_FOUND if no spectrum yet.
esp_err_t dsp_get_peak_dbm_around_vfo(int center_bin, int half_width_bins, float *peak_dbm);

// Snap-to-peak helper for touch-to-tune. Given a touched frequency offset
// from the QMX dial (in Hz, can be negative), search ±radius_hz for the
// strongest spectrum bin. If the peak exceeds the local mean by >3 dB,
// returns its Hz offset in *out_hz; otherwise returns center_hz unchanged.
// IF offset (+12 kHz baseband shift) is handled internally.
esp_err_t dsp_find_peak_hz_around(int32_t center_hz, int32_t radius_hz, int32_t *out_hz);

// Step 3 v0.10 FT8 RX: one-shot slot capture.
// When armed, the FFT task diverts its 1024-sample reads through a mixer
// (-12 kHz IF removal via fs/4 sign-flip), then an esp-dsp 31-tap FIR
// decimator (/4 -> 12 kHz mono real), filling dst with exactly 180000
// samples (= 15 s at 12 kHz). DC blocker / panadapter FFT / spectrum push
// are skipped during capture; waterfall freezes for the duration.
// dst MUST point to 180000 floats in PSRAM (heap_caps_malloc).
// Returns ESP_OK on success, ESP_ERR_TIMEOUT if no audio after timeout_ms.
esp_err_t dsp_ft8_capture(float *dst_180000, uint32_t timeout_ms);

// Incremental capture (v0.18 fast-decode): arm / poll / finalize variant of
// dsp_ft8_capture, letting the caller run the FT8 STFT block-by-block during
// the slot instead of after. Usage per slot:
//   dsp_ft8_capture_begin(scratch, 180000);
//   while (...) { int n = dsp_ft8_capture_progress(); process new whole blocks; delay; }
//   dsp_ft8_capture_finish(ms_to_boundary);   // disarms + zero-pads the tail
// dst MUST point to >= target_samples floats in PSRAM and stay valid until
// finish() returns. progress() returns the decimated-12 kHz sample count so far.
esp_err_t dsp_ft8_capture_begin(float *dst, uint32_t target_samples);
int       dsp_ft8_capture_progress(void);
esp_err_t dsp_ft8_capture_finish(uint32_t timeout_ms);

// ---- Zoom-FFT (v0.16.0): real frequency-resolution increase at zoom > x1 ---
// The fft_task mixes the pan-center down to DC, low-pass filters, decimates
// by a power-of-two factor D in {1,2,4,8,16} (chosen as the largest such D
// <= the requested zoom, so D divides DSP_FFT_SIZE evenly), accumulates
// DSP_FFT_SIZE decimated complex samples and runs a second 1024-pt FFT.
// The display then applies any *residual* zoom (zoom / D, in [1,2)) on top
// of this already-higher-resolution spectrum using the existing
// magnification math with center_bin = 0.
//
// if_bin_shift: ui_get_if_bin_shift(DSP_FFT_SIZE), passed in by the caller
// (ui.c) to avoid a dsp<->ui include cycle.
void dsp_set_zoom(float zoom_factor, int pan_offset_bins, int if_bin_shift);

// Returns the currently active decimation factor D (1, 2, 4, 8, or 16).
// D == 1 means zoom-FFT is inactive; the display should use the normal
// dsp_get_spectrum() output with the existing zoom/pan math.
int dsp_get_zoom_decim(void);

// Residual zoom to apply on top of the zoom-FFT spectrum (zoom_factor / D).
float dsp_get_zoom_residual(void);

// Latest zoom-FFT spectrum (DSP_FFT_SIZE dB values, same layout as
// dsp_get_spectrum: index 0 = DC = pan center, non-fftshifted), or NULL if
// zoom-FFT is inactive (D==1) or no frame yet (accumulation still filling
// after a zoom/pan change). Lock-free double-buffered: the returned pointer
// stays valid and unmodified until the next call to this function from the
// same caller (fft_task never writes to a buffer once it's been handed out).
const float *dsp_get_zoom_spectrum(void);
