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

/* ⛔ TRUE WHEN THE SWITCH IS ON AND IS DOING NOTHING.
 *
 * The ES8388/I2S codec is opened once at boot, and only when the saved enable
 * flag was already set - it has to claim its DMA RAM before the USB host takes
 * that pool, so it cannot be opened later in the session. Turning RX audio on
 * afterwards therefore sets the flag, logs "restart required", and produces no
 * sound at all until the next boot.
 *
 * Samuel W7STF, 2026-10-02: booted with audio off, used the radio for 15
 * minutes, turned audio on and heard nothing - "I had to reboot the Tab5 and
 * QMX/+ for audio to work. This is not the way it's supposed to be switched
 * on/off, is it?" The behaviour was right and the UI was silent about it,
 * which is the part that was wrong. Ask this after enabling and say so. */
bool rx_audio_restart_pending(void);

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

// Operator-facing units (ms) for agc_attack/agc_release above - converts via
// tau ~= 1/(alpha*fs_dec) and calls through to the coefficient setters.
void rx_audio_set_agc_attack_ms(uint8_t ms);    // 1..50   (default 3)
void rx_audio_set_agc_release_ms(uint16_t ms);  // 10..500 (default 150)
/* AGC bypass. With it set, the envelope is still tracked (the squelch needs
 * it) but the gain is pinned at the ceiling set by rx_audio_set_agc_gain_max()
 * - so the AGC Ceiling control becomes a manual gain. See the branch in the
 * sample loop for why a "very slow AGC" is not the same thing. */
void rx_audio_set_agc_off(bool off);

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

/* ⛔ THE AM AUDIO BANDWIDTH, AND IT IS THIS FILE'S TO STATE.
 *
 * AM is demodulated on the Tab5 from the raw I/Q (the QMX's own AM filter,
 * fixed at 3.2 kHz, is in the radio's AUDIO and not in the I/Q - CAT manual
 * Q9: "the raw I & Q samples from the ADC are streamed directly to the USB
 * Sound card"). So the passband an operator is actually listening through is
 * set HERE, by the decimation in rx_audio.c, and not by the radio.
 *
 * 4800 Hz, measured in test/am_demod_harness.c. AM runs at its own decimation
 * factor (RX_DECIM_D_AM = 4, a 12 kHz internal rate and a 5400 Hz stage-1
 * cutoff), and against that chain the skirt is -0.42 dB at 4500, -1.33 at
 * 4800, -6.05 at 5400 and -17.04 at 6000.
 *
 * ⛔ BUT THE DEFAULT IS 3000, NOT THE CHAIN'S 4800, AND THE REASON IS THE
 * BAND AND NOT THE DSP. Shortwave broadcast channels are spaced 5 kHz, so the
 * NEIGHBOURING station's carrier sits 5 kHz from the one being listened to. A
 * +/-4800 filter puts its edge essentially on that neighbour and would hand
 * the operator the adjacent channel along with 4.5 kHz of fidelity - a bad
 * trade on a crowded band, and the reason every receiver with an AM mode
 * offers a CHOICE of width rather than its widest.
 *
 * 3000 (6 kHz RF) is the classic "wide" communications AM filter: it leaves
 * 2 kHz of margin to the adjacent carrier and still carries far more audio
 * than the 1600 that started this. The operator, on air 2026-10-15 on a very
 * strong Chinese broadcaster at 7.410 MHz: "listening now 3.2 kHz but wf
 * indicates signal is approx 5 kHz".
 *
 * ⚠ A strong signal LOOKS wider on a waterfall than it is - the visible
 * width is a function of the display's dynamic range - so that 5 kHz is not a
 * measurement of the transmitted bandwidth. The 5 kHz channel SPACING is the
 * solid argument, not the trace width.
 *
 * ▶ The chain is now capable of 4800 (measured -1.33 dB there, -0.42 dB at a
 * broadcaster's 4500), so a selectable AM width is a small addition on top of
 * this - it needs a setting, a drawer row and a web control, not more DSP.
 *
 * ⚠ IT WAS 2400 FOR ONE COMMIT, when AM shared CW's D=8 (that chain is
 * -3.06 dB at 2400 and -6.00 at 2700). That was half a broadcast station, and
 * the operator heard it as "like you had your ear in a cup".
 *
 * ⚠ ui.c's compute_passband_edges_hz() DRAWS this, so it is exported rather
 * than duplicated: the tint and the filter must be the same number or the
 * display describes a passband nobody is listening through. It was a literal
 * in both files for one commit and that is exactly how they drift.
 *
 * ✅ This now covers what a broadcast station actually transmits. The two
 * earlier values are the history worth keeping: +/-1600 (reading the radio's
 * reported filter as a width) and +/-2400 (the chain's limit while AM shared
 * CW's decimation). */
#define RX_AUDIO_AM_HALF_BW_HZ  3000
/* What the chain can carry if a selectable width is ever added - measured, not
 * assumed (test/am_demod_harness.c). Nothing uses it yet; it is here so the
 * ceiling and the default are not the same number by accident again. */
#define RX_AUDIO_AM_HALF_BW_MAX_HZ  4800

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
