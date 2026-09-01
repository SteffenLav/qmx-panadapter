#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// FFT configuration constants (visible to consumers like the renderer in Phase 5)
#define DSP_FFT_SIZE       1024
#define DSP_SAMPLE_RATE_HZ 48000

// Phase 5.8: dBm calibration offset. Added to every dB value before display so
// readings match real-world signal strength. Procedure: with QMX on dummy load,
// log the per-second MEDIAN dB across all bins (= noise floor in raw dB);
// the offset is then -130 - median.
// In the header because spur_map.c must UNDO it: the spectra it compares are
// dBm, but the powers it subtracts go back into raw FFT magnitude-squared.
#ifndef DSP_DB_CALIBRATION_OFFSET
#define DSP_DB_CALIBRATION_OFFSET -148.0f  /* measured against QMX on dummy load (-130 dBm floor target) */
#endif

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
 * @brief Select the FFT analysis window (affects spectrum + waterfall sharpness).
 *
 * 0 = Blackman-Harris (default; lowest spectral leakage, widest main lobe so
 *     signals look a touch fatter), 1 = Hann (narrowest main lobe -> sharpest
 *     signals, slightly higher side-lobe haze), 2 = Nuttall (middle ground).
 * Rebuilds the window in place; the change is visible on the next FFT frame.
 * Safe to call after dsp_init() at any time.
 */
/* #298: the dial the samples behind the CURRENT spectrum were captured under.
 * Drawing must locate bins with this, not with the live dial - the pipeline is
 * hundreds of ms deep and its depth was measured varying better than 2:1. */
uint32_t dsp_get_spectrum_dial_hz(void);

void dsp_set_window(uint8_t idx);

/**
 * @brief Quiet the FFT/FT8 processing during an outbound network transfer.
 *
 * When true, fft_task drains the audio ring but skips all FFT/FT8 work and
 * sleeps, freeing the core so a QRZ/eQSL upload's TLS handshake isn't preempted
 * (fft_task is priority 4, the upload task priority 3). Cascades to pause FT8
 * capture/decode. Resumes instantly when cleared. Wrap transfers true/false.
 */
void dsp_set_transfer_quiet(bool quiet);

// True once fft_task has actually observed a quiet request and drained the
// audio ring. Setting the flag is not enough on its own - it is cooperative,
// and anything that needs a genuinely idle system (a long flash verify) must
// wait for this, bounded, before starting.
bool dsp_transfer_quiet_settled(void);

/**
 * @brief Get a snapshot of the latest spectrum.
 *
 * @param[out] dst       Destination array of DSP_FFT_SIZE floats (dB units)
 * @return ESP_OK if a fresh spectrum was copied, ESP_ERR_NOT_FOUND if no
 *         spectrum has been computed yet.
 */
esp_err_t dsp_get_spectrum(float *dst);

// Average LINEAR power over the next `frames` FFT frames, then convert to dB.
// A single frame is Rayleigh-noisy (+/-13 dB bin to bin), so anything that has
// to compare two spectra - spur_map.c's dial-nudge detector - needs averaging
// to tell a real difference from luck. Averaging in the linear domain matters:
// averaging dB would bias every result low.
void dsp_avg_start(uint32_t frames);

// True once the run has completed; copies DSP_FFT_SIZE dB values into `dst` and
// disarms. Poll it - the accumulation happens on the FFT task.
bool dsp_avg_ready(float *dst);

// Phase 5.10D: peak dBm in a window centered on the VFO bin.
// center_bin: index of the VFO bin in the raw (non-fftshifted) FFT array
// (i.e. ui_get_if_bin_shift(DSP_FFT_SIZE), wrapped into [0, DSP_FFT_SIZE)).
// half_width_bins=64 at 48 kHz/1024 ~ ±3 kHz.
// Returns peak via *peak_dbm. ESP_ERR_NOT_FOUND if no spectrum yet.
esp_err_t dsp_get_peak_dbm_around_vfo(int center_bin, int half_width_bins, float *peak_dbm);

// Step 3 v0.10 FT8 RX: one-shot slot capture.
// When armed, the FFT task diverts its 1024-sample reads through a mixer
// (-12 kHz IF removal via fs/4 sign-flip), then an esp-dsp 31-tap FIR
// decimator (/4 -> 12 kHz mono real), filling dst with exactly 180000
// samples (= 15 s at 12 kHz). DC blocker / panadapter FFT / spectrum push
// are skipped during capture; waterfall freezes for the duration.
// dst MUST point to 180000 floats in PSRAM (heap_caps_malloc).
// Returns ESP_OK on success, ESP_ERR_TIMEOUT if no audio after timeout_ms.
esp_err_t dsp_ft8_capture(float *dst_180000, uint32_t timeout_ms);

// Incremental capture (v0.18 fast-decode + boundary-discard fix): arm / poll /
// finalize variant of dsp_ft8_capture. Audio is sourced from a continuous
// pre-ring the producer fills every window while in FT8 mode, so the window is
// anchored to the UTC boundary rather than to when the caller armed it. Usage:
//   dsp_ft8_capture_begin(scratch, 180000, start_off_ms * 12);
//   while (...) { int n = dsp_ft8_capture_progress(); process new whole blocks; delay; }
//   dsp_ft8_capture_finish(ms_to_boundary);   // disarms + zero-pads the tail
// backfill_samples = how many decimated samples back from "now" the UTC boundary
// was (start_off_ms * 12), so begin() can prepend that gap from the pre-ring.
// dst MUST point to >= target_samples floats in PSRAM and stay valid until
// finish() returns. progress() returns the decimated-12 kHz sample count so far.
esp_err_t dsp_ft8_capture_begin(float *dst, uint32_t target_samples,
                                uint32_t backfill_samples);
int       dsp_ft8_capture_progress(void);
esp_err_t dsp_ft8_capture_finish(uint32_t timeout_ms);

// ---- CW audio out (v0.18+): forward raw I/Q to the CW demodulator ----------
// The CW demodulator needs EVERY I/Q sample in real time. fft_task is a
// snapshot consumer (~15 windows/s, lets the rest overflow) so it is NOT a
// usable audio source. Instead audio.c's real-time producer (process_rx) calls
// dsp_cw_forward() for every decoded chunk into an internal PSRAM byte ring;
// cw_audio_task drains it with dsp_cw_read(). Zero cost when forwarding is off.
void   dsp_cw_forward_enable(bool en);                       // consumer enables/disables
void   dsp_cw_forward(const int16_t *pairs, size_t n_pairs); // producer (audio.c); no-op if off
// Read up to n_pairs interleaved int16 stereo pairs into dst (dst holds
// n_pairs*2 int16). Returns pairs actually read (0 on timeout).
size_t dsp_cw_read(int16_t *dst, size_t n_pairs, uint32_t timeout_ms);

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
