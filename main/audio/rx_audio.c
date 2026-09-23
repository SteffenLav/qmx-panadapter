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
// Stack for the short-lived core-1 task that opens the I2S channel + codec
// (see rx_audio_preopen). 4096 overflowed on the first boot it was tried;
// 16 KB is deliberately far more than the measured need, which the task logs
// as a high water mark - this runs once at boot and is freed immediately.
#define RXAUD_PREOPEN_STACK    16384

#define RX_AUDIO_TASK_PRIORITY (DSP_FFT_TASK_PRIORITY - 1)
_Static_assert(RX_AUDIO_TASK_PRIORITY < DSP_FFT_TASK_PRIORITY,
               "rx_audio_task must never be able to preempt fft_task");

// ---- Demod mode -----------------------------------------------------------
typedef enum {
    RXAUD_MODE_NONE = 0,   // unsupported CAT mode - path stays idle
    RXAUD_MODE_CW,
    RXAUD_MODE_SSB,        // USB: wanted audio sits ABOVE the dial
    RXAUD_MODE_SSB_L,      // LSB: wanted audio sits BELOW the dial
} rxaud_mode_t;

/* ⛔ LSB AND USB ARE NOT THE SAME PASSBAND, AND THEY WERE TREATED AS ONE UNTIL
 * v1.16.1 (Gyula HA3HZ: "please listen to the LSB, something isn't right").
 *
 * The QMX's LO sits 12 kHz below the dial, so baseband DC is the dial and the
 * SIGN of a baseband offset says which side of the dial you are on. USB audio
 * occupies dial+200 .. dial+200+w, i.e. POSITIVE offsets; LSB occupies
 * dial-200 .. dial-(200+w), i.e. NEGATIVE ones. Collapsing both to one mode
 * left filter_params_for_mode() with only the positive case, so on LSB the
 * demodulator tuned the wrong side of the dial entirely - it played whatever
 * was mirrored there rather than the station on the display.
 *
 * ⭐ ui.c's compute_passband_edges_hz() has had this right all along, and its
 * LSB branch is the authority this now agrees with - that is where to look
 * first if a third mode ever needs adding, rather than deriving it again. */
static rxaud_mode_t mode_from_cat_str(const char *m)
{
    if (!m) return RXAUD_MODE_NONE;
    if (strcmp(m, "CW") == 0 || strcmp(m, "CW-R") == 0) return RXAUD_MODE_CW;
    if (strcmp(m, "USB") == 0) return RXAUD_MODE_SSB;
    if (strcmp(m, "LSB") == 0) return RXAUD_MODE_SSB_L;
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
// Headroom for the continuous clock-drift resampler (see rx_audio_preopen's
// s_out allocation and the upsample loop). Sized for the resample clamp's
// worst case, not the ~2.5 extra samples the measured 0.24% drift actually
// needs - "generous, not incremental" after the bug this undersizing caused.
#define RX_OUT_HEADROOM 64
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
// ⭐ The "revisit it if it turns out to matter" note that used to sit here is
// RESOLVED - it turned out to matter, and the rescale is applied at
// DEF_AGC_ATTACK / DEF_AGC_RELEASE below with the full reasoning. Kept in
// summary because the shape of the mistake is worth remembering: the rescale
// was correct arithmetic, was reverted after being tested against CW clicking,
// and CW is the one signal whose steady envelope never exercises an AGC's rate.
// The other two changes reverted in that same 2026-09-04 session (silence-gap
// fade, doubled DMA buffer) remain out and are still unconfirmed.
#define DEF_OUT_CLAMP      32000.0f  // hard clip before int16 cast (headroom)
#define DEF_AGC_TARGET     30000.0f
/* ⭐ RESCALED FOR THE DECIMATED LOOP - and this time the symptom that judges it
 * is SSB intelligibility, not CW clicking.
 *
 * These are per-sample coefficients and the AGC runs on the fs/RX_DECIM_D
 * stream (6 kHz), not 48 kHz. tau ~= 1/(alpha*fs), so at 6 kHz the bare figures
 * gave 1/(0.007*6000) = 24 ms attack and 1/(0.00014*6000) = 1.2 s release -
 * eight times slower than the 3 ms / 150 ms they were written for, exactly the
 * RX_DECIM_D factor.
 *
 * ⛔ WHY THIS WAS REVERTED ONCE AND IS BACK. The rescale was tried before and
 * dropped because it "made no confirmed difference to the reported on-air
 * artifact" - but that artifact was CW CLICKING, and CW is the one case these
 * constants cannot hurt: a steady tone settles the AGC once and never tests its
 * rate. The change was judged against a symptom it could not have fixed.
 *
 * Gyula HA3HZ, 2026-09-21, is the symptom it CAN fix: "LSB/USB signals appear
 * very distorted ... unintelligible", while CW the same afternoon was clear.
 * Speech is what a 24 ms attack and a 1.2 s release destroy - every syllable
 * onset overshoots into out_clamp before the gain moves, then the gain stays
 * ducked through the syllables that follow.
 *
 * Multiplying by RX_DECIM_D restores the intended 3 ms / 150 ms and, because it
 * is written as the scaling rather than a new literal, it follows RX_DECIM_D if
 * the decimation ever changes again - which is how it drifted in the first
 * place.
 *
 * ⚠ NOT VERIFIED ON AIR. Both are live-tunable over /api/cmd "rxaudio", so if
 * this does bring back a CW artifact it can be put back without a reflash. */
#define DEF_AGC_ATTACK     (0.007f   * (float)RX_DECIM_D)  // ~3 ms at the 6 kHz loop rate
#define DEF_AGC_RELEASE    (0.00014f * (float)RX_DECIM_D)  // ~150 ms at the 6 kHz loop rate
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
/* Per-10-s-window counters - see the diag line. Reset every window on purpose:
   a running maximum cannot distinguish one stall from a thousand. */
static volatile uint32_t s_write_late_win = 0;   /* writes over 25 ms */
static volatile uint32_t s_frames_win     = 0;   /* frames in this window */
static volatile uint32_t s_pairs_win      = 0;   /* sample-pairs written this window */
/* Clock-drift corrector state - see the rate-match block in the task loop. */
static int64_t  s_rate_t0_us   = 0;   /* wall clock when the accounting began */
static int64_t  s_rate_written = 0;   /* pairs written since s_rate_t0_us */
static volatile uint32_t s_rate_ins  = 0;  /* pairs duplicated (source slow) */
static volatile uint32_t s_rate_drop = 0;  /* pairs dropped    (source fast) */
static volatile int64_t  s_win_start_us   = 0;   /* when this window began */
/* ⛔ THE SILENT FAILURE MODE. The TX channel is created with auto_clear = true,
   so when the DMA ring runs dry it plays ZEROS and says nothing - no counter,
   no log line, and it sounds exactly like the break-ups being chased. This is
   fed by the I2S on_send_q_ovf callback, which is the only notification the
   driver offers that data was lost. If it stays 0 while the operator hears
   break-ups, the artifact is NOT an underrun and the search moves elsewhere. */
static volatile uint32_t s_i2s_ovf = 0;
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

// ---- Panoramic CW split (2026-09-20) --------------------------------------
// Splits the mode's already-selected passband (s_narrow_re/im above, which
// spans the QMX's own chosen CW filter width) into its LOWER and UPPER
// halves and pans each hard L/R, so two DIFFERENT CW stations sitting on
// opposite sides of the tuned pitch separate spatially instead of both
// landing in one mono note. This replaced an earlier "binaural" attempt
// (one signal at two nearly-identical detuned pitches - a weak-signal aid,
// not spatial separation of multiple signals) that was the wrong technique
// for what was actually wanted; see the git log for that one's own reasoning
// if it is ever worth reviving as a separate mode.
//
// Standard "complex bandpass via pre-shift + real lowpass + remix": no
// complex-coefficient filter is implemented directly (this file's FIR
// machinery is real-coefficient only). Instead each half is shifted to
// baseband first (s_nco_pre, +/- half_bw_hz/2), passed through a REAL
// lowpass at half that cutoff (reusing build_lpf's own sinc/Hamming design,
// just called with a smaller bandwidth - build_lpf_half() below), then
// remixed up to its true position by s_nco_l / s_nco_r (repurposed from the
// old technique - same struct, same retune() call site, new meaning).
// s_nco_pre itself is declared below, alongside s_nco_l/s_nco_r - it is an
// nco_t, and that typedef isn't in scope yet at this point in the file.
static float   *s_coeff_half     = NULL;  // [FIR_LEN] - cutoff = half_bw_hz/2, shared by low+high, low+re/im
static float   *s_delay_low_re   = NULL;  // [FIR_LEN+4] each - four independent legs, one per (channel,I/Q)
static float   *s_delay_low_im   = NULL;
static float   *s_delay_high_re  = NULL;
static float   *s_delay_high_im  = NULL;
static fir_f32_t s_fir_low_re, s_fir_low_im, s_fir_high_re, s_fir_high_im;
static float   *s_low_re  = NULL, *s_low_im  = NULL;  // [DSP_FFT_SIZE/RX_DECIM_D] each - final, post-filter
static float   *s_high_re = NULL, *s_high_im = NULL;
static float   *s_pre_low_re  = NULL, *s_pre_low_im  = NULL;  // scratch: pre-shifted, pre-filter
static float   *s_pre_high_re = NULL, *s_pre_high_im = NULL;
static int s_half_built_for_hz = -1;   // half_bw_hz the panoramic filter pair was last built for

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
// NCO2 is now TWO instances, L and R - the FINAL remix step for panoramic
// CW's two already-separated halves (see the "Panoramic CW split" block
// below). Plain mono (binaural off, or any non-CW mode) drives both to the
// same center_hz, which makes mono not an approximation of the panoramic
// code but an EXACT special case of it: same steps, same phase, so L and R
// compute byte-identical output every sample, same as the single-NCO
// version this replaced did.
static nco_t s_nco_l = {1.0f, 0.0f, 1.0f, 0.0f};
static nco_t s_nco_r = {1.0f, 0.0f, 1.0f, 0.0f};
// Pre-shift NCO for the panoramic split - see the "Panoramic CW split" block
// above for what it does and why it needs only one instance for both halves.
static nco_t s_nco_pre = {1.0f, 0.0f, 1.0f, 0.0f};
#define NCO_RENORM_EVERY 64   // samples between renormalisations

// RAM-only (see rx_audio.h) - CW/CW-R only; retune() below enforces that by
// only ever splitting when mode == RXAUD_MODE_CW.
static volatile bool s_binaural_en = false;
// Cross-feed fraction, 0.0 (hard L/R split) .. 0.5 (fully centered/mono).
// See the per-sample loop for why a hard split reads as "always one ear,
// never the middle" even for a dead-center station. RAM-only, live-tunable,
// same class as the AGC params. 0.35 (the first value tried) collapsed the
// image to "a mix more or less in the center" - too much. 0.15 confirmed on
// the air, real separation still intact plus noticeable center presence.
static volatile float s_pan_blend = 0.15f;
// 0.0 (hard split, original behaviour) .. 1.0 (each half filter nearly as
// wide as the original passband - heavy overlap in the middle). See
// build_lpf_half() for the full reasoning. Changing this needs a filter
// REBUILD (unlike pan_blend, which is a plain per-sample multiply), so it is
// tracked alongside half_bw in the main loop rather than read fresh every
// sample.
static volatile float s_pan_overlap = 0.3f;
// Mid/side stereo width multiplier, 1.0 = unchanged, >1.0 = wider (exaggerates
// L-R difference), <1.0 = narrower. Plain per-sample multiply, unlike
// pan_overlap - no filter rebuild needed. See the per-sample loop for the
// full reasoning; default picked to noticeably widen the perceived image.
static volatile float s_pan_width = 1.8f;

static float s_agc_env = 1.0f;
/* Peak limiter (see the block in the per-sample loop). 26000 of 32767 leaves
   about 2 dB of headroom under the hard clamp, so the clamp stops being part
   of normal operation. The release is rescaled for the decimated loop exactly
   like the AGC's - it is a per-sample coefficient and this runs at
   DSP_SAMPLE_RATE_HZ/RX_DECIM_D, the same trap that made SSB unintelligible
   until it was found. ~250 ms at the 6 kHz rate. */
#define LIM_THRESH   26000.0f
#define LIM_RELEASE  (0.00008f * (float)RX_DECIM_D)
static float s_lim_gain = 1.0f;
/* Fractional-resample state for the clock-drift corrector. s_up_inc is the
   phase advance per OUTPUT sample; 1/RX_DECIM_D is exactly no correction, and
   the accounting below nudges it by a few parts per million to track the
   radio. Clamped hard: this multiplies the playback rate, so a runaway value
   would change the pitch. */
static float s_up_phase = 0.0f;
static float s_up_inc   = 1.0f / (float)RX_DECIM_D;
static volatile uint32_t s_rate_railed = 0;   /* times the rate loop hit its clamp */
static float    s_up_target    = 0.0f;   /* smoothed increment from the measured source rate */
static uint32_t s_in_pairs_win = 0;      /* input pairs seen in the current measuring window */
static int64_t  s_in_win_us    = 0;      /* when that window started */
static float s_noise   = 1.0f;     // slow noise-floor estimate (for squelch)
// Linear-interpolation upsample state, one per ear - the value each
// channel's ramp ended on, carried forward so the next frame's ramp starts
// from where the last one left off instead of jumping. In plain mono these
// two are always numerically identical (same input, same filter, same
// history), which is exactly what makes the mono case exact rather than
// approximate. See the upsample comment below for why plain sample-and-hold
// was replaced in the first place.
static float s_last_up_v_l = 0.0f;
static float s_last_up_v_r = 0.0f;
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
// Two cascaded stages = 4th order, one full set per ear so panoramic CW's
// two channels each get their own independent filter STATE (x1/x2/y1/y2) -
// a single shared filter fed alternating L/R samples would smear one
// channel's history into the other's output. In plain mono both channels
// are fed the identical input sequence from identical initial state, so
// they stay numerically identical throughout - same "exact, not
// approximate" property as the NCOs and the upsample state above.
static biquad_t s_smooth_l_a, s_smooth_l_b;
static biquad_t s_smooth_r_a, s_smooth_r_b;

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

// Panoramic CW's per-half filter. Same sinc/Hamming design as build_lpf(),
// at a cutoff of half_bw_hz/2 * (1 + s_pan_overlap) so it isolates one half
// of the already-selected passband once that half has been shifted to
// baseband by s_nco_pre (see the per-sample loop). Coefficients are shared
// between the low and high channels - same cutoff, same window - only the
// four delay lines (independent per channel/leg) differ, which is why this
// builds one coefficient array but initialises four fir_f32_t instances
// from it.
//
// s_pan_overlap is the STRUCTURAL fix for "the middle disappeared, and
// bringing it back weakens the sides" (operator, 2026-09-20): a post-mix
// blend (s_pan_blend, see the per-sample loop) can only trade separation for
// center presence, because it is redistributing energy that the two half
// filters already put ENTIRELY on one side or the other - overlap changes
// how much energy near the crossover lands in BOTH filters' passbands to
// begin with. At overlap=0 each filter's cutoff is exactly half_bw/2 (the
// original hard split - the crossover sits right at each filter's own edge,
// where its transition band gives only the fixed, narrow ~78 Hz blend this
// file shipped with first). At overlap=1 each filter's cutoff approaches
// half_bw_hz itself - nearly the WHOLE original passband - so a station well
// off-center still gets real separation (it is still much closer to one
// filter's passband center than the other), while a station near the
// crossover now has genuine energy in both from the filtering itself, not
// from mixing borrowed from the other channel. The two controls are meant to
// be used together: overlap sets how gradual the pan is across the whole
// width, pan_blend is a lighter final touch on top.
static void build_lpf_half(int half_bw_hz)
{
    if (half_bw_hz < 20) half_bw_hz = 20;
    float overlap = s_pan_overlap;
    if (overlap < 0.0f) overlap = 0.0f;
    if (overlap > 1.0f) overlap = 1.0f;
    int quarter_bw_hz = (int)((float)(half_bw_hz / 2) * (1.0f + overlap));
    if (quarter_bw_hz < 10) quarter_bw_hz = 10;
    if (quarter_bw_hz > half_bw_hz) quarter_bw_hz = half_bw_hz;   // never wider than the source passband itself

    float fs = (float)DSP_SAMPLE_RATE_HZ / (float)RX_DECIM_D;
    float fc = (float)quarter_bw_hz / fs;
    int   M  = FIR_LEN - 1;
    float half = M / 2.0f;

    for (int n = 0; n < FIR_LEN; n++) {
        float m  = (float)n - half;
        float lp = 2.0f * fc * sinc_norm(2.0f * fc * m);
        float w  = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)n / (float)M); // Hamming
        s_coeff_half[n] = lp * w;
    }
    float sum = 0.0f;
    for (int n = 0; n < FIR_LEN; n++) sum += s_coeff_half[n];
    if (fabsf(sum) > 1e-6f) {
        for (int n = 0; n < FIR_LEN; n++) s_coeff_half[n] /= sum;
    }

    memset(s_delay_low_re,  0, (FIR_LEN + 4) * sizeof(float));
    memset(s_delay_low_im,  0, (FIR_LEN + 4) * sizeof(float));
    memset(s_delay_high_re, 0, (FIR_LEN + 4) * sizeof(float));
    memset(s_delay_high_im, 0, (FIR_LEN + 4) * sizeof(float));
    dsps_fir_init_f32(&s_fir_low_re,  s_coeff_half, s_delay_low_re,  FIR_LEN);
    dsps_fir_init_f32(&s_fir_low_im,  s_coeff_half, s_delay_low_im,  FIR_LEN);
    dsps_fir_init_f32(&s_fir_high_re, s_coeff_half, s_delay_high_re, FIR_LEN);
    dsps_fir_init_f32(&s_fir_high_im, s_coeff_half, s_delay_high_im, FIR_LEN);
    s_half_built_for_hz = half_bw_hz;
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
    s_smooth_l_a = c; s_smooth_l_b = c;
    s_smooth_r_a = c; s_smooth_r_b = c;
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
// negligible cost - see the SMOOTH_FC_HZ comment above. One version per ear;
// smooth_step() (the original name) stays as the L-channel/mono call so every
// existing call site keeps working unchanged, smooth_step_r() is new.
static inline float smooth_step(float x)
{
    return biquad_step(&s_smooth_l_b, biquad_step(&s_smooth_l_a, x));
}
static inline float smooth_step_r(float x)
{
    return biquad_step(&s_smooth_r_b, biquad_step(&s_smooth_r_a, x));
}

// (Re)point NCO1, the pre-shift NCO, and both ears' final-remix NCO2 at a
// new center frequency/half-bandwidth and reset phase - a phase jump here is
// one click on a mode/filter change, same tradeoff the old AGC-reset-on-
// mode-change already made.
//
// Panoramic CW's three extra frequencies are all derived HERE, not in the
// per-sample loop, for the same reason the plain center frequency already
// was: computing sinf/cosf once here instead of every sample is the whole
// reason this file has an NCO struct at all (see the "called sinf/cosf
// FRESH EVERY SAMPLE" comment above).
//
// CW/CW-R + binaural-on only: any other case forces the split to 0, which
// points s_nco_l and s_nco_r at the SAME frequency as plain center_hz would
// have used - i.e. the mono path is an exact special case of this one, not
// an approximation of it (see the per-sample loop for what that buys).
static void retune(int center_hz, int half_bw_hz, rxaud_mode_t mode)
{
    float fs = (float)DSP_SAMPLE_RATE_HZ;
    float fs_dec = fs / (float)RX_DECIM_D;   // NCO2/pre run on the decimated stream
    float w1 = 2.0f * (float)M_PI * (float)(12000 + center_hz) / fs;

    bool panoramic = (mode == RXAUD_MODE_CW && s_binaural_en);
    float half_split = panoramic ? (float)half_bw_hz / 2.0f : 0.0f;

    float w2l = 2.0f * (float)M_PI * ((float)center_hz - half_split) / fs_dec;
    float w2r = 2.0f * (float)M_PI * ((float)center_hz + half_split) / fs_dec;
    // Pre-shift: moves the LOWER half [-half_bw,0] up to baseband (shift-up
    // formula in the loop) and the UPPER half [0,+half_bw] down to baseband
    // (shift-down formula) using the SAME nco_pre at +half_split - one NCO,
    // two mix formulas, see the per-sample loop.
    float wpre = 2.0f * (float)M_PI * half_split / fs_dec;

    s_nco1.step_re = cosf(w1); s_nco1.step_im = sinf(w1);
    s_nco_l.step_re = cosf(w2l); s_nco_l.step_im = sinf(w2l);
    s_nco_r.step_re = cosf(w2r); s_nco_r.step_im = sinf(w2r);
    s_nco_pre.step_re = cosf(wpre); s_nco_pre.step_im = sinf(wpre);
    s_nco1.re = 1.0f; s_nco1.im = 0.0f;
    s_nco_l.re = 1.0f; s_nco_l.im = 0.0f;
    s_nco_r.re = 1.0f; s_nco_r.im = 0.0f;
    s_nco_pre.re = 1.0f; s_nco_pre.im = 0.0f;
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
    } else {   // RXAUD_MODE_SSB / RXAUD_MODE_SSB_L
        if (w == 0) w = SSB_DEF_WIDTH_HZ;
        int low  = SSB_LOW_HZ;
        int high = SSB_LOW_HZ + (int)w;
        *center_hz = (low + high) / 2;
        *half_bw_hz = (high - low) / 2;
        // LSB is the mirror of USB about the dial: same width, negative
        // centre. Mirrors ui.c's compute_passband_edges_hz(), which computes
        // the LSB edges as -(SSB_LOW + w) .. -SSB_LOW. half_bw is unsigned
        // and identical for both - only the centre carries the side.
        if (mode == RXAUD_MODE_SSB_L) *center_hz = -*center_hz;
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
    bool binaural_prev = false;
    float overlap_prev = -1.0f;   // force a build_lpf_half() call on the first active loop pass
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
            s_lim_gain = 1.0f;
            s_up_phase = 0.0f;
            s_up_inc   = 1.0f / (float)RX_DECIM_D;
            s_up_target = 0.0f; s_in_pairs_win = 0; s_in_win_us = 0;
            s_noise   = 1.0f;
            s_last_up_v_l = 0.0f;
            s_last_up_v_r = 0.0f;
            ever_got_data = false;
            s_rate_t0_us = 0;          /* restart the drift accounting, not a deficit */
            s_rate_written = 0;
            dsp_rxaudio_forward_enable(true);
            active_prev = true;
            ESP_LOGI(TAG, "RX audio on (mode=%s vol=%d)",
                     mode == RXAUD_MODE_CW    ? "CW"  :
                     mode == RXAUD_MODE_SSB_L ? "LSB" : "USB", (int)s_volume);
        }

        // Track the live filter target (CW offset moves; SSB is fixed but the
        // mode itself can change) and retune/rebuild if it moved. Panoramic
        // CW's enable flag is also live-tunable (rx_audio.h), tracked
        // separately since it changes neither want_center nor mode - and
        // retune() now needs half_bw too (the split/pre-shift frequencies
        // depend on it), so a width change re-retunes as well as rebuilding
        // the filters, where it used to only do the latter.
        int want_center, want_half_bw;
        filter_params_for_mode(mode, &want_center, &want_half_bw);
        bool want_binaural = s_binaural_en;
        float want_overlap = s_pan_overlap;
        bool half_bw_changed = (want_half_bw != s_half_bw_hz);
        bool overlap_changed = (want_overlap != overlap_prev);
        if (mode != mode_prev || want_center != s_center_hz ||
            want_binaural != binaural_prev || half_bw_changed) {
            retune(want_center, want_half_bw, mode);
            mode_prev = mode;
            binaural_prev = want_binaural;
        }
        if (half_bw_changed) {
            build_lpf(want_half_bw);
        }
        if (half_bw_changed || overlap_changed) {
            build_lpf_half(want_half_bw);
            overlap_prev = want_overlap;
        }

        s_loop_count++;
        int64_t read_start_us = esp_timer_get_time();
        int pairs = (int)dsp_rxaudio_read(s_rxbuf, DSP_FFT_SIZE, 60);
        const int in_pairs = pairs;   /* INPUT count - `pairs` becomes the OUTPUT count below */
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
                float from_l = s_last_up_v_l;
                float from_r = s_last_up_v_r;
                for (int i = 0; i < DSP_FFT_SIZE; i++) {
                    float vl = (i < fade_n) ? from_l * (1.0f - (float)i / (float)fade_n) : 0.0f;
                    float vr = (i < fade_n) ? from_r * (1.0f - (float)i / (float)fade_n) : 0.0f;
                    // Both channels' filters must be stepped, or the one not
                    // stepped keeps its pre-gap state frozen and rings it back
                    // out on resume - the exact bug this whole fade exists to
                    // avoid, just reintroduced in whichever ear got skipped.
                    float ysl = smooth_step(vl);
                    float ysr = smooth_step_r(vr);
                    s_out[2 * i] = (int16_t)ysl; s_out[2 * i + 1] = (int16_t)ysr;
                }
            }
            s_last_up_v_l = 0.0f;
            s_last_up_v_r = 0.0f;
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

            // Panoramic CW split: only when CW/CW-R + the flag is on. Splits
            // the passband s_narrow_re/im already carries (the QMX's own
            // selected CW filter width) into its lower and upper halves.
            //
            // Pre-shift both halves to baseband with ONE nco_pre (frequency
            // half_bw/2, set in retune()) using the two complex-multiply
            // variants: shift UP moves the LOWER half [-half_bw,0] to sit at
            // baseband; shift DOWN moves the UPPER half [0,+half_bw] to sit
            // at baseband. Then a REAL lowpass at half_bw/2 (build_lpf_half)
            // isolates each - a real filter cannot separate +f from -f, but
            // once a half is sitting at baseband a lowpass IS that half's
            // own selectivity filter. This is the standard "complex bandpass
            // via pre-shift + real lowpass + remix" construction, not a true
            // complex-coefficient filter (this file's FIR machinery is
            // real-coefficient only) - cheaper, and reuses build_lpf's exact
            // sinc/Hamming design unchanged.
            bool panoramic_now = (mode == RXAUD_MODE_CW) && s_binaural_en;
            if (panoramic_now) {
                for (int i = 0; i < n_out; i++) {
                    float cp, sp;
                    nco_step(&s_nco_pre, (uint32_t)i, &cp, &sp);
                    float zre = s_narrow_re[i], zim = s_narrow_im[i];
                    // Shift UP by half_bw/2 (Z * e^{+j*wpre*n}): lower half -> baseband.
                    s_pre_low_re[i]  = zre * cp - zim * sp;
                    s_pre_low_im[i]  = zre * sp + zim * cp;
                    // Shift DOWN by half_bw/2 (Z * e^{-j*wpre*n}): upper half -> baseband.
                    s_pre_high_re[i] = zre * cp + zim * sp;
                    s_pre_high_im[i] = zim * cp - zre * sp;
                }
                dsps_fir_f32(&s_fir_low_re,  s_pre_low_re,  s_low_re,  n_out);
                dsps_fir_f32(&s_fir_low_im,  s_pre_low_im,  s_low_im,  n_out);
                dsps_fir_f32(&s_fir_high_re, s_pre_high_re, s_high_re, n_out);
                dsps_fir_f32(&s_fir_high_im, s_pre_high_im, s_high_im, n_out);
            }
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
        bool  panoramic_now = (mode == RXAUD_MODE_CW) && s_binaural_en;

        // Stereo width, BANDWIDTH-COMPENSATED: the two half-band filters'
        // own transition zone is a FIXED number of Hz (FIR_LEN taps at
        // fs_dec, not a function of half_bw_hz at all), so it eats a much
        // bigger FRACTION of a narrow CW filter than a wide one - at
        // half_bw=250 (500 Hz filter) it is maybe a sixth of the passband;
        // at half_bw=50 (100 Hz filter) it can be most of it. That is why
        // narrowing the filter collapsed the stereo spread even with
        // pan_width unchanged (operator, 2026-09-20: "when I narrow the bw
        // ... it needs to be tied to the bw"). Compensate by scaling width
        // UP as half_bw shrinks below the reference it was tuned at (250 Hz
        // half-width = 500 Hz filter, the session's own test bandwidth), so
        // the FELT spread stays roughly constant as the operator zooms the
        // CW filter in or out. Clamped both ways: never below the operator's
        // own pan_width (a WIDE filter should not get LESS spread than what
        // was tuned), never above 4x it (an extremely narrow filter, e.g.
        // 50 Hz, would otherwise demand an absurd multiplier).
        /* Output write cursor. The upsampler emits RX_DECIM_D samples per
           decimated sample normally, and one more or one fewer on the frame
           where the clock-drift accounting asks for it - so the output length
           is counted, not computed. */
        int outn = 0;

        float eff_pan_width = s_pan_width;
        if (panoramic_now && s_half_bw_hz > 0) {
            const float PAN_WIDTH_REF_HALF_BW_HZ = 250.0f;   // half of the 500 Hz test filter
            float scale = PAN_WIDTH_REF_HALF_BW_HZ / (float)s_half_bw_hz;
            if (scale < 1.0f) scale = 1.0f;   // never REDUCE width for a wider-than-reference filter
            if (scale > 8.0f) scale = 8.0f;   // allow up to 8x for very narrow filters
            eff_pan_width = s_pan_width * scale;
        }

        for (int i = 0; i < n_out; i++) {
            float cl, sl, cr, sr;
            nco_step(&s_nco_l, (uint32_t)i, &cl, &sl);
            nco_step(&s_nco_r, (uint32_t)i, &cr, &sr);
            // (X_re + j*X_im) * (c + js), real part: re*c - im*s. Plain mono
            // (panoramic off, or any non-CW mode) reads BOTH channels from
            // the same s_narrow_re/im with s_nco_l == s_nco_r (retune()
            // forces the split to 0), so re_l == re_r every sample - the
            // mono case is an exact special case of this code, not merely
            // close to it. Panoramic reads each channel from its OWN
            // already-separated half (computed above).
            float re_l, re_r;
            if (panoramic_now) {
                // Swapped 2026-09-20 per the operator's ear: the lower half
                // of the passband (below the tuned pitch) sounds right to
                // him in the RIGHT ear, not the left - nco_l/s_low_* etc.
                // keep their names (they still mean "the -half_split-shifted
                // channel"), only which output channel they feed is flipped.
                re_r = s_low_re[i]  * cl - s_low_im[i]  * sl;
                re_l = s_high_re[i] * cr - s_high_im[i] * sr;
            } else {
                re_l = s_narrow_re[i] * cl - s_narrow_im[i] * sl;
                re_r = s_narrow_re[i] * cr - s_narrow_im[i] * sr;
            }

            // AGC/squelch are driven from ONE shared envelope, not each
            // channel's own - in mono that's re_l alone (byte-identical to
            // the original single-channel path, since re_l == re_r there
            // anyway). In panoramic mode a station can genuinely exist on
            // only ONE side, so take the STRONGER of the two - an L-only
            // envelope would fail to bring up a station that only exists on
            // the right. Either way it stays ONE shared gain applied to both
            // channels: a per-ear AGC would let the two ears' loudness drift
            // apart independently, which defeats the spatial cue this whole
            // feature exists to provide.
            float a = panoramic_now ? fmaxf(fabsf(re_l), fabsf(re_r)) : fabsf(re_l);
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

            float v_l = re_l * gain * sq;
            float v_r = re_r * gain * sq;
            // Cross-feed: a hard L/R split (blend=0) means a station right
            // at the crossover point only gets picked up by whichever half
            // filter's transition band happens to catch it - a real tone is
            // narrow enough that it lands mostly on ONE side even when it is
            // sitting dead-center, so the stereo image reads as "always hard
            // L or hard R, nothing in between" (operator's report,
            // 2026-09-20). Mixing a fraction of each channel into the other
            // widens the effective blend zone across the whole passband
            // instead of just the filters' own narrow transition band -
            // still panned by which side a station is actually on, just not
            // ALL THE WAY to one ear. panoramic_now-gated only for cost;
            // mono already has v_l == v_r so it would be a no-op regardless.
            if (panoramic_now) {
                float blend = s_pan_blend;
                float bl = v_l + blend * (v_r - v_l);
                float br = v_r + blend * (v_l - v_r);
                v_l = bl; v_r = br;

                // Stereo WIDTH, standard mid/side widening: at width=1 this
                // is a no-op (mid+side == v_l, mid-side == v_r, always); at
                // width>1 it exaggerates the difference between the ears
                // beyond what pan_overlap/pan_blend produced, without
                // touching how centered a centered station sounds (mid is
                // untouched - only side is scaled). This is the answer to
                // "the whole image only swings +/-30 degrees, needs to be
                // +/-60 or more" (operator, 2026-09-20): overlap/blend shape
                // WHERE energy goes near the crossover, width controls how
                // FAR APART the two ears end up sounding once it has.
                float mid  = 0.5f * (v_l + v_r);
                float side = 0.5f * (v_l - v_r);
                v_l = mid + eff_pan_width * side;
                v_r = mid - eff_pan_width * side;
            }
            /* ⭐⭐ PEAK LIMITER - "extremely strong signals almost blew my
             * ears" (operator, 2026-09-22).
             *
             * The AGC alone could never prevent this. It normalises to
             * agc_target, which is 30000 of 32767 - 92% of full scale - so
             * EVERY signal arrives near maximum, and anything faster than its
             * ~3 ms attack goes straight through to the hard clamp below as
             * clipping. That clamp is not protection, it is distortion: it
             * squares off the waveform and the result is both loud AND harsh,
             * which is exactly what a strong CW signal in headphones sounds
             * like. It is also where the 17,692 clips measured this evening
             * were coming from.
             *
             * ⛔ THE FIX IS NOT A LOWER agc_target. Samuel W7STF is on the same
             * firmware asking for MORE volume, and turning the target down
             * would quieten everybody to solve one operator's peaks. A limiter
             * is the control that does only what is asked: it holds the peaks
             * down and leaves the average level alone.
             *
             * Instantaneous attack, slow release - the standard shape. The gain
             * can only ever fall immediately (so nothing escapes) and recovers
             * gently, so there is no pumping on CW. One gain for both ears, or
             * the binaural image would shift sideways whenever one channel
             * limited on its own. */
            {
                float peak = fmaxf(fabsf(v_l), fabsf(v_r));
                float need = (peak > LIM_THRESH) ? (LIM_THRESH / peak) : 1.0f;
                if (need < s_lim_gain) s_lim_gain = need;              /* catch it now */
                else s_lim_gain += (1.0f - s_lim_gain) * LIM_RELEASE;  /* let go slowly */
                v_l *= s_lim_gain;
                v_r *= s_lim_gain;
            }

            /* Still clamped afterwards, but it should now be unreachable - the
               limiter holds peaks below LIM_THRESH, which is under out_clamp.
               s_clip_count becoming non-zero again means the limiter is not
               doing its job, so this stays as the instrument that would say so. */
            if (v_l >  out_clamp) { v_l =  out_clamp; s_clip_count++; }
            if (v_l < -out_clamp) { v_l = -out_clamp; s_clip_count++; }
            if (v_r >  out_clamp) { v_r =  out_clamp; s_clip_count++; }
            if (v_r < -out_clamp) { v_r = -out_clamp; s_clip_count++; }
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
            // than a new idea. s_last_up_v_l/r carry each ramp's end value
            // across frame boundaries so there is no click at i=0 either.
            /* ⭐ THE CLOCK DRIFT IS ABSORBED HERE, INSIDE THE INTERPOLATION.
             *
             * The first cut spliced a duplicated sample pair onto the end of the
             * frame instead. Measured on a 700 Hz test tone, host-side:
             *
             *   clean reference                      113.2 dB SNDR
             *   duplicate one sample per frame        31.3 dB
             *   absorb it in the interpolation        43.8 dB
             *
             * Repeating a sample is a step discontinuity whose SIZE IS THE
             * SIGNAL AMPLITUDE, ~47 times a second - which the operator heard
             * immediately and described exactly: "a ripple or noise that lingers
             * with the tone level" (2026-09-22). It was my own fix making the
             * audio worse than the drift it corrected.
             *
             * ⛔ AND A PER-FRAME STEP IS NOT GOOD ENOUGH EITHER. Stretching one
             * input sample's worth of output by a single step got rid of the
             * spliced sample, but it still put ONE timing event in every frame -
             * and a frame is 46.9 Hz. Measured on the bench afterwards: sidebands
             * at +/-46.9 Hz around the tone at -27 to -32 dBc, and the operator
             * heard what was left as "slightly better... but chirps from time to
             * time". A once-per-frame correction has a once-per-frame spectrum,
             * however gently it is applied.
             *
             * ⭐ So the drift is now spread over EVERY sample by a phase
             * accumulator - proper fractional resampling. The interpolation runs
             * at a rate fractionally different from 1/RX_DECIM_D, and there is no
             * per-frame event to have a spectrum at all. Measured host-side on a
             * 700 Hz tone, same net correction in each case:
             *
             *   no correction at all (v1.16.1)  SNDR 32.9 dB   46.9 Hz  -160 dBc
             *   per-frame single step           SNDR 29.0 dB   46.9 Hz   -37 dBc
             *   continuous fractional resample  SNDR 33.0 dB   46.9 Hz   -74 dBc
             *
             * i.e. it restores the UNCORRECTED purity exactly while still
             * absorbing the clock difference. (The 33 dB floor is this linear
             * upsampler itself and is present in every version - not a
             * regression, and the thing to improve if anyone wants more.)
             *
             * s_up_phase carries the fractional position across frames, so there
             * is no discontinuity at a frame boundary either. */
            /* ⛔⛔ THE ACTUAL RUNAWAY BUG (found 2026-09-23, on re-reading the
             * whole block rather than re-simulating the rate math again - the
             * operator was right that simulation was not finding this).
             *
             * The buffer only had "+1" pair of headroom, sized for the OLD
             * spliced corrector which added at most one sample. THIS
             * resampler can need MANY more: stretching a full DSP_FFT_SIZE
             * (1024) input frame by even the measured 0.24% needs ~2.5 EXTRA
             * output samples, and dsp_rxaudio_read asks for a full frame
             * essentially every call - so the `outn < DSP_FFT_SIZE` cap was
             * being hit on ordinary frames, not just a pathological one, and
             * NOTHING ABOUT THAT DEPENDS ON THERE BEING A SIGNAL - which is
             * why the operator heard it grow on a completely silent band.
             *
             * And the old code decremented s_up_phase UNCONDITIONALLY after
             * the while loop:
             *
             *     while (s_up_phase < 1.0f && outn < CAP) { ... }
             *     s_up_phase -= 1.0f;
             *
             * When the loop exits because the CAP stopped it (phase still
             * <1.0, not because phase reached 1.0), subtracting 1.0 anyway
             * drives s_up_phase NEGATIVE. Next input sample, the while
             * condition (phase < 1.0) is now true for many more iterations
             * than it should be, each one computing `frac = s_up_phase` -
             * strongly negative - which the linear interpolation
             * (last + delta*frac) EXTRAPOLATES far outside the two real
             * samples instead of interpolating between them. If the cap is
             * hit again before phase claws back to normal (likely, since the
             * buffer is still the same size), phase goes more negative still.
             * That is a compounding, self-worsening runaway with no signal
             * amplitude anywhere in its cause - exactly "starts faint, then
             * increases, and increases" on a silent band, and exactly why the
             * two earlier "fixes" to the RATE MATH (attempts #3 and #4 in the
             * commit history) could never have touched it: this bug is in
             * the BUFFER ACCOUNTING around the resampler, not in the rate
             * controller feeding it.
             *
             * Fixed two ways, either of which alone would have stopped the
             * compounding, but both belong here:
             *   1. RX_OUT_HEADROOM (64 pairs) - sized for the resample
             *      clamp's worst case (~21 samples at +/-2%), not the ~2.5
             *      actually needed today, so ordinary operation never
             *      touches the cap at all.
             *   2. The phase decrement is now conditional on the loop having
             *      exited NORMALLY (phase actually reached >=1.0). If the cap
             *      stops it instead, phase is left exactly as it was and the
             *      remaining decimated samples for this call are silently
             *      skipped (a few microseconds of audio, not a corruption) -
             *      the next call resumes cleanly from a valid phase.
             *
             * This is also why "measure, don't simulate the part you already
             * modelled" only gets you so far: the rate-drift simulations were
             * accurate FOR THE RATE MATH. They could not have found a buffer
             * size bug because the model never allocated a buffer. */
            {
                float dl = v_l - s_last_up_v_l;
                float dr = v_r - s_last_up_v_r;
                bool up_phase_completed = false;
                while (s_up_phase < 1.0f && outn < DSP_FFT_SIZE + RX_OUT_HEADROOM) {
                float frac = s_up_phase;
                s_up_phase += s_up_inc;
                if (s_up_phase >= 1.0f) up_phase_completed = true;
                float yl = s_last_up_v_l + dl * frac;
                float yr = s_last_up_v_r + dr * frac;
                // Post-upsample smoothing (see SMOOTH_FC_HZ comment above) -
                // knocks down the interpolation image further, effectively
                // free next to the FIR stages. A Butterworth has no passband
                // overshoot, but re-clamp defensively before the int16 cast
                // anyway - cheap insurance, not expected to ever trigger.
                float ysl = smooth_step(yl);
                float ysr = smooth_step_r(yr);
                if (s_resume_ramp > 0) {
                    // Linear ramp-in over RESUME_RAMP_N samples. Counted in
                    // OUTPUT samples so it is the same 10 ms as the fade-down
                    // regardless of RX_DECIM_D. Shared by both channels - the
                    // gap that triggered it was silence in both ears alike.
                    float ramp = 1.0f - (float)s_resume_ramp / (float)(DSP_SAMPLE_RATE_HZ / 100);
                    ysl *= ramp; ysr *= ramp;
                    s_resume_ramp--;
                }
                if (ysl >  out_clamp) ysl =  out_clamp;
                if (ysl < -out_clamp) ysl = -out_clamp;
                if (ysr >  out_clamp) ysr =  out_clamp;
                if (ysr < -out_clamp) ysr = -out_clamp;
                s_out[2 * outn]     = (int16_t)ysl;   // L
                s_out[2 * outn + 1] = (int16_t)ysr;   // R
                outn++;
                }
                /* Only carry the fraction when the loop finished NORMALLY -
                   see the note above the loop. If the cap stopped it instead,
                   s_up_phase is already < 1.0 and must be left untouched, or
                   it goes negative and the next sample extrapolates instead
                   of interpolating. */
                if (up_phase_completed) s_up_phase -= 1.0f;
            }
            s_last_up_v_l = v_l;
            s_last_up_v_r = v_r;
        }
        // Any tail beyond what the upsampler actually wrote gets silence
        // rather than stale/garbage data. outn, not n_out*RX_DECIM_D: the
        // continuous resampler makes the written length vary by the whole
        // RX_OUT_HEADROOM range now (see the upsample loop), not by one - a
        // no-op here when outn >= pairs, which stretching makes the common
        // case.
        for (int i = outn; i < pairs; i++) {
            s_out[2 * i] = 0; s_out[2 * i + 1] = 0;
        }
        if (outn > 0) pairs = outn;   /* play exactly what was produced, which
                                          may now be MORE than the input count -
                                          rxcap_push/esp_codec_dev_write both
                                          take pairs as a parameter, not a
                                          fixed size, so this is safe. */

        // DSP-only time (NCO x2 + FIR x2 + AGC) - excludes the intentionally
        // real-time-paced I2S write below on purpose, so this answers "is the
        // math itself keeping up with the 21.3 ms/frame budget" cleanly.
        uint32_t frame_us = (uint32_t)(esp_timer_get_time() - frame_start_us);
        if (frame_us > s_frame_us_max) s_frame_us_max = frame_us;
        s_frame_us_sum += frame_us;
        s_frame_count++;

        // Blocking write paces the task to real time (~21 ms per frame).
        int64_t write_start_us = esp_timer_get_time();
        /* ⭐⭐ CLOCK-DRIFT CORRECTION - THE QMX AND THE CODEC DO NOT SHARE A CLOCK.
         *
         * Measured 2026-09-22, and this is the actual cause of the audio
         * break-ups, not CPU load:
         *
         *     QMX delivers over USB :  47,885 pairs/s
         *     ES8388 I2S plays at   :  48,000 samples/s, exactly, from the
         *                              Tab5's own oscillator
         *
         * A 0.24% shortfall. We can only write what the radio sends, so the DMA
         * ring loses ~115 samples every second. A descriptor is 320 frames
         * (~6.7 ms), so one comes up empty roughly every 2.8 s and auto_clear
         * fills it with ZEROS - silently. That is why every counter reads clean
         * (I2Sovf flat, read to=0, clips=0, gap n=0) while the operator plainly
         * hears it. It is also, almost certainly, the "chirp every 4-5 s on a
         * silent band" recorded at the top of this file over a year ago.
         *
         * The fix is to stop letting the source dictate the output rate. This
         * holds the OUTPUT at exactly DSP_SAMPLE_RATE_HZ against the wall clock
         * by duplicating or dropping ONE sample pair per frame when the running
         * total drifts by a whole pair. At 0.24% that is one pair in ~420 -
         * about 21 µs of correction per second, inaudible - and it self-tracks
         * if the radio's rate changes with temperature or between units.
         *
         * ⛔ Bounded to +/-1 pair per frame on purpose. That is up to ~0.1% per
         * frame, over four times the drift being corrected, so it always
         * catches up - but it can never run away and resample the audio if the
         * accounting is ever wrong.
         *
         * ⚠ The accumulator resets whenever audio restarts (see !active_prev),
         * or a mode change would be charged as a vast deficit and the corrector
         * would insert a burst. */
        if (pairs > 0) {
            int64_t now_rm = esp_timer_get_time();
            if (s_rate_t0_us == 0) { s_rate_t0_us = now_rm; s_rate_written = 0; }
            int64_t elapsed  = now_rm - s_rate_t0_us;
            int64_t expected = elapsed * DSP_SAMPLE_RATE_HZ / 1000000;
            int64_t written  = s_rate_written + pairs;
            /* Nudge the RESAMPLE RATE, not the sample count. err is already an
               integral of the rate mismatch, so a proportional step here is
               integral control on the rate - it settles on the radio's true
               rate and then stops moving, which is what removes the per-frame
               event entirely. The gain is deliberately tiny: the whole
               correction needed is 0.24%, and a loop that hunts would be
               audible as exactly the chirp being removed. */
            /* ⛔ THE SIGN HERE IS THE WHOLE LOOP, AND I HAD IT BACKWARDS ONCE.
             *
             * s_up_inc is the phase advance per OUTPUT sample, so the number of
             * outputs produced per input is ~1/s_up_inc. Being BEHIND the clock
             * (err > 0) means we owe more output, which needs a SMALLER
             * increment. Adding err instead of subtracting it is positive
             * feedback: behind -> fewer samples -> further behind, until the
             * clamp. The operator heard it as crackling that grew steadily
             * louder until he switched the radio off (2026-09-22). A rate loop
             * with the sign inverted has no safe gain - the clamp is the only
             * thing that stops it, and by then it is changing the pitch.
             *
             * Integral control on the rate: err is already the accumulated
             * sample deficit, so stepping the rate by it drives err to zero and
             * then holds, which is what leaves no periodic event behind. */
            /* ⛔⛔ NO FEEDBACK LOOP HERE. THE RATE IS COMPUTED, NOT SERVOED.
             *
             * Two attempts at a loop both failed on the bench, and the second
             * one is why the operator had to switch his radio off:
             *
             *  1. `s_up_inc += err` - the SIGN was inverted. s_up_inc is the
             *     phase advance per OUTPUT sample, so outputs per input is
             *     ~1/s_up_inc: being behind needs a SMALLER increment, not a
             *     larger one. Simulated afterwards, err diverged to 32,298
             *     pairs and hit the clamp 5,642 times. Heard as crackling that
             *     grew steadily louder.
             *  2. `s_up_inc -= err` with the sign fixed converges, but a pure
             *     integrator against a transport delay RINGS - simulated, it
             *     hunted around the answer instead of settling.
             *
             * ⭐ There is nothing to servo. The required rate is known exactly:
             * the radio delivers in_pairs per frame, each input sample yields
             * 1/s_up_inc outputs, and the codec needs DSP_SAMPLE_RATE_HZ of
             * them per second. So
             *
             *     s_up_inc = source_pairs_per_second / (RX_DECIM_D * FS)
             *
             * measured over a smoothed window. Simulated: settles on -0.2396%
             * for a 47,885 pairs/s radio - the exact figure needed - with a
             * residual error of 2.4 pairs and the clamp never touched, and it
             * follows a mid-session rate change without ringing.
             *
             * The tiny err term is a trim, not the controller: it nulls the
             * slow residue from rounding, and it CANNOT run away because
             * s_up_inc is recomputed from the measurement every frame rather
             * than accumulated. */
            const float inc_nom = 1.0f / (float)RX_DECIM_D;
            int64_t err = expected - written;            /* + = we are behind */
            if (err >  2000) err =  2000;
            if (err < -2000) err = -2000;

            s_in_pairs_win += (uint32_t)in_pairs;
            int64_t win_us = now_rm - s_in_win_us;
            if (s_in_win_us == 0) { s_in_win_us = now_rm; s_in_pairs_win = 0; }
            else if (win_us >= 1000000) {                /* 1 s of measurement */
                float src_rate = (float)s_in_pairs_win * 1000000.0f / (float)win_us;
                float target   = src_rate / ((float)RX_DECIM_D * (float)DSP_SAMPLE_RATE_HZ);
                s_up_target = (s_up_target <= 0.0f) ? target
                                                    : s_up_target + (target - s_up_target) * 0.25f;
                s_in_win_us = now_rm; s_in_pairs_win = 0;
            }
            if (s_up_target > 0.0f) s_up_inc = s_up_target - (float)err * 2.0e-10f;

            /* ±2% is far more than any real unit needs (0.24% here) and is the
               only thing between a wrong measurement and a pitch change, so it
               says so when it is reached instead of sitting at the rail. */
            if (s_up_inc > inc_nom * 1.02f || s_up_inc < inc_nom * 0.98f) {
                s_up_inc = (s_up_inc > inc_nom) ? inc_nom * 1.02f : inc_nom * 0.98f;
                if ((s_rate_railed++ % 200) == 0)
                    ESP_LOGW(TAG, "resample rate hit the rail (err=%lld) - audio "
                                  "may be off pitch", (long long)err);
            }
            if (err > 0) s_rate_ins++; else if (err < 0) s_rate_drop++;
            s_rate_written += pairs;
        }

        rxcap_push(s_out, pairs);   /* record exactly what is played */
        rxcap_auto_tick();          /* self-arm / self-dump, no host needed */
        esp_codec_dev_write(s_codec, s_out, pairs * 2 * (int)sizeof(int16_t));
        uint32_t write_us = (uint32_t)(esp_timer_get_time() - write_start_us);
        if (write_us > s_write_us_max) s_write_us_max = write_us;
        s_write_us_sum += write_us;
        /* ⭐ EVENT RATE, NOT A RUNNING MAXIMUM (2026-09-22).
         *
         * "write max" is cumulative and never resets, so it cannot tell a
         * single 185 ms stall at start-up from one every second - and that is
         * exactly the question the operator's "break-ups less than a second
         * apart" asks. These two counters are per-window and printed alongside,
         * and they are what decides whether the write path is the artifact at
         * all. A frame is 21.3 ms of audio, so a write over 25 ms means the
         * task was late for that frame. */
        if (write_us > 25000) s_write_late_win++;
        s_frames_win++;
        /* ⭐ THE NUMBER THAT DECIDES IT: are we feeding the codec at exactly
         * 48 kHz? The I2S plays at precisely DSP_SAMPLE_RATE_HZ no matter what
         * we do. Feed it less and the DMA ring drains and you hear gaps; feed
         * it more and the write simply blocks (which is the intended pacing,
         * see the comment above - so "write took a long time" proves nothing).
         * pairs is VARIABLE per frame, so this cannot be derived from the frame
         * count, which is why the LATE counter above could not answer it. */
        s_pairs_win += (uint32_t)pairs;

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
                         "LATE %lu/%lu  OUT %lu smp/s  drift +%lu/-%lu  I2Sovf %lu  "
                         "read to=%lu  clips=%lu  gap iv %lu/%lu/%lums (n=%lu)",
                         (unsigned long)favg, (unsigned long)s_frame_us_max, (unsigned long)fc,
                         (unsigned long)s_write_us_max,
                         (unsigned long)s_write_late_win, (unsigned long)s_frames_win,
                         (unsigned long)(s_win_start_us
                             ? (uint32_t)((uint64_t)s_pairs_win * 1000000ULL
                                          / (uint64_t)(now_us - s_win_start_us))
                             : 0),
                         (unsigned long)s_rate_ins, (unsigned long)s_rate_drop,
                         (unsigned long)s_i2s_ovf,
                         (unsigned long)s_read_timeout_count, (unsigned long)s_clip_count,
                         (unsigned long)(gn ? s_gap_iv_min_ms : 0),
                         (unsigned long)(gn ? s_gap_iv_sum_ms / gn : 0),
                         (unsigned long)s_gap_iv_max_ms, (unsigned long)gn);
                s_write_late_win = 0;
                s_frames_win     = 0;
                s_pairs_win      = 0;
                s_win_start_us   = now_us;
            }
        }
    }
}

// ---- Public API --------------------------------------------------------
/* Push the stored gain and pan values into the live DSP.
 *
 * Two callers, and the second is the reason this is public rather than folded
 * into rx_audio_init(): a config IMPORT writes the settings but has no way to
 * make them audible, so without this a restored backup would read correctly on
 * the sliders and sound like the old values until the next reboot.
 *
 * settings.c clamps on the way in, so the values arriving here are already
 * inside the ranges rx_audio.h documents. */
void rx_audio_apply_settings(void)
{
    rx_audio_set_agc_gain_max((float)settings_get_rxaud_gain_d10()       * 10.0f);
    rx_audio_set_pan_width   ((float)settings_get_rxaud_pan_width_x10()  / 10.0f);
    rx_audio_set_pan_blend   ((float)settings_get_rxaud_pan_blend_x100() / 100.0f);
    rx_audio_set_pan_overlap ((float)settings_get_rxaud_pan_ovlp_x100()  / 100.0f);
}

void rx_audio_init(void)
{
    if (s_task) return;  // already initialised

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    s_enabled = cfg.rx_audio_en;
    s_volume  = cfg.rx_audio_vol;
    rx_audio_apply_settings();   // gain ceiling + the three pan controls
    // NOT net_quiet - that stays OTA-only (see net_quiet.h). Standing
    // feeds that should hold off while RX audio is on (SelfSpotter, RBN,
    // DX cluster, PSK Reporter RX) check rx_audio_is_enabled() directly
    // instead, added 2026-09-20 - net_quiet was too blunt: it held off
    // POTA/SOTA and PSK Reporter TX too, which are periodic/batched, own no
    // standing task, and the operator specifically asked to keep running
    // alongside audio once the panel made the blanket rule visible.

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
    // RX_OUT_HEADROOM pairs of headroom - NOT the "+1" the old spliced
    // corrector needed. The continuous resampler can produce MORE than
    // DSP_FFT_SIZE output samples from a full DSP_FFT_SIZE-pair input frame
    // whenever it is stretching (source slower than the codec), and a full
    // frame is common (dsp_rxaudio_read asks for up to DSP_FFT_SIZE every
    // time). At the resample clamp's worst case (+/-2%, see s_up_inc below)
    // a 1024-pair frame needs up to ~21 EXTRA samples - the "+1" allocated
    // for the old scheme silently truncated that, which is the actual cause
    // of the runway/growing-crackle bug (2026-09-23): see the note by
    // RX_OUT_HEADROOM's use in the upsample loop for the mechanism.
    s_out       = heap_caps_malloc((DSP_FFT_SIZE + RX_OUT_HEADROOM) * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_dec_coeff    = heap_caps_malloc(FIR_DECIM_LEN * sizeof(float),  MALLOC_CAP_SPIRAM);
    s_dec_delay_re = heap_caps_malloc(FIR_DECIM_LEN * sizeof(float),  MALLOC_CAP_SPIRAM);
    s_dec_delay_im = heap_caps_malloc(FIR_DECIM_LEN * sizeof(float),  MALLOC_CAP_SPIRAM);
    s_coeff    = heap_caps_malloc(FIR_LEN * sizeof(float),           MALLOC_CAP_SPIRAM);
    s_delay_re = heap_caps_malloc((FIR_LEN + 4) * sizeof(float),     MALLOC_CAP_SPIRAM);
    s_delay_im = heap_caps_malloc((FIR_LEN + 4) * sizeof(float),     MALLOC_CAP_SPIRAM);
    // Panoramic CW split - see the block comment by the statics above.
    // Internal RAM, NOT PSRAM, unlike every other buffer in this file - these
    // four are read on EVERY tap of EVERY inner-loop iteration of FOUR FIR
    // instances per sample (dsps_fir_f32_ansi's delay-line scan touches up to
    // FIR_LEN=255 floats per call), not once per sample like the output
    // arrays. First panoramic build measured frame_us_avg 25-32 ms against a
    // 21.3 ms budget (7-9x the ~3.5 ms mono baseline, which uses the exact
    // same dsps_fir_f32 on PSRAM buffers of the same shape) - the existing 2
    // filter instances' PSRAM traffic fits comfortably, but tripling to 6
    // simultaneous instances (2 existing + 4 new) saturates something PSRAM
    // access doesn't have the headroom for at that rate. Same class of fix
    // as dsp.c's FFT buffers and audio.c's ring (CLAUDE.md: "a PSRAM spill
    // makes the STFT ~10x slower") - hot per-sample DSP state belongs
    // internal on this board. ~5 KB, affordable after tonight's taskLVGL fix
    // (internal free 7 KB -> 50 KB steady state). UNVERIFIED until measured
    // on hardware - this is a hypothesis about the mechanism, not a proven
    // fix; say so if asked, and check frame_us before trusting it.
    s_coeff_half    = heap_caps_malloc(FIR_LEN * sizeof(float),       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_delay_low_re  = heap_caps_malloc((FIR_LEN + 4) * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_delay_low_im  = heap_caps_malloc((FIR_LEN + 4) * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_delay_high_re = heap_caps_malloc((FIR_LEN + 4) * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_delay_high_im = heap_caps_malloc((FIR_LEN + 4) * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_low_re  = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),        MALLOC_CAP_SPIRAM);
    s_low_im  = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),        MALLOC_CAP_SPIRAM);
    s_high_re = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),        MALLOC_CAP_SPIRAM);
    s_high_im = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),        MALLOC_CAP_SPIRAM);
    s_pre_low_re  = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),    MALLOC_CAP_SPIRAM);
    s_pre_low_im  = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),    MALLOC_CAP_SPIRAM);
    s_pre_high_re = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),    MALLOC_CAP_SPIRAM);
    s_pre_high_im = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),    MALLOC_CAP_SPIRAM);
    if (!s_rxbuf || !s_mix_re || !s_mix_im || !s_filt_re || !s_filt_im ||
        !s_narrow_re || !s_narrow_im || !s_out ||
        !s_dec_coeff || !s_dec_delay_re || !s_dec_delay_im ||
        !s_coeff || !s_delay_re || !s_delay_im ||
        !s_coeff_half || !s_delay_low_re || !s_delay_low_im ||
        !s_delay_high_re || !s_delay_high_im ||
        !s_low_re || !s_low_im || !s_high_re || !s_high_im ||
        !s_pre_low_re || !s_pre_low_im || !s_pre_high_re || !s_pre_high_im) {
        ESP_LOGE(TAG, "buffer alloc failed; RX audio disabled");
        return;
    }
    retune(CW_DEF_OFFSET, CW_DEF_WIDTH_HZ, RXAUD_MODE_NONE);   // panoramic is never live before mode is known
    build_decimator();                // stage 1 - fixed, built once
    build_smoothing_biquad();         // post-upsample smoothing - fixed, built once
    build_lpf(CW_DEF_WIDTH_HZ / 2);        // stage 2 placeholder - rebuilt on first active loop iteration
    build_lpf_half(CW_DEF_WIDTH_HZ / 2);   // panoramic split placeholder - same

    // ⛔ FOUND 2026-09-20: this return value was never checked, and the task
    // never got created - SILENTLY, every boot, for the whole session. No
    // "RX audio on" line, no 10 s diag line, ever, in a 2.2M-line capture
    // spanning many boots including ones confirmed in CW with the codec
    // ready. rx_audio_init() runs AFTER panadapter_wifi_start() (main.c), so
    // this 4096 B internal-RAM stack is requested right in the same boot
    // window that starves everything else here - the exact class of bug
    // this board has hit repeatedly (BLE's assert, OTA's silently-failed
    // task). A few retries a few seconds apart is cheap: internal free
    // measured recovering to 42 KB by ~17.5 s on this same boot sequence.
    BaseType_t created = xTaskCreatePinnedToCore(rx_audio_task, "rx_audio", 4096, NULL,
                             RX_AUDIO_TASK_PRIORITY, &s_task, 1);
    for (int attempt = 1; created != pdPASS && attempt <= 5; attempt++) {
        ESP_LOGW(TAG, "rx_audio task create failed (%d/5) - retrying in 2 s", attempt);
        vTaskDelay(pdMS_TO_TICKS(2000));
        created = xTaskCreatePinnedToCore(rx_audio_task, "rx_audio", 4096, NULL,
                             RX_AUDIO_TASK_PRIORITY, &s_task, 1);
    }
    if (created != pdPASS) {
        ESP_LOGE(TAG, "rx_audio task create failed after 5 attempts - RX audio dead this session");
        s_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "init (enabled=%d vol=%d codec_ready=%d priority=%d)",
             (int)s_enabled, (int)s_volume, (int)s_codec_ready, RX_AUDIO_TASK_PRIORITY);
}

/* ⛔ THE I2S TX INTERRUPT MUST BE REGISTERED FROM CORE 1, NOT CORE 0.
 *
 * ESP-IDF allocates a peripheral interrupt on whichever core calls
 * esp_intr_alloc, and i2s_new_channel() does that inside preopen(). preopen()
 * is called from main.c's app_main, which runs on CORE 0 - so the I2S TX ISR
 * landed on core 0, at dma_frame_num 320 / 48 kHz = one interrupt every 6.7 ms,
 * 150 per second, for the whole session whenever RX audio is enabled.
 *
 * ⛔ CORE 0 IS THE WALL ON THIS BOARD and has been for a year - see display.c's
 * note by .task_affinity, where moving taskLVGL off it was FALSIFIED on
 * hardware. Measured 2026-09-22 with the radio streaming: idle0 0.0-0.2%, core
 * 1 at 53% idle. webserver_ws.c's note recording idle0 ~12% with this pipeline
 * running, and that a browser was NOT the driver, predates RX audio
 * (2026-07-14) - audio consumed the remaining headroom, which is why a browser
 * left open now BREAKS THE AUDIO (operator A/B: closing it returned idle0
 * 0.1% -> 2.9%, and Gyula HA3HZ had already told the list to close the page).
 *
 * ⚠ THE FIRST ATTEMPT AT THIS CRASHED THE BOOT, and the reason is recorded
 * because it is the same mistake CLAUDE.md already names: the task was given
 * 4096 bytes. esp_codec_dev_open + i2s_channel_init_std_mode need far more,
 * and it died with SP and MTVAL eight bytes apart - a stack overflow - on the
 * first boot. "Generous, not incremental" exists for exactly this. The high
 * water mark is logged below so the real figure is a measurement, not another
 * guess.
 *
 * ⚠ HYPOTHESIS UNTIL MEASURED: that moving the ISR returns a useful slice of
 * core 0. The handler is short; what it costs on a core with no headroom is
 * what is being tested. The revert is this wrapper and nothing else. */
// ISR context: touch nothing but the counter.
static IRAM_ATTR bool rx_audio_i2s_ovf_cb(i2s_chan_handle_t h, i2s_event_data_t *e, void *u)
{
    (void)h; (void)e; (void)u;
    s_i2s_ovf++;
    return false;
}

static void preopen_body(void);

static void preopen_task(void *arg)
{
    preopen_body();
    ESP_LOGW(TAG, "preopen: core-1 open done, stack high water %u B of %u",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)RXAUD_PREOPEN_STACK);
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

void rx_audio_preopen(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(preopen_task, "rxaud_pre", RXAUD_PREOPEN_STACK,
                                self, 5, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "preopen: core-1 task create failed - opening on this core "
                      "instead (the I2S ISR will stay on core 0)");
        preopen_body();
        return;
    }
    // Bounded: a hang here must not take the boot with it.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000)) == 0)
        ESP_LOGE(TAG, "preopen: core-1 open did not finish in 15 s");
}

static void preopen_body(void)
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
    // The only notification the driver gives that TX data was lost. See
    // s_i2s_ovf: auto_clear makes an underrun silent otherwise.
    {
        i2s_event_callbacks_t cbs = { .on_send_q_ovf = rx_audio_i2s_ovf_cb };
        if (i2s_channel_register_event_callback(s_tx_chan, &cbs, NULL) != ESP_OK)
            ESP_LOGW(TAG, "preopen: could not register the I2S overflow callback");
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
    s_enabled = en;
    settings_set_rx_audio_en(en);
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

// Takes effect via the same mode/center-tracking check rx_audio_task()
// already runs every loop (binaural_prev comparison) - no restart, no
// re-enable, same "live" promise as the AGC tuning above.
void rx_audio_set_binaural_enabled(bool en) { s_binaural_en = en; }
bool rx_audio_get_binaural_enabled(void) { return s_binaural_en; }
void rx_audio_set_pan_blend(float v) { if (v >= 0.0f && v <= 0.5f) s_pan_blend = v; }
float rx_audio_get_pan_blend(void) { return s_pan_blend; }
void rx_audio_set_pan_overlap(float v) { if (v >= 0.0f && v <= 1.0f) s_pan_overlap = v; }
float rx_audio_get_pan_overlap(void) { return s_pan_overlap; }
void rx_audio_set_pan_width(float v) { if (v >= 0.0f && v <= 3.0f) s_pan_width = v; }
float rx_audio_get_pan_width(void) { return s_pan_width; }

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
