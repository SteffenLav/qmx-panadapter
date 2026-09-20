#include "rx_audio.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8388_codec.h"
#include "driver/i2s_std.h"
#include "dsps_fir.h"

#include "dsp.h"          // DSP_FFT_SIZE, DSP_SAMPLE_RATE_HZ, DSP_FFT_TASK_PRIORITY, dsp_rxaudio_*
#include "cat.h"          // cat_get_mode_str(), cat_get_cw_offset_hz()
#include "settings.h"
#include "ui.h"           // ui_get_passband_width_hz() - the QMX's actual selected filter width
#include "net/net_quiet.h" // hold this while RX audio is on - see net_quiet.h's 2026-09-20 note

static const char *TAG = "rx_audio";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---- Scheduling: the whole point of this rewrite ------------------------
// The original cw_audio_task ran at priority 6 (ABOVE fft_task's 4) and woke
// every 120 ms even when fully idle, preempting fft_task - the audio ring's
// sole consumer for both the panadapter spectrum and every FT8/FT4 capture -
// roughly 125 times per 15 s FT8 slot, for the entire session. That let ring
// backlog grow slot over slot, so later captures decoded time-shifted audio:
// still found sync (jitter-tolerant) but failed LDPC decode (needs exact
// symbol alignment). Root-caused 2026-06-25/26 (see CLAUDE.md, "CW audio
// (shelved)"). This task is pinned BELOW fft_task instead, so FreeRTOS's
// preemptive scheduler cannot hand it the CPU while fft_task is ready - not
// "usually doesn't," structurally cannot. See rx_audio.h for the rest of the
// design (blocked-not-polling while disabled, mode-aware filtering).
#define RX_AUDIO_TASK_PRIORITY (DSP_FFT_TASK_PRIORITY - 1)
_Static_assert(RX_AUDIO_TASK_PRIORITY < DSP_FFT_TASK_PRIORITY,
               "rx_audio_task must never be able to preempt fft_task");

// ---- Demod mode -----------------------------------------------------------
typedef enum {
    RXAUD_MODE_NONE = 0,   // unsupported CAT mode - path stays idle
    RXAUD_MODE_CW,
    RXAUD_MODE_SSB,
} rxaud_mode_t;

static rxaud_mode_t mode_from_cat_str(const char *m)
{
    if (!m) return RXAUD_MODE_NONE;
    if (strcmp(m, "CW") == 0 || strcmp(m, "CW-R") == 0) return RXAUD_MODE_CW;
    if (strcmp(m, "USB") == 0 || strcmp(m, "LSB") == 0) return RXAUD_MODE_SSB;
    return RXAUD_MODE_NONE;
}

// ---- Filter design tunables -----------------------------------------------
// History: FIR_LEN went 63 -> 1023 (too slow, no audio at all) -> 255 (audio
// back, but transition band ~622 Hz - still leaking content 200-300 Hz off
// the dial into a nominal 150 Hz CW filter, confirmed on the air 2026-09-04).
//
// Then a SECOND bug, found the same day once audio was flowing again and
// still measured wider than the set 150 Hz: the "decimate to get narrower
// selectivity for the same tap count" idea was right, but it was implemented
// as ONE dsps_fird_f32 call whose coefficients were designed against the
// DECIMATED rate (half_bw_hz / (fs/D)). dsps_fird_f32 is a single-stage
// polyphase decimator - it filters the FULL-RATE input directly (just
// computing only every Dth output), so per its own header and this
// codebase's own zoom-FFT precedent (dsp.c's zoom_design_lpf, called with
// cutoff_norm = 0.45/D - a fraction of the FULL rate, not the decimated one)
// its coefficients must be normalised against the INPUT rate. Designing them
// against fs/D instead made the numeric cutoff fraction D x too large, so
// the realised cutoff was D x wider than intended (150 Hz requested -> ~1200
// Hz actual at D=8) - exactly "much wider than the set 150Hz", on the air.
//
// A single fused decimating stage genuinely CANNOT give ~75 Hz half-bandwidth
// selectivity for an affordable tap count at the full 48 kHz rate - that's
// the same wall FIR_LEN=1023 hit (transition ~155 Hz there, and already too
// slow). The actual win from decimating only appears with TWO stages:
//   Stage 1 (dsps_fird_f32, FIR_DECIM_LEN taps, fixed, mode-independent):
//     a coarse anti-alias/decimate filter, cutoff_norm = 0.45/RX_DECIM_D
//     against the FULL rate - the same convention and tap count as the
//     zoom-FFT's own ZOOMFIR (measured ~1.0 ms/window there for 63 taps).
//   Stage 2 (dsps_fir_f32, NON-decimating, FIR_LEN taps, rebuilt per mode):
//     runs on the now-decimated fs/RX_DECIM_D stream, so a cutoff genuinely
//     computed against THAT rate (half_bw_hz/(fs/D)) is correct here, and
//     the same FIR_LEN=255 taps gives transition ~78 Hz (3.3*6000/255) at a
//     fraction of the cost 255 taps would have at the full rate, because it
//     only ever sees n_out = pairs/RX_DECIM_D samples per frame instead of
//     pairs.
// D=8 -> 6 kHz internal rate, Nyquist 3 kHz, comfortably above SSB's widest
// half-bandwidth (~1500 Hz) so one D covers both modes.
//
// The output of stage 2 is only DSP_FFT_SIZE/RX_DECIM_D samples/frame; NCO2
// and the AGC/output stage now run at that rate too, and the result is
// upsampled back to 48 kHz by simple sample-and-hold (repeat each sample
// RX_DECIM_D times) before the codec write - crude, but the audio content
// here (a CW/SSB tone under ~3 kHz) is far below where a zero-order hold's
// imaging artifacts would matter for this purpose. Revisit with linear
// interpolation if that proves audible.
#define FIR_LEN        255       // stage-2 narrow lowpass taps (odd, linear phase) - shared by both modes
#define FIR_DECIM_LEN  63        // stage-1 decimator taps (fixed, coarse - same as dsp.c's ZOOMFIR)
#define RX_DECIM_D     8         // internal rate = DSP_SAMPLE_RATE_HZ / RX_DECIM_D
#define CW_DEF_OFFSET  700       // fallback CW offset if CAT hasn't reported one
// Mode-default passband widths, used only when ui_get_passband_width_hz()
// reads 0 (CAT hasn't reported one yet). Mirrors compute_passband_edges_hz()'s
// own per-mode defaults in ui.c - keep them in step if either changes.
#define CW_DEF_WIDTH_HZ   300
#define SSB_DEF_WIDTH_HZ  2700
#define SSB_LOW_HZ        200    // matches ui.c's PB_SSB_LOW_HZ (not exported)

// Runtime-adjustable (see rx_audio_set_tuning() / the /api/cmd "rxaudio"
// action) instead of #define constants - 2026-09-04, after several
// build/flash/QMX-power-cycle rounds to chase "clicking on stronger signals"
// blind. Defaults below are the starting point; live values live in the
// _t struct so a whole session of tuning survives without a reflash.
// out_clamp/agc_target/agc_gain_max raised 2026-09-04 - the original figures
// (20000/10000/120) left the output audibly quiet even at codec volume 99;
// confirmed live (30000/32000... see below/200) loud enough on the air, so
// that is now the shipped default instead of something pushed by hand over
// /api/cmd after every reflash.
//
// ⚠ attack/release ARE 8x too slow relative to the AGC loop's real rate
// (it now runs on the DECIMATED stream, 6 kHz, since the two-stage filter
// refactor - these coefficients were never rescaled for that). That analysis
// still stands - see git history 2026-09-04 for the full writeup - but the
// rescaled values were flashed and tested and made NO confirmed difference
// to the reported on-air artifact, so they are reverted here along with two
// other unconfirmed changes from the same session (silence-gap fade, doubled
// DMA buffer) to get back to the last build that was known-good: the
// two-stage filter fix + linear-interp upsample, both independently
// confirmed (offline numbers + on-air "the build is narrower"). Revisit the
// AGC rate fix on its own, separately tested, if it turns out to matter.
#define DEF_OUT_CLAMP      32000.0f  // hard clip before int16 cast (headroom)
#define DEF_AGC_TARGET     30000.0f
#define DEF_AGC_ATTACK     0.007f    // ~3 ms attack (pre-existing figure, not rescaled)
#define DEF_AGC_RELEASE    0.00014f  // ~150 ms release (pre-existing figure, not rescaled)
#define DEF_AGC_GAIN_MAX   200.0f    // allow weak signals up
#define AGC_NOISE_TC   0.00010f  // noise-floor tracker (diag/squelch)

static volatile float s_out_clamp    = DEF_OUT_CLAMP;
static volatile float s_agc_target   = DEF_AGC_TARGET;
static volatile float s_agc_attack   = DEF_AGC_ATTACK;
static volatile float s_agc_release  = DEF_AGC_RELEASE;
static volatile float s_agc_gain_max = DEF_AGC_GAIN_MAX;

// How many output samples hit s_out_clamp since the last read - an objective
// answer to "how much is it actually clicking", instead of judging by ear.
// Read + zeroed together by rx_audio_get_clip_count() so each reading is a
// rate since the previous call, not a lifetime total.
static volatile uint32_t s_clip_count = 0;

// Real diagnostics, added 2026-09-04 after three rounds of guessing at the
// audio problem from theory alone with the operator unable to hear any
// change from any of it. Settles two questions that were only ever
// speculated about: is the per-frame DSP (NCO + 2x FIR + AGC) actually
// keeping up with the 21.3 ms/frame real-time budget, and is the forward
// ring (dsp.c) actually delivering fresh audio or mostly timing out (which
// would explain "no real signal, just intermittent clicks" regardless of any
// AGC/clamp setting - silence has nothing for those to act on).
static volatile uint32_t s_frame_us_max = 0;    // worst single-frame DSP time this window
static volatile uint64_t s_frame_us_sum = 0;    // for an average - divide by s_frame_count
static volatile uint32_t s_frame_count  = 0;    // frames processed since last read
static volatile uint32_t s_read_timeout_count = 0;  // dsp_rxaudio_read() returned <=0

/* ---- CHIRP CHARACTERISATION (2026-09-06) --------------------------------
 *
 * The operator hears a chirp every 4-5 s ON A SILENT BAND WITH NO SIGNAL, so
 * it is not signal-dependent - and each read timeout above already plays a
 * faded frame of silence, i.e. a gap. read to=1656 over 3360 s is one gap
 * every ~2 s, the same order as what he hears.
 *
 * What we cannot yet say is WHY the ring runs dry on a cadence, and the two
 * candidates need different fixes:
 *
 *   RATE MISMATCH  - the QMX's audio clock is not bit-exact 48 kHz (CLAUDE.md
 *                    records the FT8 capture needing a UTC boundary for this
 *                    very reason). If we consume faster than it produces, the
 *                    ring drains steadily and underruns at a NEAR-CONSTANT
 *                    interval. More buffering only makes it rarer, never
 *                    fixes it; the fix is rate adaptation.
 *   CONTENTION     - something periodic starves the producer. Then the
 *                    intervals are IRREGULAR and cluster around that event.
 *
 * The interval between gaps discriminates them, so that is what this records:
 * the spread of the last intervals, and the ring level at the moment of the
 * gap. A tight spread means rate; a wide one means contention. */
/* ---- RECORDER (2026-09-06) ----------------------------------------------
 *
 * ⭐ OBSERVE THE PHENOMENON BEFORE EXPLAINING IT. Counting events says WHEN
 * something happened and never WHAT IT SOUNDED LIKE - and "chirp", "stutter"
 * and "click" are different artefacts with different causes. A frequency sweep,
 * a step discontinuity, a burst of noise and a hole in the audio all sound
 * wrong and look nothing like each other.
 *
 * So this keeps the EXACT samples handed to the codec, in PSRAM (which has
 * ~15 MB spare), and serves them as a WAV. Mono: the two channels are written
 * identical a few lines below, so a second copy would only double the size.
 *
 * Alongside it, the sample index of every gap - so an artefact seen in the
 * waveform can be matched against the event that produced it, or shown NOT to
 * coincide with one, which would be just as informative.
 *
 * One-shot on purpose: it records until full and stops, so the window is
 * contiguous and cannot be overwritten while it is being downloaded. */
#define RXCAP_MAX_GAPS 512
static int16_t          *s_cap        = NULL;   /* PSRAM, mono, DSP_SAMPLE_RATE_HZ */
static volatile uint32_t s_cap_cap    = 0;      /* capacity in samples */
static volatile uint32_t s_cap_n      = 0;      /* samples written */
static volatile bool     s_cap_run    = false;
static uint32_t          s_cap_gap[RXCAP_MAX_GAPS];  /* sample index of each gap */

/* ⛔ WHY THIS DUMPS OVER SERIAL AND NOT OVER WiFi (2026-09-06).
 *
 * The recorder was reachable only through /api/cmd + /api/rxaudio.wav, and
 * on this track WiFi wedges constantly - the esp_hosted/SDIO link dies and
 * STAYS dead until a REBOOT. That makes a WiFi fetch of a WiFi-off recording
 * impossible IN PRINCIPLE, not merely awkward: the only way to get the link
 * back is the one action that also clears this PSRAM buffer. I asked the
 * operator to toggle WiFi off and back on to work around it, which could
 * never have worked, and he had already said twice that it was wedged.
 *
 * The serial capture has none of that: it needs no network, it is already
 * running for every bench session, and it survives the reboot. So the
 * capture can also ARM ITSELF a fixed time after boot and BASE64 ITSELF to
 * the log when full - no host round-trip anywhere in the loop.
 *
 * Set RXCAP_AUTO_SECONDS to 0 to disable. Decode with:
 *   python tools/rxcap_decode.py scratchpad/capture-dev.txt out.wav          */
/* 0 = OFF, and off is the shipping value. Set it to a number of seconds to
 * arm the capture automatically once per boot - it then base64s itself to the
 * serial log, which is the only route that works when WiFi is wedged. Left
 * off because the dump takes ~70 s and stops nothing else from running, but
 * it is noise on every boot when nobody is measuring. */
#define RXCAP_AUTO_SECONDS   0     /* 0 = off; one shot, armed once per boot */
#define RXCAP_AUTO_DELAY_MS  90000 /* after audio starts - time to power-cycle
                                      the QMX and let the band settle */
static bool     s_cap_auto_done = false;   /* armed once per boot */
static bool     s_cap_autodump  = false;   /* dump to serial when full */
static int64_t  s_cap_first_us  = 0;       /* first audio frame, for the delay */
static volatile bool s_cap_dumping = false;  /* a dump task is running */
static void rxcap_auto_tick(void);   /* defined by the recorder block below */
static volatile uint32_t s_cap_gap_n  = 0;

static volatile uint32_t s_gap_prev_us   = 0;   /* uptime of the previous gap */
static volatile uint32_t s_gap_iv_min_ms = 0xFFFFFFFF;
static volatile uint32_t s_gap_iv_max_ms = 0;
static volatile uint32_t s_gap_iv_sum_ms = 0;
static volatile uint32_t s_gap_iv_n      = 0;
// Round 2 (2026-09-04): the recursive-phasor NCO fix cut frame_us_avg but the
// operator heard NO change at all - so the bottleneck is somewhere frame_us
// does not cover. It only spans read-success to output-ready; it excludes
// BOTH dsp_rxaudio_read()'s own wait and the blocking I2S write. Timing both
// separately settles which one actually owns the missing time.
static volatile uint32_t s_read_us_max = 0, s_write_us_max = 0;
static volatile uint64_t s_read_us_sum = 0, s_write_us_sum = 0;
static volatile uint32_t s_loop_count = 0;   // every iteration, success or timeout - denominator for both sums above
// Squelch DISABLED for now (floor = 1.0 => always fully open) - carried over
// unchanged from cw_audio.c. The previous noise-floor math settled at the
// signal average so SNR never exceeded 1 and it muted everything. Get clean
// audible AGC audio first, revisit squelch.
#define SQ_LO          1.5f
#define SQ_HI          2.8f
#define SQ_FLOOR       1.0f

// ---- Module state ----------------------------------------------------------
static volatile bool s_enabled = false;
static volatile uint8_t s_volume = 60;

static esp_codec_dev_handle_t s_codec = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;   // TX-only I2S channel (no RX/mic)
static volatile bool s_codec_ready = false;  // codec opened (at boot, pre-USB-host)
static TaskHandle_t s_task = NULL;

#ifndef CONFIG_BSP_I2S_NUM
#define CONFIG_BSP_I2S_NUM 1
#endif

// DSP work buffers (PSRAM - accessed once per ~21 ms frame, internal DRAM is
// already crowded by USB host / LVGL / FFT).
static int16_t *s_rxbuf   = NULL;   // [DSP_FFT_SIZE*2] raw I/Q pairs from the ring
static float   *s_mix_re  = NULL;   // [DSP_FFT_SIZE] complex baseband after NCO1, real part, full rate
static float   *s_mix_im  = NULL;   // [DSP_FFT_SIZE] complex baseband after NCO1, imag part, full rate
static float   *s_filt_re = NULL;   // [DSP_FFT_SIZE/RX_DECIM_D] stage-1 (coarse decimate) output, real part
static float   *s_filt_im = NULL;   // [DSP_FFT_SIZE/RX_DECIM_D] stage-1 (coarse decimate) output, imag part
static float   *s_narrow_re = NULL; // [DSP_FFT_SIZE/RX_DECIM_D] stage-2 (narrow, mode-width) output, real part
static float   *s_narrow_im = NULL; // [DSP_FFT_SIZE/RX_DECIM_D] stage-2 (narrow, mode-width) output, imag part
static int16_t *s_out     = NULL;   // [DSP_FFT_SIZE*2] interleaved L/R for codec, full rate (sample-and-hold upsampled)
// Stage 1: fixed coarse decimator, built once at init, never rebuilt.
static float   *s_dec_coeff    = NULL;  // [FIR_DECIM_LEN]
static float   *s_dec_delay_re = NULL;  // [FIR_DECIM_LEN] (dsps_fird convention: exactly N, not N+4)
static float   *s_dec_delay_im = NULL;  // [FIR_DECIM_LEN]
static fir_f32_t s_dec_fir_re;
static fir_f32_t s_dec_fir_im;
// Stage 2: narrow lowpass at the DECIMATED rate, rebuilt whenever the mode's
// half-bandwidth changes (build_lpf()).
static float   *s_coeff   = NULL;   // [FIR_LEN] - one lowpass, shared by both I/Q legs
static float   *s_delay_re = NULL;  // [FIR_LEN+4] - independent delay line, real leg (dsps_fir, non-decimating convention: N+4)
static float   *s_delay_im = NULL;  // [FIR_LEN+4] - independent delay line, imag leg
static fir_f32_t s_fir_re;
static fir_f32_t s_fir_im;

// NCO1 shifts the wanted signal down to complex baseband 0 (removes the QMX's
// +12 kHz IF AND the mode's own center/offset in one step); NCO2 shifts the
// filtered result back up so it is audible at the same pitch the QMX's own
// sidetone would use.
//
// ⛔ Originally called sinf/cosf FRESH EVERY SAMPLE ("cheap on this core" -
// it was not). Measured 2026-09-04 via rx_audio_take_diag(): average frame
// time sat right at the 21.3 ms/frame budget with spikes to 33 ms, and the
// task was completing only ~half the expected frames/second - exactly
// "intermittent clicking, no coherent signal", because the forward ring
// backlogs and the producer starts dropping stale audio once the consumer
// falls behind. 4096 transcendental calls/frame (2 NCOs x 2 trig calls x
// 1024 samples) is not free on this core without hardware trig.
//
// Fixed with a recursive phasor: (re,im) is a unit vector representing the
// CURRENT phase; each sample rotates it by a fixed per-sample step
// (step_re,step_im), computed via sinf/cosf ONCE per retune() instead of
// per sample - 4 multiplies + 2 adds replaces 2 transcendental calls.
// Renormalised periodically (rotation is not perfectly unitary in floating
// point, so magnitude drifts very slowly without it) rather than per sample.
typedef struct { float re, im, step_re, step_im; } nco_t;
static nco_t s_nco1 = {1.0f, 0.0f, 1.0f, 0.0f};
static nco_t s_nco2 = {1.0f, 0.0f, 1.0f, 0.0f};
#define NCO_RENORM_EVERY 64   // samples between renormalisations

static float s_agc_env = 1.0f;
static float s_noise   = 1.0f;     // slow noise-floor estimate (for squelch)
// Linear-interpolation upsample state - the value the last frame's ramp
// ended on, carried forward so this frame's ramp starts from where the last
// one left off instead of jumping. See the upsample comment below for why
// plain sample-and-hold was replaced.
static float s_last_up_v = 0.0f;
// Counts DOWN the samples remaining in the post-gap ramp-in - the mirror
// of the ~10 ms fade-down in the read-timeout branch. See the long note
// there for the measurement that showed the resume, not the entry, was
// the audible half.
static int s_resume_ramp = 0;
static int   s_center_hz  = 0;     // center the current NCO steps + lowpass are built for
static int   s_half_bw_hz = 0;     // half-bandwidth the current lowpass is built for

// ---- Post-upsample smoothing (2026-09-04) ---------------------------------
// Linear interpolation cut the upsample image ~18.5 dB (verified offline:
// scratchpad/rx_audio_dsp_check.py) but didn't eliminate it - still audible
// as a whine on the air. A 4th-order (two cascaded biquad stages) Butterworth
// lowpass run once per OUTPUT sample at the full 48 kHz rate adds another
// ~19.8 dB of suppression for CW (content passthrough -0.01 dB, i.e. free)
// and a smaller but real ~10.8 dB for SSB (its content-to-image gap is only
// ~1000 Hz vs CW's ~4750 Hz, so that's the honest physical limit, not a
// tuning miss - same script, both cases checked before flashing this).
// Fixed at 3200 Hz - just above SSB's ~2700 Hz widest passband, comfortably
// below CW's much narrower content - never rebuilt per mode, unlike the
// stage-2 filter. A biquad is a RECURSIVE (IIR) filter - 5 multiply-adds per
// sample per stage, negligible next to the FIR stages above; the cost is in
// the coefficients, not the tap count, which is why this is affordable where
// another decimating FIR pass would not have been.
#define SMOOTH_FC_HZ 3200.0f
typedef struct { float b0, b1, b2, a1, a2; float x1, x2, y1, y2; } biquad_t;
static biquad_t s_smooth_a, s_smooth_b;  // two cascaded stages = 4th order

// ---- Low-pass FIR design (windowed sinc) ------------------------------------
static inline float sinc_norm(float x)  // sin(pi x)/(pi x)
{
    if (fabsf(x) < 1e-6f) return 1.0f;
    float px = (float)M_PI * x;
    return sinf(px) / px;
}

// ⛔ WHY A LOW-PASS, NOT A BAND-PASS CENTERED ON THE OFFSET (2026-09-04):
// the original cw_audio.c design mixed the wanted signal down to +offset Hz,
// took the REAL PART of that complex mix (discarding the imaginary part), and
// only THEN band-pass filtered - on the theory that "the -offset mirror is
// rejected by the narrow band-pass". It is not, and cannot be: a real-valued
// signal is inherently symmetric around 0 Hz (that is what "real" means
// spectrally), so by the time the real part was extracted, the true +offset
// content and whatever ELSE was sitting at -offset (an unrelated part of the
// band - which is why the field report described it as sounding "like digi"
// near an FT8 sub-band) were already folded on top of each other. No filter
// applied afterward, however narrow, can un-fold them - the phase information
// needed to tell them apart was already discarded. Reported on the air
// 2026-09-04: rough/broadband, not a clean tone, pitch barely moved while
// tuning - exactly what a folded image sounds like.
//
// The fix keeps BOTH real and imaginary parts through the whole chain: shift
// the wanted signal all the way down to complex 0 (NCO1, below), low-pass
// filter that - a real-coefficient low-pass applied identically to both I and
// Q of a complex signal is unambiguous exactly AT 0, because that is where a
// low-pass's symmetric passband is actually centered on the thing we want -
// then shift the filtered result back UP to the audible pitch (NCO2) and only
// THEN take the real part for playback. This is the standard SSB/CW "phasing
// method" replayed as shift-filter-shift-back instead of a Hilbert pair,
// which needs only two real low-pass filters instead of a 90-degree
// all-pass network. CW and SSB share this unchanged - SSB had the identical
// flaw, just less obviously audible under normal voice-bandwidth listening.
static void build_lpf(int half_bw_hz)
{
    if (half_bw_hz < 20) half_bw_hz = 20;
    // Stage 2 only - designed for the DECIMATED rate (fs/RX_DECIM_D), which
    // is correct HERE because this filter genuinely runs on the already-
    // decimated stream (see the FIR_LEN comment above for why the earlier
    // single-stage version normalising against this same rate was wrong -
    // that stage ran on the FULL-rate input via a fused decimating call).
    float fs = (float)DSP_SAMPLE_RATE_HZ / (float)RX_DECIM_D;
    float fc = (float)half_bw_hz / fs;
    int   M  = FIR_LEN - 1;
    float half = M / 2.0f;

    for (int n = 0; n < FIR_LEN; n++) {
        float m  = (float)n - half;
        float lp = 2.0f * fc * sinc_norm(2.0f * fc * m);
        float w  = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)n / (float)M); // Hamming
        s_coeff[n] = lp * w;
    }

    // Normalise to unity gain at DC (a low-pass's own passband center).
    float sum = 0.0f;
    for (int n = 0; n < FIR_LEN; n++) sum += s_coeff[n];
    if (fabsf(sum) > 1e-6f) {
        for (int n = 0; n < FIR_LEN; n++) s_coeff[n] /= sum;
    }

    // Stage 2 is NON-decimating (n_out samples in, n_out out - the decimating
    // already happened in stage 1) - dsps_fir_init_f32, not dsps_fird_*, and
    // per that convention (see reference_dsps_fir_delay_n_plus_4.md) the
    // delay buffer must be FIR_LEN+4 floats, not exactly FIR_LEN like the
    // decimating stage-1 filter below.
    memset(s_delay_re, 0, (FIR_LEN + 4) * sizeof(float));
    memset(s_delay_im, 0, (FIR_LEN + 4) * sizeof(float));
    dsps_fir_init_f32(&s_fir_re, s_coeff, s_delay_re, FIR_LEN);
    dsps_fir_init_f32(&s_fir_im, s_coeff, s_delay_im, FIR_LEN);
    s_half_bw_hz = half_bw_hz;
}

// Stage 1: fixed, coarse anti-alias/decimate filter, built exactly ONCE
// (called from rx_audio_init(), never rebuilt on mode/width change - it has
// no mode-dependent parameter). dsps_fird_f32 IS a single-stage decimator,
// so per its own header its coefficients must be normalised against the
// FULL input rate - the same convention this codebase's own zoom-FFT already
// uses (dsp.c's zoom_design_lpf, cutoff_norm = 0.45/D against the full
// rate). A generous cutoff just inside the decimated Nyquist is all this
// stage needs to do; stage 2 (build_lpf, above) does the real selectivity
// work at the now-lower rate.
static void build_decimator(void)
{
    float cutoff_norm = 0.45f / (float)RX_DECIM_D;   // fraction of the FULL rate
    int   M    = FIR_DECIM_LEN - 1;
    float half = M / 2.0f;

    for (int n = 0; n < FIR_DECIM_LEN; n++) {
        float m  = (float)n - half;
        float lp = 2.0f * cutoff_norm * sinc_norm(2.0f * cutoff_norm * m);
        float w  = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)n / (float)M); // Hamming
        s_dec_coeff[n] = lp * w;
    }
    float sum = 0.0f;
    for (int n = 0; n < FIR_DECIM_LEN; n++) sum += s_dec_coeff[n];
    if (fabsf(sum) > 1e-6f) {
        for (int n = 0; n < FIR_DECIM_LEN; n++) s_dec_coeff[n] /= sum;
    }

    // Decimating convention (dsps_fird_init_f32): delay buffer exactly
    // FIR_DECIM_LEN floats, not +4 - see build_lpf's comment for the
    // contrast with stage 2's non-decimating convention.
    memset(s_dec_delay_re, 0, FIR_DECIM_LEN * sizeof(float));
    memset(s_dec_delay_im, 0, FIR_DECIM_LEN * sizeof(float));
    dsps_fird_init_f32(&s_dec_fir_re, s_dec_coeff, s_dec_delay_re, FIR_DECIM_LEN, RX_DECIM_D);
    dsps_fird_init_f32(&s_dec_fir_im, s_dec_coeff, s_dec_delay_im, FIR_DECIM_LEN, RX_DECIM_D);
}

// RBJ audio-EQ-cookbook lowpass biquad, Butterworth (Q = 1/sqrt(2)) - the
// standard maximally-flat-passband design, matching
// scratchpad/rx_audio_dsp_check.py's biquad_lpf_coeffs() exactly. Fixed at
// SMOOTH_FC_HZ, built once at init (not mode-dependent, unlike build_lpf).
static void build_smoothing_biquad(void)
{
    float w0    = 2.0f * (float)M_PI * SMOOTH_FC_HZ / (float)DSP_SAMPLE_RATE_HZ;
    float q     = 0.70710678f;
    float alpha = sinf(w0) / (2.0f * q);
    float cosw0 = cosf(w0);
    float b0 = (1.0f - cosw0) / 2.0f;
    float b1 = 1.0f - cosw0;
    float b2 = (1.0f - cosw0) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha;

    biquad_t c = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0, 0, 0 };
    s_smooth_a = c;
    s_smooth_b = c;
}

static inline float biquad_step(biquad_t *bq, float x)
{
    float y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
              - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    bq->x2 = bq->x1; bq->x1 = x;
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

// Two cascaded stages = 4th order, ~19.8 dB more image suppression on CW for
// negligible cost - see the SMOOTH_FC_HZ comment above.
static inline float smooth_step(float x)
{
    return biquad_step(&s_smooth_b, biquad_step(&s_smooth_a, x));
}

// (Re)point both NCOs at a new center frequency and reset their phase - a
// phase jump here is one click on a mode/filter change, same tradeoff the old
// AGC-reset-on-mode-change already made.
static void retune(int center_hz)
{
    float fs = (float)DSP_SAMPLE_RATE_HZ;
    float fs_dec = fs / (float)RX_DECIM_D;   // NCO2 now runs on the decimated stream
    float w1 = 2.0f * (float)M_PI * (float)(12000 + center_hz) / fs;
    float w2 = 2.0f * (float)M_PI * (float)center_hz / fs_dec;
    s_nco1.step_re = cosf(w1); s_nco1.step_im = sinf(w1);
    s_nco2.step_re = cosf(w2); s_nco2.step_im = sinf(w2);
    s_nco1.re = 1.0f; s_nco1.im = 0.0f;
    s_nco2.re = 1.0f; s_nco2.im = 0.0f;
    s_center_hz = center_hz;
}

// Rotate the phasor one sample forward and hand back this sample's (cos,sin).
// Renormalises every NCO_RENORM_EVERY samples - a plain complex multiply is
// not perfectly unitary in floating point, so |re,im| creeps away from 1.0
// very slowly without this; cheap enough to just always compute the norm and
// only apply it periodically rather than branching on a counter per call.
static inline void nco_step(nco_t *n, uint32_t sample_idx, float *c, float *s)
{
    *c = n->re; *s = n->im;
    float nre = n->re * n->step_re - n->im * n->step_im;
    float nim = n->re * n->step_im + n->im * n->step_re;
    n->re = nre; n->im = nim;
    if ((sample_idx % NCO_RENORM_EVERY) == 0) {
        float mag2 = n->re * n->re + n->im * n->im;
        // mag2 stays extremely close to 1 - a cheap 1st-order correction
        // (1.5 - 0.5*mag2 ~= 1/sqrt(mag2)) beats a real sqrtf/division here.
        float k = 1.5f - 0.5f * mag2;
        n->re *= k; n->im *= k;
    }
}

// What center/half-bandwidth the given mode wants right now, tracking the
// QMX's own selected filter width via ui_get_passband_width_hz() - the same
// source the spectrum's passband tint reads (compute_passband_edges_hz() in
// ui.c). Found 2026-09-04: the first cut used fixed constants here regardless
// of what the operator selected on the radio (CW's 50-500 Hz choices, SSB's
// 2500-3200 Hz choices) - a 500 Hz-wide CW listen while the QMX itself was
// set to 150 Hz let nearby FT8 tones bleed straight through, which is what
// "still sounds like digi" on a narrow CW filter was.
static void filter_params_for_mode(rxaud_mode_t mode, int *center_hz, int *half_bw_hz)
{
    uint32_t w = ui_get_passband_width_hz();   // 0 = CAT hasn't reported one yet
    if (mode == RXAUD_MODE_CW) {
        int off = cat_get_cw_offset_hz();
        if (off < 100 || off > 5000) off = CW_DEF_OFFSET;
        if (w == 0) w = CW_DEF_WIDTH_HZ;
        *center_hz = off;
        *half_bw_hz = (int)w / 2;
    } else {   // RXAUD_MODE_SSB
        if (w == 0) w = SSB_DEF_WIDTH_HZ;
        int low  = SSB_LOW_HZ;
        int high = SSB_LOW_HZ + (int)w;
        *center_hz = (low + high) / 2;
        *half_bw_hz = (high - low) / 2;
    }
}

// ---- Demodulation task ------------------------------------------------------
/* Append the frame we are about to play. LEFT channel only - the two are
   written identical, so mono halves the size and loses nothing. Silently stops
   when full; the download is what reports how much was captured. */
static inline void rxcap_push(const int16_t *out, int pairs)
{
    if (!s_cap_run || !s_cap) return;
    uint32_t n = s_cap_n;
    if (n >= s_cap_cap) { s_cap_run = false; return; }
    uint32_t room = s_cap_cap - n;
    uint32_t take = ((uint32_t)pairs < room) ? (uint32_t)pairs : room;
    for (uint32_t i = 0; i < take; i++) s_cap[n + i] = out[2 * i];
    s_cap_n = n + take;
    if (s_cap_n >= s_cap_cap) s_cap_run = false;   /* one-shot */
}

static void rx_audio_task(void *arg)
{
    (void)arg;
    bool active_prev = false;
    rxaud_mode_t mode_prev = RXAUD_MODE_NONE;
    // Set on the first real read after going active, cleared on every
    // false->true transition. While false, every timeout retries
    // dsp_rxaudio_forward_enable(true) - see the timeout branch below for
    // why: the ring is created lazily on FIRST activation and, found
    // 2026-09-04, a failed allocation (internal-RAM pressure - the same
    // class that hit BLE and the web server earlier the same session) was
    // never retried, so one bad moment at boot silenced RX audio for the
    // entire session with no way to recover short of a reboot.
    bool ever_got_data = false;
    int64_t s_last_ring_retry_us = 0;

    while (1) {
        if (!s_enabled) {
            if (active_prev) { dsp_rxaudio_forward_enable(false); active_prev = false; }
            // Fully asleep - zero wakeups, zero cost - until
            // rx_audio_set_enabled(true) gives this notification. This is the
            // default state on every unit that never turns RX audio on.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        rxaud_mode_t mode = s_codec_ready ? mode_from_cat_str(cat_get_mode_str())
                                           : RXAUD_MODE_NONE;
        bool active = (mode != RXAUD_MODE_NONE);

        if (!active) {
            if (active_prev) { dsp_rxaudio_forward_enable(false); active_prev = false; }
            // Enabled by the user but not currently in a supported mode (or
            // the codec never opened). Safe to poll here even though it is
            // not a full block: RX_AUDIO_TASK_PRIORITY sits strictly below
            // fft_task's, so FreeRTOS cannot schedule this task while
            // fft_task is ready - this can only ever spend core-1 idle time,
            // never fft_task's, no matter how often it wakes.
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!active_prev) {
            s_agc_env = 1.0f;
            s_noise   = 1.0f;
            s_last_up_v = 0.0f;
            ever_got_data = false;
            dsp_rxaudio_forward_enable(true);
            active_prev = true;
            ESP_LOGI(TAG, "RX audio on (mode=%s vol=%d)",
                     mode == RXAUD_MODE_CW ? "CW" : "SSB", (int)s_volume);
        }

        // Track the live filter target (CW offset moves; SSB is fixed but the
        // mode itself can change) and retune/rebuild if it moved.
        int want_center, want_half_bw;
        filter_params_for_mode(mode, &want_center, &want_half_bw);
        if (mode != mode_prev || want_center != s_center_hz) {
            retune(want_center);
            mode_prev = mode;
        }
        if (want_half_bw != s_half_bw_hz) {
            build_lpf(want_half_bw);
        }

        s_loop_count++;
        int64_t read_start_us = esp_timer_get_time();
        int pairs = (int)dsp_rxaudio_read(s_rxbuf, DSP_FFT_SIZE, 60);
        uint32_t read_us = (uint32_t)(esp_timer_get_time() - read_start_us);
        if (read_us > s_read_us_max) s_read_us_max = read_us;
        s_read_us_sum += read_us;
        if (pairs <= 0) {
            // Producer momentarily behind: feed the I2S a frame of silence so
            // the DMA never underruns (an underrun is an audible click). A
            // brief silence is far less objectionable than breaking up.
            s_read_timeout_count++;
            {   /* interval since the previous gap - see the note by the
                   counters: a tight spread means a clock-rate mismatch, a
                   wide one means something is periodically starving us. */
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                if (s_gap_prev_us) {
                    uint32_t iv = now_ms - s_gap_prev_us;
                    if (iv < s_gap_iv_min_ms) s_gap_iv_min_ms = iv;
                    if (iv > s_gap_iv_max_ms) s_gap_iv_max_ms = iv;
                    s_gap_iv_sum_ms += iv;
                    s_gap_iv_n++;
                }
                s_gap_prev_us = now_ms;
            }
            if (s_cap_run && s_cap_gap_n < RXCAP_MAX_GAPS)
                s_cap_gap[s_cap_gap_n++] = s_cap_n;   /* where in the WAV it lands */
            if (!ever_got_data) {
                // The forward ring is created lazily on first activation; if
                // that allocation failed (internal-RAM pressure), every read
                // returns 0 IMMEDIATELY (no ring to block on), so this branch
                // is hit at whatever rate the loop spins - measured ~50/s,
                // not the ~60ms read timeout the "every timeout" comment
                // below used to assume. Retrying the alloc at that rate only
                // adds to the fragmentation it is trying to recover from, so
                // it is throttled to once/second instead - the failure is
                // rare and the retry is not on the recovery's own critical
                // path.
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_last_ring_retry_us >= 1000000) {
                    s_last_ring_retry_us = now_us;
                    dsp_rxaudio_forward_enable(true);
                }
            }
            // A hard memset here is an INSTANT drop to zero, and because the
            // next real frame's linear-interp upsample ramps from
            // s_last_up_v (whatever it was BEFORE this gap, never updated
            // across a silent frame), resuming audio jumps straight back to
            // that stale value too - two hard discontinuities bracketing
            // every gap. Confirmed 2026-09-04 via the periodic serial diag
            // line (read to=N > 0 correlating with an active WiFi/SDIO
            // storm and a reported chirp that scaled with signal strength -
            // a bigger signal makes the same jump more audible, which a
            // fixed-size click could never explain on its own). Re-added
            // after being reverted alongside two OTHER unconfirmed changes
            // in the same session - this is the one piece of that revert
            // with actual evidence behind it. Fade the first ~10 ms of the
            // gap from s_last_up_v down to true silence instead, and leave
            // s_last_up_v at 0 so the frame that resumes real audio ramps up
            // FROM the silence that was actually just played.
            //
            // 2026-09-06, MEASURED: the fade above is only HALF the job, and
            // the missing half is what is actually heard. Recorded 20 s of
            // exactly these bytes (rxcap -> /api/rxaudio.wav) and looked at
            // it: entering the gap is clean (the ramp lands on -1, then 0),
            // but the first sample AFTER the silence jumps straight to a
            // median of 132 (worst 451) - i.e. essentially the pre-gap
            // amplitude (median 140), where the intended 8-sample ramp from
            // s_last_up_v == 0 would give about 18. So the ramp was being
            // swamped, and the resumed audio began with a step
            // discontinuity: a BROADBAND CLICK, +36 dB above the 3-9 kHz
            // floor - a band the CW filter means real audio can never
            // occupy, which is what made it measurable at all. 63 of them in
            // 20 s, 90 % landing on a reported gap.
            //
            // The cause is that this branch wrote (int16_t)v STRAIGHT to
            // s_out, bypassing smooth_step() - the one thing every normal
            // sample goes through. So the two cascaded biquads kept their
            // pre-gap state frozen for the whole silence and rang it back
            // out the moment audio resumed, on top of a ramp that was
            // correct but inaudible underneath it. Running the fade through
            // the same filter lets that state decay to rest along with the
            // audio, so the filter starts the next frame from silence too.
            //
            // The gaps themselves are a separate problem and NOT ours to fix
            // here: measured 3.08/s with WiFi up against 0.42/s with it off,
            // an 86 % reduction, i.e. they are WiFi/SDIO contention. This
            // makes the ones that remain inaudible rather than pretending
            // they are gone.
            {
                const int fade_n = DSP_SAMPLE_RATE_HZ / 100;  // ~10 ms
                float from = s_last_up_v;
                for (int i = 0; i < DSP_FFT_SIZE; i++) {
                    float v = (i < fade_n) ? from * (1.0f - (float)i / (float)fade_n) : 0.0f;
                    float ys = smooth_step(v);   // <- keeps the biquads in step with what is played
                    int16_t o = (int16_t)ys;
                    s_out[2 * i] = o; s_out[2 * i + 1] = o;
                }
            }
            s_last_up_v = 0.0f;
            // Mirror of the fade-down: ramp the first ~10 ms of resumed audio
            // up from silence. Without it the recovery still has to climb
            // from 0 to full inside RX_DECIM_D == 8 samples (167 us), which
            // is a step at audio rates however clean the filter state is.
            s_resume_ramp = DSP_SAMPLE_RATE_HZ / 100;
            int64_t w0 = esp_timer_get_time();
            rxcap_push(s_out, DSP_FFT_SIZE);   /* record exactly what is played */
            esp_codec_dev_write(s_codec, s_out, DSP_FFT_SIZE * 2 * (int)sizeof(int16_t));
            uint32_t w_us = (uint32_t)(esp_timer_get_time() - w0);
            if (w_us > s_write_us_max) s_write_us_max = w_us;
            s_write_us_sum += w_us;
            continue;
        }
        ever_got_data = true;

        int64_t frame_start_us = esp_timer_get_time();

        // NCO1: shift the wanted signal all the way down to complex baseband 0
        // (removes the QMX's +12 kHz IF AND the mode's own center/offset in
        // one multiply). Keeps BOTH real and imaginary parts - see build_lpf's
        // comment above for why that is the actual fix, not an optimisation.
        for (int i = 0; i < pairs; i++) {
            float c, s;
            nco_step(&s_nco1, (uint32_t)i, &c, &s);
            float I = (float)s_rxbuf[2 * i];
            float Q = (float)s_rxbuf[2 * i + 1];
            // (I + jQ) * (c - js): re = I*c + Q*s, im = Q*c - I*s
            s_mix_re[i] = I * c + Q * s;
            s_mix_im[i] = Q * c - I * s;
        }

        // Stage 1: coarse decimate both legs identically by RX_DECIM_D (fixed
        // filter, see build_decimator()). n_out is pairs/D, floor-divided;
        // any remainder samples (<D, only possible on a partial read - a
        // full DSP_FFT_SIZE read is an exact multiple of 8) are simply not
        // consumed this frame - negligible and self-correcting next frame,
        // not worth carrying state for.
        int n_out = pairs / RX_DECIM_D;
        if (n_out > 0) {
            dsps_fird_f32(&s_dec_fir_re, s_mix_re, s_filt_re, n_out);
            dsps_fird_f32(&s_dec_fir_im, s_mix_im, s_filt_im, n_out);
            // Stage 2: the actual mode-width selectivity, at the now-
            // decimated rate - see the FIR_LEN comment above for why this
            // has to be a second, non-decimating pass rather than folded
            // into stage 1.
            dsps_fir_f32(&s_fir_re, s_filt_re, s_narrow_re, n_out);
            dsps_fir_f32(&s_fir_im, s_filt_im, s_narrow_im, n_out);
        }

        // NCO2 + AGC now run at the DECIMATED rate (n_out samples, not
        // pairs) - NCO2's step was sized for fs/RX_DECIM_D in retune().
        // Shift the filtered result back up so it is audible at the same
        // pitch the QMX's own sidetone would use, then take the real part -
        // only now, after filtering, so the image stays rejected. Per-sample
        // AGC (smooth, no frame-boundary clicks) + noise-floor squelch (gaps
        // go quiet instead of hissing) - squelch floor is 1.0 (fully open)
        // for now, see SQ_FLOOR above.
        float agc_attack   = s_agc_attack;
        float agc_release  = s_agc_release;
        float agc_target   = s_agc_target;
        float agc_gain_max = s_agc_gain_max;
        float out_clamp    = s_out_clamp;

        for (int i = 0; i < n_out; i++) {
            float c, s;
            nco_step(&s_nco2, (uint32_t)i, &c, &s);
            // (narrow_re + j*narrow_im) * (c + js), real part: re*c - im*s
            float re = s_narrow_re[i] * c - s_narrow_im[i] * s;

            float a = fabsf(re);
            if (a > s_agc_env) s_agc_env += (a - s_agc_env) * agc_attack;
            else               s_agc_env += (a - s_agc_env) * agc_release;

            s_noise += (s_agc_env - s_noise) * AGC_NOISE_TC;
            if (s_noise < 1.0f) s_noise = 1.0f;

            float gain = agc_target / (s_agc_env + 1.0f);
            if (gain > agc_gain_max) gain = agc_gain_max;

            float snr = s_agc_env / s_noise;
            float sq  = (snr - SQ_LO) / (SQ_HI - SQ_LO);
            if (sq < 0.0f) sq = 0.0f;
            else if (sq > 1.0f) sq = 1.0f;
            sq = SQ_FLOOR + (1.0f - SQ_FLOOR) * sq;

            float v = re * gain * sq;
            if (v >  out_clamp) { v =  out_clamp; s_clip_count++; }
            if (v < -out_clamp) { v = -out_clamp; s_clip_count++; }
            // Upsample back to the full 48 kHz output rate by LINEAR
            // INTERPOLATION between this sample and the last, not plain
            // sample-and-hold. Found 2026-09-04, on the air: a zero-order
            // hold's staircase has energy at every image of the 6 kHz
            // decimated rate (6, 12, 18 kHz...), and while that is inaudible
            // as a *tone* it beats against the codec's own reconstruction
            // and came through as harsh, metallic high-pitched artifacts on
            // top of an otherwise-narrow, otherwise-correct CW note -
            // reported as "sampled with a too low rate". A first-order
            // (linear) hold's spectrum falls off as sinc^2 instead of sinc -
            // roughly twice the rolloff in dB/octave - which is why this is
            // the documented fallback in the FIR_LEN comment above rather
            // than a new idea. s_last_up_v carries the ramp's end value
            // across frame boundaries so there is no click at i=0 either.
            int base = i * RX_DECIM_D;
            for (int k = 0; k < RX_DECIM_D; k++) {
                float frac = (float)(k + 1) / (float)RX_DECIM_D;
                float y = s_last_up_v + (v - s_last_up_v) * frac;
                // Post-upsample smoothing (see SMOOTH_FC_HZ comment above) -
                // knocks down the interpolation image further, effectively
                // free next to the FIR stages. A Butterworth has no passband
                // overshoot, but re-clamp defensively before the int16 cast
                // anyway - cheap insurance, not expected to ever trigger.
                float ys = smooth_step(y);
                if (s_resume_ramp > 0) {
                    // Linear ramp-in over RESUME_RAMP_N samples. Counted in
                    // OUTPUT samples so it is the same 10 ms as the fade-down
                    // regardless of RX_DECIM_D.
                    ys *= 1.0f - (float)s_resume_ramp / (float)(DSP_SAMPLE_RATE_HZ / 100);
                    s_resume_ramp--;
                }
                if (ys >  out_clamp) ys =  out_clamp;
                if (ys < -out_clamp) ys = -out_clamp;
                int16_t o = (int16_t)ys;
                s_out[2 * (base + k)]     = o;   // L
                s_out[2 * (base + k) + 1] = o;   // R
            }
            s_last_up_v = v;
        }
        // Any tail beyond n_out*RX_DECIM_D (the un-consumed remainder from
        // the floor-divide above) gets silence rather than stale/garbage data.
        for (int i = n_out * RX_DECIM_D; i < pairs; i++) {
            s_out[2 * i] = 0; s_out[2 * i + 1] = 0;
        }

        // DSP-only time (NCO x2 + FIR x2 + AGC) - excludes the intentionally
        // real-time-paced I2S write below on purpose, so this answers "is the
        // math itself keeping up with the 21.3 ms/frame budget" cleanly.
        uint32_t frame_us = (uint32_t)(esp_timer_get_time() - frame_start_us);
        if (frame_us > s_frame_us_max) s_frame_us_max = frame_us;
        s_frame_us_sum += frame_us;
        s_frame_count++;

        // Blocking write paces the task to real time (~21 ms per frame).
        int64_t write_start_us = esp_timer_get_time();
        rxcap_push(s_out, pairs);   /* record exactly what is played */
        rxcap_auto_tick();          /* self-arm / self-dump, no host needed */
        esp_codec_dev_write(s_codec, s_out, pairs * 2 * (int)sizeof(int16_t));
        uint32_t write_us = (uint32_t)(esp_timer_get_time() - write_start_us);
        if (write_us > s_write_us_max) s_write_us_max = write_us;
        s_write_us_sum += write_us;

        // Periodic SERIAL diagnostics, added 2026-09-04 while WiFi was down
        // for the whole session and the /api/cmd rxaudio endpoint (and the
        // /rxaudio tuning page) were unreachable - a peek, not a consumer:
        // reads the same counters rx_audio_take_diag() reads but does NOT
        // reset them, so the two coexist without racing if WiFi ever comes
        // back. "since boot" semantics here, not "since last read".
        {
            static int64_t s_last_diag_log_us = 0;
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_last_diag_log_us >= 10000000) {  // every 10 s
                s_last_diag_log_us = now_us;
                uint32_t fc = s_frame_count;
                uint32_t favg = fc ? (uint32_t)(s_frame_us_sum / fc) : 0;
                uint32_t gn = s_gap_iv_n;
                ESP_LOGI(TAG, "diag: frame %lu/%luus (n=%lu)  write max=%luus  "
                         "read to=%lu  clips=%lu  gap iv %lu/%lu/%lums (n=%lu)",
                         (unsigned long)favg, (unsigned long)s_frame_us_max, (unsigned long)fc,
                         (unsigned long)s_write_us_max,
                         (unsigned long)s_read_timeout_count, (unsigned long)s_clip_count,
                         (unsigned long)(gn ? s_gap_iv_min_ms : 0),
                         (unsigned long)(gn ? s_gap_iv_sum_ms / gn : 0),
                         (unsigned long)s_gap_iv_max_ms, (unsigned long)gn);
            }
        }
    }
}

// ---- Public API --------------------------------------------------------
void rx_audio_init(void)
{
    if (s_task) return;  // already initialised

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    s_enabled = cfg.rx_audio_en;
    s_volume  = cfg.rx_audio_vol;
    // Claim the room from the background feeds for the whole time RX audio is
    // persisted-on, not just from the moment it starts producing frames - see
    // net_quiet.h. Matches rx_audio_set_enabled()'s own hold/release pairing.
    if (s_enabled) net_quiet_hold();

    // Work buffers in PSRAM (core-1 only). Putting all of these in internal
    // RAM starved the internal heap and destabilised boot, so only the
    // cross-core ring lives in internal RAM (see dsp.c).
    s_rxbuf     = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_mix_re    = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),      MALLOC_CAP_SPIRAM);
    s_mix_im    = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),      MALLOC_CAP_SPIRAM);
    s_filt_re   = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),      MALLOC_CAP_SPIRAM);
    s_filt_im   = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),      MALLOC_CAP_SPIRAM);
    s_narrow_re = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),      MALLOC_CAP_SPIRAM);
    s_narrow_im = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),      MALLOC_CAP_SPIRAM);
    s_out       = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_dec_coeff    = heap_caps_malloc(FIR_DECIM_LEN * sizeof(float),  MALLOC_CAP_SPIRAM);
    s_dec_delay_re = heap_caps_malloc(FIR_DECIM_LEN * sizeof(float),  MALLOC_CAP_SPIRAM);
    s_dec_delay_im = heap_caps_malloc(FIR_DECIM_LEN * sizeof(float),  MALLOC_CAP_SPIRAM);
    s_coeff    = heap_caps_malloc(FIR_LEN * sizeof(float),           MALLOC_CAP_SPIRAM);
    s_delay_re = heap_caps_malloc((FIR_LEN + 4) * sizeof(float),     MALLOC_CAP_SPIRAM);
    s_delay_im = heap_caps_malloc((FIR_LEN + 4) * sizeof(float),     MALLOC_CAP_SPIRAM);
    if (!s_rxbuf || !s_mix_re || !s_mix_im || !s_filt_re || !s_filt_im ||
        !s_narrow_re || !s_narrow_im || !s_out ||
        !s_dec_coeff || !s_dec_delay_re || !s_dec_delay_im ||
        !s_coeff || !s_delay_re || !s_delay_im) {
        ESP_LOGE(TAG, "buffer alloc failed; RX audio disabled");
        return;
    }
    retune(CW_DEF_OFFSET);
    build_decimator();                // stage 1 - fixed, built once
    build_smoothing_biquad();         // post-upsample smoothing - fixed, built once
    build_lpf(CW_DEF_WIDTH_HZ / 2);   // stage 2 placeholder - rebuilt on first active loop iteration

    xTaskCreatePinnedToCore(rx_audio_task, "rx_audio", 4096, NULL,
                             RX_AUDIO_TASK_PRIORITY, &s_task, 1);
    ESP_LOGI(TAG, "init (enabled=%d vol=%d codec_ready=%d priority=%d)",
             (int)s_enabled, (int)s_volume, (int)s_codec_ready, RX_AUDIO_TASK_PRIORITY);
}

void rx_audio_preopen(void)
{
    // Open the ES8388 / I2S output path NOW, before the USB host starts and
    // claims the DMA-capable internal RAM. I2S allocates its DMA descriptors
    // from that pool; doing it after the UAC stream is up fails (NO_MEM) and
    // esp_codec_dev_open then crashes on the un-checked error. We keep the
    // codec open for the whole session; the task only writes when RX audio is
    // active. Gated on the persisted enable flag so units that never use RX
    // audio don't claim I2S/DMA at all (and there's zero risk to USB host).
    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (!cfg.rx_audio_en) {
        ESP_LOGI(TAG, "preopen skipped (RX audio disabled)");
        return;
    }
    s_volume = cfg.rx_audio_vol;

    // Same reasoning as the I2S channel below: claim the forward ring's 24 KB
    // now, before WiFi/BLE/spots fragment internal RAM. See dsp.c.
    dsp_rxaudio_ring_preinit();

    // --- Minimal TX-ONLY I2S channel (NOT bsp_audio_codec_speaker_init) ---
    // bsp_audio_init creates BOTH a TX and an RX (mic, TDM 4-slot) channel and
    // uses large default DMA buffers - two GDMA channels + several KB of DMA
    // RAM. That starves the USB host's endpoint allocation (CDC-ACM/CAT can't
    // claim its EPs). We only need playback, so create just the TX channel with
    // small DMA buffers: one GDMA channel, ~1.5 KB DMA, leaving room for USB.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear    = true;
    // ~80 ms of DMA buffering (12 x ~6.7 ms), re-raised 2026-09-04. Was 40 ms
    // (6 descriptors); doubled once already after live diagnostics caught
    // esp_codec_dev_write() blocking up to 48.5 ms, reverted alongside two
    // OTHER unconfirmed changes when none of the three showed a clear fix -
    // but reverting THIS one specifically made the reported chirp audibly
    // worse again, which is real evidence for keeping it even without a full
    // fix. Costs ~7.7 KB more DMA-capable RAM (12 x 320 frames x 4 bytes),
    // claimed here in preopen() - before WiFi/USB/SD start, so it comes out
    // of the pool before their own much larger claims, not away from them
    // mid-session. Still TX-only = one GDMA channel.
    chan_cfg.dma_desc_num  = 12;
    chan_cfg.dma_frame_num = 320;
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "preopen: i2s_new_channel failed");
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DSP_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "preopen: i2s_channel_init_std_mode failed");
        return;
    }
    // Leave the channel in READY state (not enabled) - esp_codec_dev_open
    // reconfigures + enables it.

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM, .tx_handle = s_tx_chan, .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0, .addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es8388_codec_cfg_t es_cfg = {
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .ctrl_if     = ctrl_if,
        .pa_pin      = -1,
    };
    const audio_codec_if_t *es_dev = es8388_codec_new(&es_cfg);
    if (!data_if || !ctrl_if || !es_dev) {
        ESP_LOGE(TAG, "preopen: codec interface init failed");
        return;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es_dev, .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) {
        ESP_LOGE(TAG, "preopen: esp_codec_dev_new failed");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .sample_rate     = DSP_SAMPLE_RATE_HZ,
    };
    int ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "preopen: codec_open failed (%d)", ret);
        return;
    }
    esp_codec_dev_set_out_vol(s_codec, (int)s_volume);
    s_codec_ready = true;
    ESP_LOGI(TAG, "preopen OK (TX-only I2S, codec ready, vol=%d, free_int=%u)",
             (int)s_volume, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void rx_audio_set_enabled(bool en)
{
    bool was = s_enabled;
    s_enabled = en;
    settings_set_rx_audio_en(en);
    // Mirrors rx_audio_init()'s own hold on a transition, never on a
    // same-value call (the UI can call this repeatedly with the same state).
    if (en && !was) net_quiet_hold();
    else if (!en && was) net_quiet_release();
    if (!en) return;
    if (!s_codec_ready) {
        // Codec is only opened at boot (before the USB host takes the DMA
        // RAM). Turning RX audio on mid-session can't open it safely, so it
        // takes effect after a restart.
        ESP_LOGW(TAG, "RX audio enabled - restart required to take effect");
        return;
    }
    if (s_task) xTaskNotifyGive(s_task);   // wake it out of the indefinite block
}

bool rx_audio_is_enabled(void) { return s_enabled; }

void rx_audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_codec) esp_codec_dev_set_out_vol(s_codec, (int)vol);
    settings_set_rx_audio_vol(vol);
}

uint8_t rx_audio_get_volume(void) { return s_volume; }

// ---- Live tuning ------------------------------------------------------
// RAM only, deliberately NOT persisted - these are for iterating on the AGC/
// clip behaviour over the air without a rebuild+reflash+QMX-power-cycle each
// time (main/net/webserver.c's "rxaudio" /api/cmd action). A real bug (the
// FIR tap count) still needed a flash to fix; this is for tuning the parts
// that don't need one.
void rx_audio_set_out_clamp(float v)    { if (v > 0.0f) s_out_clamp    = v; }
void rx_audio_set_agc_target(float v)   { if (v > 0.0f) s_agc_target   = v; }
void rx_audio_set_agc_attack(float v)   { if (v > 0.0f && v <= 1.0f) s_agc_attack  = v; }
void rx_audio_set_agc_release(float v)  { if (v > 0.0f && v <= 1.0f) s_agc_release = v; }
void rx_audio_set_agc_gain_max(float v) { if (v > 0.0f) s_agc_gain_max = v; }

void rx_audio_get_tuning(rx_audio_tuning_t *out)
{
    if (!out) return;
    out->out_clamp    = s_out_clamp;
    out->agc_target   = s_agc_target;
    out->agc_attack   = s_agc_attack;
    out->agc_release  = s_agc_release;
    out->agc_gain_max = s_agc_gain_max;
}

uint32_t rx_audio_take_clip_count(void)
{
    uint32_t n = s_clip_count;
    s_clip_count -= n;   // subtract rather than assign 0 - a clip landing between the read and this line is not lost
    return n;
}

void rx_audio_take_diag(rx_audio_diag_t *out)
{
    if (!out) return;
    uint32_t frames = s_frame_count;
    uint32_t loops  = s_loop_count;
    out->frame_us_avg   = frames ? (uint32_t)(s_frame_us_sum / frames) : 0;
    out->frame_us_max   = s_frame_us_max;
    out->frame_count    = frames;
    out->read_timeouts  = s_read_timeout_count;
    out->read_us_avg    = loops ? (uint32_t)(s_read_us_sum / loops) : 0;
    out->read_us_max    = s_read_us_max;
    out->write_us_avg   = loops ? (uint32_t)(s_write_us_sum / loops) : 0;
    out->write_us_max   = s_write_us_max;
    s_frame_us_max       -= s_frame_us_max;
    s_frame_us_sum        = 0;
    s_frame_count         -= frames;
    s_read_timeout_count  -= s_read_timeout_count;
    s_read_us_sum          = 0;
    s_read_us_max         -= s_read_us_max;
    s_write_us_sum         = 0;
    s_write_us_max        -= s_write_us_max;
    s_loop_count           -= loops;
}

/* Base64 the captured samples into the serial log.
 *
 * ⛔ THIS IS BAUD-LIMITED, so the fix for "it takes 6 minutes" is to send
 * LESS, not to send it faster. Measured 2026-09-06: 12,827 lines of ~110 B
 * in ~130 s = 11.5 KB/s, which is exactly 115200 baud. Longer lines only
 * amortise the ~40 B log prefix and buy about 1.4x; nothing buys more.
 *
 * So only the DIAGNOSTICALLY INTERESTING audio goes out: a reference chunk
 * of ordinary background from the start, plus a window either side of every
 * gap. A 20 s capture with 9 gaps is then ~3.7 s of audio rather than 20 -
 * about 5x less - and the parts dropped are the parts where, by
 * construction, nothing happened.
 *
 * Each region carries its own START SAMPLE INDEX so the decoder can place it
 * and so a region is never silently confused with its neighbour. Regions are
 * merged when they overlap, which matters because two gaps 60 ms apart are
 * routine.
 *
 * ⛔ AND IT RUNS ON ITS OWN TASK. The first version dumped inline from
 * rx_audio_task, which stopped the audio for the whole six minutes - the
 * instrument silencing the thing it is measuring. */
#define RXCAP_REF_SAMPLES  (DSP_SAMPLE_RATE_HZ)        /* 1 s of plain background */
#define RXCAP_GAP_PRE      (DSP_SAMPLE_RATE_HZ / 8)    /* 125 ms before a gap */
#define RXCAP_GAP_POST     (DSP_SAMPLE_RATE_HZ / 8)    /* 125 ms after it      */

static void rxcap_emit_region(uint32_t start, uint32_t count, uint32_t *line_idx)
{
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    ESP_LOGW(TAG, "RXCAP-REGION %lu %lu", (unsigned long)start, (unsigned long)count);
    const uint8_t *p = (const uint8_t *)(s_cap + start);
    uint32_t total = count * sizeof(int16_t);
    char line[248];
    /* ⛔ 180 IS NOT ARBITRARY, AND THE OLD 45 WAS A BUG (measured 2026-09-06).
     *
     * 45 bytes is ODD, so every line boundary split an int16 sample across two
     * lines. Any character lost in transit then manufactured a one-sample
     * NEEDLE - and the needles were indistinguishable from real clicks by the
     * 3-9 kHz detector this whole investigation runs on. 93 % of the 304
     * needles in a 20 s dump sat at byte offsets 40-44 or 0-4 of the 45-byte
     * line (chi-square 2087 against a uniform-distribution threshold of 60).
     * The instrument was manufacturing the artefact it was measuring, and it
     * had already been reported once as "a second, unexplained source".
     *
     * 180 is even AND a multiple of 2, so a line holds exactly 90 whole
     * samples and no sample ever straddles a boundary. It is also a multiple
     * of 3, so base64 never pads mid-stream.
     *
     * The checksum is the other half: a corrupted-but-PRESENT line used to
     * decode silently into fake audio. Now it is dropped and counted, which
     * is the difference between a missing measurement and a wrong one. */
    for (uint32_t off = 0; off < total; off += 180) {
        uint32_t m = (total - off < 180) ? (total - off) : 180;
        int o = 0;
        for (uint32_t j = 0; j < m; j += 3) {
            uint32_t v = (uint32_t)p[off + j] << 16;
            if (j + 1 < m) v |= (uint32_t)p[off + j + 1] << 8;
            if (j + 2 < m) v |= (uint32_t)p[off + j + 2];
            line[o++] = B64[(v >> 18) & 63];
            line[o++] = B64[(v >> 12) & 63];
            line[o++] = (j + 1 < m) ? B64[(v >> 6) & 63] : '=';
            line[o++] = (j + 2 < m) ? B64[v & 63]        : '=';
        }
        line[o] = 0;
        /* Fletcher-16 over the RAW bytes this line encodes - cheap, catches
         * single-byte damage and transposition, which is what a serial link
         * actually does to a line. */
        uint16_t s1 = 0, s2 = 0;
        for (uint32_t j = 0; j < m; j++) {
            s1 = (uint16_t)((s1 + p[off + j]) % 255);
            s2 = (uint16_t)((s2 + s1) % 255);
        }
        ESP_LOGW(TAG, "RXCAP %lu %04x %s", (unsigned long)(*line_idx)++,
                 (unsigned)((s2 << 8) | s1), line);
        if (((*line_idx) & 0x0F) == 0) vTaskDelay(1);   /* let the console drain */
    }
}

static void rxcap_dump_task(void *arg)
{
    (void)arg;
    uint32_t n = s_cap_n;
    if (!s_cap || n == 0) { s_cap_dumping = false; vTaskDelete(NULL); return; }

    ESP_LOGW(TAG, "RXCAP-BEGIN rate=%d samples=%lu gaps=%lu",
             DSP_SAMPLE_RATE_HZ, (unsigned long)n, (unsigned long)s_cap_gap_n);
    for (uint32_t i = 0; i < s_cap_gap_n; i++)
        ESP_LOGW(TAG, "RXCAP-GAP %lu", (unsigned long)s_cap_gap[i]);

    uint32_t line_idx = 0;
    /* Region 0: plain background, the reference the gaps are compared against. */
    uint32_t cur_lo = 0;
    uint32_t cur_hi = (RXCAP_REF_SAMPLES < n) ? RXCAP_REF_SAMPLES : n;

    for (uint32_t i = 0; i < s_cap_gap_n; i++) {
        uint32_t g  = s_cap_gap[i];
        uint32_t lo = (g > RXCAP_GAP_PRE) ? g - RXCAP_GAP_PRE : 0;
        uint32_t hi = g + RXCAP_GAP_POST;
        if (hi > n) hi = n;
        if (lo <= cur_hi) {                 /* overlaps - merge, do not re-send */
            if (hi > cur_hi) cur_hi = hi;
            continue;
        }
        rxcap_emit_region(cur_lo, cur_hi - cur_lo, &line_idx);
        cur_lo = lo; cur_hi = hi;
    }
    if (cur_hi > cur_lo) rxcap_emit_region(cur_lo, cur_hi - cur_lo, &line_idx);

    ESP_LOGW(TAG, "RXCAP-END lines=%lu", (unsigned long)line_idx);
    s_cap_dumping = false;
    vTaskDelete(NULL);
}

/* Called once per audio frame. Arms the one-shot capture RXCAP_AUTO_DELAY_MS
 * after audio first flows, and dumps it when it fills - both without any
 * host involvement, which is the whole point (see the note above). */
static void rxcap_auto_tick(void)
{
#if RXCAP_AUTO_SECONDS > 0
    if (s_cap_first_us == 0) s_cap_first_us = esp_timer_get_time();
    if (!s_cap_auto_done &&
        esp_timer_get_time() - s_cap_first_us > (int64_t)RXCAP_AUTO_DELAY_MS * 1000) {
        s_cap_auto_done = true;
        if (rx_audio_cap_start(RXCAP_AUTO_SECONDS)) {
            s_cap_autodump = true;
            ESP_LOGW(TAG, "cap: AUTO-ARMED %d s - will base64 to serial when full",
                     RXCAP_AUTO_SECONDS);
        }
    }
    if (s_cap_autodump && !s_cap_run && s_cap_n > 0 && !s_cap_dumping) {
        s_cap_autodump = false;
        s_cap_dumping  = true;
        /* Own task: the buffer is one-shot and finished, so reading it from
         * another task races nothing - and dumping inline would stop the
         * audio for the whole transfer. */
        if (xTaskCreatePinnedToCore(rxcap_dump_task, "rxcap_dump", 4096, NULL,
                                    1, NULL, 1) != pdPASS) {
            s_cap_dumping = false;
            ESP_LOGE(TAG, "cap: could not start the dump task");
        }
    }
#endif
}

bool rx_audio_cap_start(int seconds)
{
    if (seconds < 1) seconds = 1;
    if (seconds > 30) seconds = 30;
    uint32_t want = (uint32_t)seconds * (uint32_t)DSP_SAMPLE_RATE_HZ;
    s_cap_run = false;
    if (s_cap && s_cap_cap != want) { heap_caps_free(s_cap); s_cap = NULL; s_cap_cap = 0; }
    if (!s_cap) {
        s_cap = heap_caps_malloc((size_t)want * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cap) { ESP_LOGE(TAG, "cap: no PSRAM for %lu samples", (unsigned long)want); return false; }
        s_cap_cap = want;
    }
    s_cap_n = 0; s_cap_gap_n = 0;
    s_cap_run = true;
    ESP_LOGW(TAG, "cap: recording %d s (%lu samples) - one shot", seconds, (unsigned long)want);
    return true;
}

void rx_audio_cap_stop(void) { s_cap_run = false; }

void rx_audio_cap_status(uint32_t *n, uint32_t *cap, bool *running)
{
    if (n) *n = s_cap_n;
    if (cap) *cap = s_cap_cap;
    if (running) *running = s_cap_run;
}

const int16_t *rx_audio_cap_data(uint32_t *n) { if (n) *n = s_cap_n; return s_cap; }
const uint32_t *rx_audio_cap_gaps(uint32_t *n) { if (n) *n = s_cap_gap_n; return s_cap_gap; }
