#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CW audio out (v0.18+): demodulate the tuned CW signal from the QMX I/Q
// stream and play it on the Tab5's built-in speaker / headphone jack, so a
// CW operator can listen without external audio gear.
//
// The QMX delivers raw I/Q only (the panadapter's FFT input); there is no
// audio from the radio. We recover a CW sidetone ourselves: the dsp.c
// fft_task forwards each 1024-pair I/Q frame (when enabled), and cw_audio_task
// does fs/4 IF removal -> narrow band-pass at the CW offset -> AGC -> ES8388.
//
// Output is gated to CW / CW-R mode; in any other mode the path is idle
// (forwarding off, task sleeping) so there is zero cost on SSB/FT8/etc.

// One-time bring-up: open the ES8388 codec (48 kHz, 16-bit stereo) via the
// BSP and spawn cw_audio_task. Reads persisted enable/volume from settings.
// Safe to call once from app_main after settings_init().
void cw_audio_init(void);

// Open the ES8388/I2S output path early — call this BEFORE bsp_usb_host_start()
// so I2S can claim its DMA-capable RAM before the USB UAC stream consumes the
// pool. No-op (and no I2S/DMA claimed) unless CW audio is persisted-enabled.
void cw_audio_preopen(void);

// Enable / disable CW audio output (persisted to NVS).
void cw_audio_set_enabled(bool en);
bool cw_audio_is_enabled(void);

// Output volume, 0..100 (persisted to NVS).
void cw_audio_set_volume(uint8_t vol_0_100);
uint8_t cw_audio_get_volume(void);

#ifdef __cplusplus
}
#endif
