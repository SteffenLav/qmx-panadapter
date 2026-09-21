#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RX audio out: demodulate the tuned signal from the QMX I/Q stream and play
// it on the Tab5's built-in speaker / headphone jack, so an operator can
// listen without external audio gear - "hear what the panadapter is showing."
//
// Supersedes the CW-only cw_audio.c prototype (see memory
// project_rx_audio_track.md, Track A). Two things changed on top of that
// prototype's proven pieces (the I2S preopen-before-USB-host ordering, the
// internal-RAM forward ring in dsp.c):
//
//   1. Mode-aware filtering (CW/CW-R narrow-band around the CW offset,
//      USB/LSB a voice-width passband) instead of CW only.
//   2. The consumer task is now STRUCTURALLY unable to preempt fft_task: it
//      runs at a priority BELOW fft_task's (RX_AUDIO_TASK_PRIORITY <
//      DSP_FFT_TASK_PRIORITY in dsp.h), so FreeRTOS's preemptive scheduler
//      cannot hand it the CPU while fft_task is ready - regardless of wake
//      cadence. On top of that, the task is fully BLOCKED (no wakeups at
//      all) whenever RX audio is disabled, which is the default. The
//      original cw_audio_task's bug was priority 6 (ABOVE fft_task's 4)
//      waking every 120 ms even when idle - see cw_audio.c's git history
//      and CLAUDE.md's "CW audio (shelved)" section for the post-mortem.
//
// Gated to CW/CW-R/USB/LSB; any other mode (DiGi, AM, TUNE, unknown) is
// treated as unsupported and the path stays idle.

// One-time bring-up: open the ES8388 codec (48 kHz, 16-bit stereo) via the
// BSP and spawn rx_audio_task. Reads persisted enable/volume from settings.
// Safe to call once from app_main after settings_init().
void rx_audio_init(void);

// Open the ES8388/I2S output path early - call this BEFORE bsp_usb_host_start()
// so I2S can claim its DMA-capable RAM before the USB UAC stream consumes the
// pool. No-op (and no I2S/DMA claimed) unless RX audio is persisted-enabled.
void rx_audio_preopen(void);

// Enable / disable RX audio output (persisted to NVS). Enabling wakes the
// consumer task if it was blocked; disabling lets it go back to sleep on its
// next iteration.
void rx_audio_set_enabled(bool en);
bool rx_audio_is_enabled(void);

// Output volume, 0..100 (persisted to NVS).
void rx_audio_set_volume(uint8_t vol_0_100);
uint8_t rx_audio_get_volume(void);

// ---- Live tuning (RAM only, NOT persisted - dev/field iteration without a
// rebuild+reflash+QMX-power-cycle, see main/net/webserver.c's "rxaudio"
// /api/cmd action). Each setter takes effect on the very next sample; no
// restart, no re-enable needed. -1 in any field of rx_audio_get_tuning()'s
// struct never happens - it is always fully populated, this is just the
// read side of the same five values.
typedef struct {
    float out_clamp;     // hard clip before int16 cast (headroom), >0
    float agc_target;    // AGC target envelope level, >0
    float agc_attack;    // per-sample AGC attack coefficient, 0..1
    float agc_release;   // per-sample AGC release coefficient, 0..1
    float agc_gain_max;  // AGC gain ceiling, >0
} rx_audio_tuning_t;

void rx_audio_set_out_clamp(float v);
void rx_audio_set_agc_target(float v);
void rx_audio_set_agc_attack(float v);
void rx_audio_set_agc_release(float v);
void rx_audio_set_agc_gain_max(float v);
void rx_audio_get_tuning(rx_audio_tuning_t *out);

// ---- Panoramic CW (2026-09-20) ---------------------------------------------
// Splits the CW filter's own selected width in half AT THE TUNED PITCH and
// pans each half hard L/R - two different stations sitting on opposite
// sides of the dial separate spatially (one leans left, one leans right)
// instead of both landing in one mono note. This is frequency-based stereo
// SEPARATION OF MULTIPLE SIGNALS, not a weak-signal aid applied to one -
// see rx_audio.c for the "pre-shift + real lowpass + remix" implementation
// and why an earlier detuned-tone attempt was the wrong technique for this.
// CW/CW-R only; any other mode is plain mono regardless of this setting.
// RAM-only, not persisted - same class of live tuning as the AGC params
// above, takes effect on the next retune (same loop that already tracks
// mode/center-frequency changes).
/* Re-read the stored gain/pan settings and apply them to the live DSP. Called
 * by rx_audio_init(), and by config import - which otherwise writes settings
 * the audio path never hears until the next reboot. */
void rx_audio_apply_settings(void);

void rx_audio_set_binaural_enabled(bool en);
bool rx_audio_get_binaural_enabled(void);

// Cross-feed fraction, 0.0 (hard L/R split) .. 0.5 (fully centered/mono) -
// widens the effective stereo blend zone so a station near the tuned pitch
// doesn't snap entirely to one ear. Live, RAM-only. Default 0.15 (0.35 was
// tried first and confirmed on the air as too much - collapsed the image).
// A post-mix trick - see rx_audio_set_pan_overlap() for the structural
// control that does not trade away separation to get it.
void rx_audio_set_pan_blend(float v);
float rx_audio_get_pan_blend(void);

// How much the two half-band filters THEMSELVES overlap in the middle of
// the passband, 0.0 (original hard split) .. 1.0 (each half nearly as wide
// as the whole original filter). This is the structural fix for "fixing the
// middle weakens the sides" - pan_blend above can only redistribute energy
// the filters already put entirely on one side; overlap changes how much
// energy near the crossover the filters hand to BOTH sides to begin with,
// so a station well off-center keeps full separation while a centered one
// gets real presence in both ears. Live, RAM-only, takes effect on the next
// filter rebuild (automatic, same loop pass). Default 0.3.
void rx_audio_set_pan_overlap(float v);
float rx_audio_get_pan_overlap(void);

// Mid/side stereo width, 1.0 = unchanged, up to 3.0 = strongly widened,
// down to 0.0 = mono. Exaggerates the L-R difference AFTER overlap/blend
// have shaped where energy goes - this is "how far apart do the two ears
// end up", not "where does a station near the crossover go". Live,
// RAM-only, plain per-sample multiply (no filter rebuild). Default 1.8.
void rx_audio_set_pan_width(float v);
float rx_audio_get_pan_width(void);

// Output samples clamped at out_clamp since the last call - an objective
// answer to "how much is it actually clipping", not a guess by ear. Reading
// it resets the count, so each call reports the rate since the previous one.
uint32_t rx_audio_take_clip_count(void);

// Real timing/starvation data, added 2026-09-04 after several rounds of
// theorizing about the DSP cost and the forward ring's health with no way to
// actually check either. frame_us_avg/max cover ONLY the NCO+FIR+AGC math
// (not the I2S write) - compare against ~21333 us
// (DSP_FFT_SIZE/DSP_SAMPLE_RATE_HZ) to see whether the math itself is keeping
// up. read_us_avg/max is how long dsp_rxaudio_read() itself took to return
// (its own internal 60 ms timeout, separate from frame timing); write_us is
// the blocking esp_codec_dev_write() call. read_timeouts is how many times
// the read came back with zero pairs (forward ring starved). A fix to the
// math alone (frame_us) that leaves read_us or write_us as the real cost
// will show no audible improvement, which is exactly what happened once
// already - check all three, not just frame_us. All fields are
// since-the-last-call rates, same convention as the clip count.
typedef struct {
    uint32_t frame_us_avg;
    uint32_t frame_us_max;
    uint32_t frame_count;
    uint32_t read_timeouts;
    uint32_t read_us_avg;
    uint32_t read_us_max;
    uint32_t write_us_avg;
    uint32_t write_us_max;
} rx_audio_diag_t;

void rx_audio_take_diag(rx_audio_diag_t *out);

#ifdef __cplusplus
}
#endif

/* ---- Recorder (2026-09-06, chirp characterisation) ----------------------
 *
 * Keeps the exact samples handed to the codec so the artefact can be LOOKED AT
 * rather than inferred from counters. One-shot: records `seconds` and stops, so
 * the window is contiguous and cannot be overwritten mid-download.
 *
 * Buffer is PSRAM (mono, DSP_SAMPLE_RATE_HZ): ~96 KB per second. */
bool     rx_audio_cap_start(int seconds);   /* false = out of PSRAM */
void     rx_audio_cap_stop(void);
/* Samples captured, capacity, and whether it is still running. */
void     rx_audio_cap_status(uint32_t *n, uint32_t *cap, bool *running);
/* The captured samples (mono int16) - valid while not running. */
const int16_t *rx_audio_cap_data(uint32_t *n);
/* Sample index of every gap in the window, so an artefact in the waveform can
   be matched to one - or shown NOT to coincide, which is just as informative. */
const uint32_t *rx_audio_cap_gaps(uint32_t *n);
