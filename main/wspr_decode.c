#include "wspr_decode.h"
#include "wspr_metric_table.h"
#include "wspr_subtract.h"   /* wspr_tones_from_message() - the re-encode check */
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* Phase timing only. This file is deliberately free of ESP dependencies so the
 * host harnesses can link the real decoder, hence the guard: on the device the
 * microsecond timer, on a host whatever clock() offers. Nothing else here may
 * follow this precedent without the same justification. */
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
static int64_t wspr_now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
static int64_t wspr_now_us(void) {
    return (int64_t)((double)clock() * (1000000.0 / (double)CLOCKS_PER_SEC));
}
#endif

#include "fft/kiss_fftr.h" /* ft8_lib's component INCLUDE_DIRS is its own root
                             ("."), not fft/ specifically - matters once this
                             file is built as part of the real firmware
                             (main/CMakeLists.txt REQUIRES ft8_lib), not just
                             host test builds that pass -I .../ft8_lib/fft
                             directly. */

/* ⛔ THE TONE POWERS ARE FLOAT, AND ON THIS BOARD THAT IS NOT A MICRO-
 * OPTIMISATION. The ESP32-P4 has a SINGLE-PRECISION FPU, so every `double`
 * operation is a software-library call - mix_decimate() below carries the same
 * warning on its inner loop, and CLAUDE.md records 80 s versus 2.8 s for one
 * synthesis that differed only in this.
 *
 * This array is THE hot path: it is filled once per trial alignment, roughly
 * 160 times per candidate. Measured on hardware before this change, a
 * candidate cost 11.7 s - and a candidate rejected by the sync gate, which
 * runs no Fano search at all, still cost 11.7 s. So essentially the whole
 * decode budget is spent right here.
 *
 * Precision is not at risk: these are power values consumed only as RATIOS -
 * a sync correlation normalised by total amplitude, a per-symbol difference
 * normalised by the capture's own spread. Float carries about seven digits and
 * nothing downstream asks for more. The per-decode arithmetic that genuinely
 * wants double (the metric scaling, the agreement normaliser) still uses it,
 * because it runs once per attempt rather than 160 times. */
typedef float wspr_tp_t;

/* 10*log10(TONE_SPACING / 2500) - the fixed conversion from a one-tone-bin
 * SNR to WSPR's 2500 Hz reference bandwidth. */
#define WSPR_SNR_BW_OFFSET_DB  (-32.32)

#define TONE_SPACING (WSPR_SAMPLE_RATE_HZ / WSPR_SYM_LEN_SAMPLES) /* 1.46484375 Hz */

int wspr_find_candidates(const int16_t *samples, long n, double f_lo_hz,
                          double f_hi_hz, wspr_freq_candidate_t *out,
                          int max_out)
{
    if (n <= 0 || max_out <= 0) return 0;

    /* AVERAGED PERIODOGRAM, not one FFT over the whole capture.
     *
     * This used to set nfft = n. For a 120 s capture that is a 1,440,000-point
     * real FFT, and between kiss_fftr's own twiddles, its inner complex config,
     * `in` and `spec` it asks for roughly 23 MB - so on the device
     * kiss_fftr_alloc() simply returned NULL and this function reported "0
     * candidates" in 0 ms. That is what the first on-device self-test hit.
     *
     * Averaging |X|^2 over overlapping windows is the standard answer and is
     * better here on three counts, not merely cheaper: bounded memory (~2 MB),
     * a smoother noise estimate, and cache locality.
     *
     * WINDOW LENGTH IS CHOSEN, NOT ARBITRARY. A power-of-two MULTIPLE of the
     * symbol length makes WSPR's 1.4648 Hz tone spacing land on an exact whole
     * number of bins: tone_step_bins = nfft / WSPR_SYM_LEN_SAMPLES, with no
     * rounding error in the comb at all. 16 x 8192 = 131072 gives 0.0916 Hz
     * bins and a tone step of exactly 16.
     *
     * Frequency precision: the reported peak is quantised to 0.0916 Hz instead
     * of the old 0.0083 Hz. Worst-case error is then 0.046 Hz, which over one
     * 0.6827 s symbol is 0.03 of a cycle - far inside the ~1.46 Hz sinc null,
     * i.e. well under 0.1 dB of correlation loss.
     */
    int nfft = 16 * WSPR_SYM_LEN_SAMPLES;      /* 131072 */
    while ((long)nfft > n && nfft > WSPR_SYM_LEN_SAMPLES) nfft /= 2;
    if ((long)nfft > n) return 0;              /* capture shorter than one symbol */

    kiss_fftr_cfg cfg = kiss_fftr_alloc(nfft, 0, NULL, NULL);
    if (!cfg) return 0;
    kiss_fft_scalar *in = (kiss_fft_scalar *)malloc((size_t)nfft * sizeof(kiss_fft_scalar));
    kiss_fft_cpx *spec = (kiss_fft_cpx *)malloc((size_t)(nfft / 2 + 1) * sizeof(kiss_fft_cpx));
    int nbins = nfft / 2 + 1;
    float *mag = (float *)calloc((size_t)nbins, sizeof(float));
    if (!in || !spec || !mag) {
        free(in); free(spec); free(mag); free(cfg);
        return 0;
    }

    /* 50 % overlap: every sample outside the first and last half-window is
     * covered twice, so a transmission straddling a window boundary is not
     * penalised. */
    const long step = nfft / 2;
    int nwin = 0;
    for (long off = 0; off + nfft <= n; off += step) {
        for (int i = 0; i < nfft; i++)
            in[i] = (kiss_fft_scalar)(samples[off + i] / 32768.0);
        kiss_fftr(cfg, in, spec);
        for (int b = 0; b < nbins; b++)
            mag[b] += spec[b].r * spec[b].r + spec[b].i * spec[b].i;
        nwin++;
    }
    if (nwin == 0) { free(in); free(spec); free(mag); free(cfg); return 0; }

    double bin_hz = WSPR_SAMPLE_RATE_HZ / nfft;
    int lo_bin = (int)(f_lo_hz / bin_hz), hi_bin = (int)(f_hi_hz / bin_hz);
    if (hi_bin > nbins) hi_bin = nbins;
    if (lo_bin < 0) lo_bin = 0;
    int tone_step_bins = nfft / WSPR_SYM_LEN_SAMPLES;   /* exact, by construction */

    int nscore = hi_bin - lo_bin;
    int count = 0;
    if (nscore > 0) {
        float *score = (float *)calloc((size_t)nscore, sizeof(float));
        for (int b = lo_bin; b < hi_bin; b++) {
            double s = 0;
            for (int k = 0; k < 4; k++) {
                int bb = b + k * tone_step_bins;
                if (bb < nbins) s += mag[bb];
            }
            score[b - lo_bin] = (float)s;
        }
        for (int c = 0; c < max_out; c++) {
            int best = -1;
            float bestv = -1;
            for (int i = 0; i < nscore; i++) {
                if (score[i] > bestv) { bestv = score[i]; best = i; }
            }
            if (best < 0 || bestv <= 0) break;
            out[count].freq_hz = (best + lo_bin) * bin_hz;
            out[count].comb_score = bestv;
            count++;
            /* ⛔ THIS USED TO BLANK +/-4 TONE SPACINGS (5.9 Hz) AROUND EVERY
             * PEAK, WHICH MADE WHOLE STATIONS UNREACHABLE - not merely
             * unranked, unreachable, because a frequency that is never a
             * candidate is never tried. Measured on the 19:10 reference
             * window: DK8AF (1521.8 Hz) and DD3MS (1525.8 Hz) are 4 Hz apart
             * and shared one candidate at 1524.6, where NEITHER decodes;
             * handed their own frequencies, both decode cleanly (agreement
             * 0.76 and 0.84). Three more stations shared a single candidate
             * 2 Hz away from each of them.
             *
             * The wide blanking was not arbitrary - it exists because the comb
             * score has SIDELOBES. The comb sums four bins one tone spacing
             * apart, so sliding it by k tone spacings still lands 4-|k| teeth
             * on a real signal, and one station would otherwise be reported up
             * to seven times. But that is a set of SPIKES at known offsets,
             * not a 12 Hz-wide plateau, so blank the spikes and leave the gaps
             * between them - which is where a genuine neighbour lives.
             *
             * Anything closer than about a tone spacing is genuinely the same
             * signal, so the k=0 case still blanks a small neighbourhood. */
            for (int k = -3; k <= 3; k++) {
                int c0 = best + k * tone_step_bins;
#ifndef WSPR_SIDELOBE_DIV
#define WSPR_SIDELOBE_DIV 4
#endif
                int half = (k == 0) ? tone_step_bins
                                    : (tone_step_bins / WSPR_SIDELOBE_DIV);
                for (int i = c0 - half; i <= c0 + half; i++)
                    if (i >= 0 && i < nscore) score[i] = -1;
            }
        }
        free(score);
    }

    free(mag);
    free(spec);
    free(in);
    free(cfg);
    return count;
}

/* ---------------------------------------------------------------------------
 * DECIMATING FRONT END
 *
 * The correlator used to run at the full 12 kHz sample rate, reading 162
 * symbols x 4 tones x 8192 samples - about 5.3 M values - for EVERY start-time
 * offset tried, ~119 of them per candidate. On a host that is merely
 * inefficient. On the P4 the capture only fits in PSRAM, so the loop is
 * memory-bound on ~630 M PSRAM reads per candidate and it MEASURED 67 SECONDS
 * per candidate against a 120 s cycle budget - 456 % for eight candidates, and
 * still 120 % for two, so trimming the candidate list could not have saved it.
 *
 * WSPR occupies about 6 Hz. Carrying 12 kHz of bandwidth through the inner loop
 * to look at 6 Hz of signal is the whole waste, and every real WSPR decoder
 * (wsprd included) does the same thing about it: mix the candidate down to
 * complex baseband once, decimate hard, and correlate on the small array.
 *
 * At WSPR_DECIM = 32 the rate becomes 375 Hz and a symbol is 256 samples
 * instead of 8192, so extract_tone_powers() reads ~166 K values instead of
 * 5.3 M. The mixing pass itself is one sweep of the capture per candidate,
 * negligible against the 630 M reads it removes.
 * ------------------------------------------------------------------------- */

/* Cost meter for the host benchmarks, compiled out of the firmware entirely.
 *
 * Wall-clock on the build host cannot measure this board - it has a hardware
 * double FPU and fast RAM, and CLAUDE.md records that mis-measurement costing
 * two orders of magnitude. A COUNT of tone-power correlations is the same
 * number on both machines, so it is what the sweep harness optimises against.
 * The device still reports real milliseconds per phase; the two agree on which
 * way a change moved, which is all a host measurement is entitled to claim. */
#ifdef WSPR_PROFILE_CORR
/* Counted in FULL-RATE-EQUIVALENT correlations: a strided one does 1/stride of
 * the reads, so counting calls would score a cheaper scan as free. */
double wspr_corr_work = 0;
#define WSPR_CORR_TICK(stride) (wspr_corr_work += 1.0 / (double)(stride))
#else
#define WSPR_CORR_TICK(stride) ((void)0)
#endif

#define WSPR_DECIM        32
#define WSPR_DEC_RATE_HZ  (WSPR_SAMPLE_RATE_HZ / WSPR_DECIM)     /* 375 Hz */
#define WSPR_DEC_SPS      (WSPR_SYM_LEN_SAMPLES / WSPR_DECIM)    /* 256    */

typedef struct {
    float *i;      /* complex baseband, decimated */
    float *q;
    long   n;      /* samples in i/q */
} baseband_t;

static void free_baseband(baseband_t *bb)
{
    free(bb->i); free(bb->q);
    bb->i = bb->q = NULL; bb->n = 0;
}

/* Mix `samples` down by f0 and decimate by WSPR_DECIM with a boxcar average.
 *
 * The local oscillator is an incremental complex rotation rather than a
 * cos()/sin() per sample: 1.44 M libm calls would cost more than the work being
 * saved. It is renormalised periodically because repeated complex multiplies
 * drift in magnitude - without that, the tail of a 120 s capture would be
 * scaled differently from its head, which is exactly the kind of slow error
 * that looks like fading and would be blamed on the ionosphere.
 */
/* ---- ANTI-ALIASING LOW-PASS TAPS -------------------------------------
 *
 * ⛔ THIS REPLACED A SINGLE 32-TAP BOXCAR AVERAGE, WHICH WAS THE ONLY
 * ANTI-ALIASING THIS DECODER HAD.
 *
 * A boxcar is a dreadful low-pass: first sidelobe only -13 dB. At WSPR_DECIM 32
 * the decimated band is +/-187.5 Hz, so every decode admitted everything within
 * 187 Hz at nearly full strength and folded in whatever lay beyond. On a WSPR
 * band carrying stations every 10-20 Hz across a 200 Hz window, that means
 * every decode was contaminated by every other station present.
 *
 * Found from two anomalies that no narrowband effect could explain: subtracting
 * G8MCD at 1407 Hz destroyed a decode of F6APU at 1441 Hz (34 Hz away, inside
 * the band), and subtracting a fabricated signal at 1400.94 Hz was what made
 * PA2PGU at 1589.72 Hz decodable at all (188.8 Hz away - just outside the band,
 * so aliased straight back in).
 *
 * A WSPR transmission is ~6 Hz wide (4 tones x 1.4648 Hz) and f0 is known to
 * ~0.1 Hz, so a cutoff of 50 Hz is enormously generous to the signal while
 * rejecting the neighbours. Windowed sinc, built once. */
/* ⭐ 160 TAPS AT 100 Hz, NOT 256 AT 50 - MEASURED ON THE REFERENCE FILES,
 * NOT DESIGNED ON PAPER AND ASSUMED.
 *
 * This filter is ~90 % of mix_decimate, which the device measures at ~1060 ms
 * per candidate, so its length is the single biggest fixed cost in the
 * receiver. 256 taps at a 50 Hz cutoff was chosen to be "enormously generous
 * to the signal", which it is - but generous in the wrong direction, because
 * it was never sized against what the filter actually has to reject.
 *
 * WHAT IT HAS TO REJECT, exactly: decimating 12000 -> 375 Hz folds input
 * frequency f onto f mod 375. The decoder only ever READS +/-34 Hz of the
 * result - the four tones live within +/-4.4 Hz, the frequency search adds
 * +/-1.5, and measure_noise_ref samples out to +/-34. So the only content
 * that can contaminate a decode is what lands in +/-34 Hz, i.e. the input
 * bands k*375 +/- 34 for k >= 1: 341-409 Hz, 716-784 Hz, and so on to
 * Nyquist. Everything from ~40 Hz to ~341 Hz aliases somewhere in the
 * decimated band that NOTHING READS. That slack is what the old design was
 * paying for and not using.
 *
 * Computed over every folding band to 6 kHz (scratchpad/lpf_design.py mirrors
 * build_lpf() exactly):
 *
 *     taps  cut   droop at 34 Hz   worst alias   cost   stations found
 *      256   50        -2.17 dB      -65.5 dB    1.00x   23
 *      160  100        -0.58 dB      -53.6 dB    0.62x   24  (+5B4AHZ)
 *      152  100        -0.58 dB      -55.9 dB    0.59x   24
 *      144  100        -0.57 dB      -58.0 dB    0.56x   23  (-PA2PGU)
 *
 * ⛔ AND THAT LAST COLUMN IS WHY THIS WAS SWEPT RATHER THAN CALCULATED. On
 * paper 128 taps at 100 Hz is STRICTLY BETTER than 256 at 50 - less droop AND
 * better alias rejection at half the cost - and it silently trades PA2PGU for
 * 5B4AHZ. A filter's response curve does not predict which marginal stations
 * survive it, because a marginal decode depends on where the aliases land
 * relative to that particular signal. CLAUDE.md already records PA2PGU's
 * decode turning on aliasing from 188 Hz away; it is the canary here.
 *
 * 152 is the cliff: at 144 PA2PGU goes. 160 is deliberately ONE STEP BACK from
 * it, because 5 % of a filter is a poor price for standing on an edge that
 * some unrelated front-end change could push us over.
 *
 * The gain is real in both directions: 38 % less work, and FOUR TIMES LESS
 * PASSBAND DROOP where the noise reference is sampled - the old filter was
 * quietly attenuating the +/-10..34 Hz noise samples by up to 2.2 dB, biasing
 * the measured floor DOWN and every reported SNR up. Reported SNRs drop by
 * about 1 dB as a result, which is the more honest number.
 *
 * ⚠ The +/-34 Hz figure is a CONTRACT WITH measure_noise_ref(). If its offset
 * table ever reaches further out, this filter has to be re-checked against the
 * new number - the analysis above is only valid for what the decoder reads. */
/* ⛔ THE RING IS A POWER OF TWO; THE TAP COUNT NEED NOT BE, AND USED TO BE
 * FORCED TO MATCH IT. The ring index is masked (`& (LPF_RING - 1)`) because a
 * `% 255` here was measured at ~40 s per candidate - an integer division per
 * tap, 11.5 M times. That constraint belongs to the RING, not to the filter
 * length, and tying them together left only 256 or 64 reachable when the right
 * answer was in between. Keep LPF_RING a power of two and >= LPF_TAPS. */
#ifndef LPF_TAPS
#define LPF_TAPS   160
#endif
#define LPF_RING   256
#ifndef LPF_CUT_HZ
#define LPF_CUT_HZ 100.0
#endif

static float  s_lpf[LPF_TAPS];
static int    s_lpf_ready;

static void build_lpf(void)
{
    if (s_lpf_ready) return;
    const double fc = LPF_CUT_HZ / WSPR_SAMPLE_RATE_HZ;   /* cycles/sample */
    const int    M  = LPF_TAPS / 2;
    double sum = 0.0;
    for (int k = 0; k < LPF_TAPS; k++) {
        const int    m = k - M;
        const double sinc = (m == 0) ? 2.0 * fc
                                     : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        /* Hamming: -43 dB sidelobes, which is what the boxcar's -13 dB was
         * costing us. */
        const double win = 0.54 - 0.46 * cos(2.0 * M_PI * k / (double)(LPF_TAPS - 1));
        s_lpf[k] = (float)(sinc * win);
        sum += s_lpf[k];
    }
    if (sum != 0.0) for (int k = 0; k < LPF_TAPS; k++) s_lpf[k] /= (float)sum;
    s_lpf_ready = 1;
}

static int mix_decimate(const int16_t *samples, long n, double f0, baseband_t *bb)
{
    memset(bb, 0, sizeof(*bb));
    long out_n = n / WSPR_DECIM;
    if (out_n <= 0) return 0;
    bb->i = (float *)malloc((size_t)out_n * sizeof(float));
    bb->q = (float *)malloc((size_t)out_n * sizeof(float));
    if (!bb->i || !bb->q) { free_baseband(bb); return 0; }
    bb->n = out_n;

    build_lpf();

    /* ⛔ THE LOCAL OSCILLATOR RUNS IN FLOAT, RE-SEEDED EXACTLY IN DOUBLE.
     *
     * The recurrence advances once per INPUT sample - 1.44 M of them per
     * candidate - and in double that is four software multiplies and two
     * software adds each, on a chip whose FPU is single-precision only. It was
     * the last double left on the per-input-sample path; everything else here
     * was moved to float long ago for exactly this reason (see the FLOAT, NOT
     * DOUBLE note on the filter loop below).
     *
     * Float alone would not be safe: a rotation applied 1.44 M times
     * accumulates PHASE error, and the old code's periodic renormalise only
     * fixed MAGNITUDE - it could not have caught a drifting angle. So instead
     * of renormalising, the oscillator is now recomputed from the exact phase
     * every LO_RESEED samples. Error can then accumulate over at most 1024
     * steps (~1e-4 rad, far below anything a tone correlation notices) and is
     * wiped rather than merely rescaled, which makes this strictly more
     * accurate than what it replaces as well as faster. */
#define LO_RESEED 1024
    const double w = 2.0 * M_PI * f0 / WSPR_SAMPLE_RATE_HZ;
    const float cs = (float)cos(-w), sn = (float)sin(-w);   /* step: e^(-j*w) */
    float ore = 1.0f, oim = 0.0f;

    /* Only the last LPF_TAPS mixed samples are ever needed, so this is a small
     * ring rather than an 11 MB copy of the whole mixed capture. Outputs are
     * computed 1-in-32, i.e. polyphase by hand: the filter cost falls on the
     * 45000 outputs, not on the 1.44 M inputs. */
    /* ⛔ FILE-SCOPE STATIC, SO wspr_decode_candidate() MUST NOT BE CALLED FROM
     * TWO TASKS AT ONCE. It is not today - the ping-pong runs capture and
     * decode concurrently but there is exactly one decode task - and that is
     * precisely why this warning is here, because the architecture now LOOKS
     * like it could. Kept static rather than local because 2 KB on a task that
     * already reserves a 16 KB frame is not free, and kept internal rather than
     * PSRAM because the inner loop reads it ~11.5 M times per candidate. */
    static float hi[LPF_RING], hq[LPF_RING];
    memset(hi, 0, sizeof(hi));
    memset(hq, 0, sizeof(hq));
    int hp = 0;

    long o = 0;
    int cnt = 0;
    for (long idx = 0; idx < out_n * WSPR_DECIM; idx++) {
        const float x = samples[idx] * (1.0f / 32768.0f);
        hi[hp] = x * ore;
        hq[hp] = x * oim;
        hp = (hp + 1) & (LPF_RING - 1);

        if ((idx & (LO_RESEED - 1)) == (LO_RESEED - 1)) {
            /* Exact re-seed, not a rescale - see the note above. */
            const double ph = -w * (double)(idx + 1);
            ore = (float)cos(ph);
            oim = (float)sin(ph);
        } else {
            const float nre = ore * cs - oim * sn;
            const float nim = ore * sn + oim * cs;
            ore = nre; oim = nim;
        }

        if (++cnt == WSPR_DECIM) {
            /* ⛔ FLOAT, NOT DOUBLE, AND A MASK, NOT A MODULO.
             *
             * This loop runs LPF_TAPS x 45000 = 11.5 M times per candidate, so
             * both details are the difference between a working receiver and a
             * broken one - measured on hardware, not guessed:
             *
             *   double accumulators + `% 255`  ->  ~40 s per candidate, 3 of 20
             *                                      tried, decode yield destroyed
             *   float accumulators + `& 255`   ->  see the commit message
             *
             * The ESP32-P4 has a SINGLE-PRECISION FPU, so `double` is a
             * software library here (CLAUDE.md records 80 s vs 2.8 s for the
             * same synthesis). And 255 is not a power of two, so the ring wrap
             * was an integer division per tap. The host showed +5 % for the
             * double version and told me nothing, because it has neither
             * problem. */
            float ai = 0.0f, aq = 0.0f;
            /* Oldest of the LPF_TAPS samples still in the ring. With
             * LPF_TAPS == LPF_RING this is just hp; with a shorter filter the
             * read has to start LPF_TAPS behind the write pointer, or the
             * coefficients line up against the wrong samples. */
            int r = (hp + LPF_RING - LPF_TAPS) & (LPF_RING - 1);
            for (int k = 0; k < LPF_TAPS; k++) {
                ai += s_lpf[k] * hi[r];
                aq += s_lpf[k] * hq[r];
                r = (r + 1) & (LPF_RING - 1);
            }
            if (o < out_n) { bb->i[o] = ai; bb->q[o] = aq; o++; }
            cnt = 0;
        }
    }
    return 1;
}

/* Tone twiddles at the DECIMATED rate: one symbol long (256), four tones.
 * 8 KB in total, against the 46 MB the full-rate absolute-index tables wanted. */
typedef struct {
    /* ⛔ INDEXED [sample][tone], NOT [tone][sample]. The transpose is not
     * cosmetic: extract_tone_powers() walks all four tones at one sample
     * before moving on, so this order makes its inner four reads adjacent.
     * Laid out the other way they are 1 KB apart and every symbol touches
     * eight cache lines instead of two. */
    float cos_tab[WSPR_DEC_SPS][4];
    float sin_tab[WSPR_DEC_SPS][4];
} tone_tw_t;

/* `df_hz` shifts all four tones together - the fine frequency search.
 *
 * It is legitimate to fold a frequency offset into the tone table and ignore
 * the phase discontinuity it leaves between symbols, because every consumer
 * takes the MAGNITUDE of the per-symbol correlation. A constant phase error
 * per symbol is invisible to |.|^2. That is what makes the frequency search
 * cost 1024 cos/sin per trial instead of another sweep of the whole capture. */
/* ⛔ 2048 DOUBLE TRIG CALLS PER BUILD, AND IT IS CALLED 32 TIMES PER
 * CANDIDATE - the same trap that cost wspr_subtract 67 s per signal, in a
 * function nobody had looked at because it only fills a small table.
 *
 * The ESP32-P4's FPU is single-precision, so every cos()/sin() here was a
 * software-library call. Accounted from the device's own phase timings: the
 * `curve` phase reported ~1290 ms while running only 30 correlations worth
 * ~520 ms, and the missing ~770 ms over 30 builds works out at ~26 ms each,
 * i.e. ~12.7 us per double trig call - which is the SAME per-call figure
 * wspr_subtract measured. Two independent paths agreeing on that number is
 * what makes this an accounting result rather than a guess.
 *
 * A complex phasor recurrence needs two trig calls instead of 512 per tone.
 * It is re-seeded exactly every RESEED samples rather than renormalised, for
 * the reason mix_decimate's oscillator note gives at length: a rescale fixes
 * magnitude drift and cannot touch PHASE drift, and phase is what a tone
 * correlation actually reads. Over 64 float steps the accumulated angle error
 * is ~1e-6 rad, some four orders below the 0.2 Hz the frequency search
 * resolves. */
static void build_tone_tw(tone_tw_t *tw, double df_hz)
{
#define TW_RESEED 64
    for (int k = 0; k < 4; k++) {
        const double w = 2.0 * M_PI * (k * TONE_SPACING + df_hz) / WSPR_DEC_RATE_HZ;
        const float cs = (float)cos(w), sn = (float)sin(w);
        float x = 1.0f, y = 0.0f;
        for (int j = 0; j < WSPR_DEC_SPS; j++) {
            if ((j & (TW_RESEED - 1)) == 0 && j != 0) {
                const double ph = w * (double)j;
                x = (float)cos(ph); y = (float)sin(ph);
            }
            tw->cos_tab[j][k] = x;
            tw->sin_tab[j][k] = y;
            const float nx = x * cs - y * sn;
            const float ny = x * sn + y * cs;
            x = nx; y = ny;
        }
    }
}

/* Correlate each symbol against each of the 4 tones, on the decimated complex
 * baseband. start_dec is the transmission start in DECIMATED samples.
 *
 * As before only the magnitude is kept, so the per-symbol phase of the local
 * oscillator cancels and the tables can be indexed by the offset within the
 * symbol. */
/* `stride` reads every Nth sample of the symbol instead of all 256.
 *
 * It exists for the coarse start-time scan, which is the single largest cost
 * in the receiver (~120 correlations per candidate, measured at ~2070 ms on
 * the device) and which only has to find the right 1/8th of a symbol - the
 * fine refinement afterwards does the precision work at full rate.
 *
 * ⚠ SUB-SAMPLING WITHOUT A PRE-FILTER IS NORMALLY AN ALIASING BUG. It is safe
 * HERE, and only here, because mix_decimate has already low-passed this
 * baseband at 50 Hz with a 43 dB stopband: at stride 4 the folding frequency
 * is 46.9 Hz, so everything that would alias is already down in that
 * stopband. If LPF_CUT_HZ is ever raised, this assumption has to be re-checked
 * - it is the filter, not the arithmetic, that makes the shortcut legal.
 *
 * The tone powers come out ~1/16 of their full-rate value, which does not
 * matter to the only consumer: sync_score() is normalised by total power, so
 * it is scale-free. A strided score is NOT comparable to a full-rate one
 * though, and the caller must not mix them - see the re-score at full rate
 * after the coarse scan. */
static void extract_tone_powers_s(const baseband_t *bb, const tone_tw_t *tw,
                                   long start_dec, wspr_tp_t tone_power[WSPR_NSYM][4],
                                   int stride)
{
    WSPR_CORR_TICK(stride);
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        long base = start_dec + (long)sym * WSPR_DEC_SPS;
        long j0 = 0, j1 = WSPR_DEC_SPS;
        if (base < 0)               j0 = -base;
        if (base + j1 > bb->n)      j1 = bb->n - base;
        /* ⛔ SAMPLE OUTER, TONE INNER - AND THE FOUR ACCUMULATORS ARE WHY IT
         * IS ALLOWED TO BE.
         *
         * This is the hottest function in the receiver: ~150 calls per
         * candidate, each 162 x 4 x 256 multiply-accumulates. Written with the
         * tone loop outside, it re-reads the WHOLE baseband once per tone -
         * and the baseband is 360 KB of malloc'd float, which on this board
         * means PSRAM (CLAUDE.md: allocations at or above 16 KB spill there).
         * So three quarters of the PSRAM traffic in the decoder was re-reading
         * samples it had just read.
         *
         * Keeping a separate accumulator pair per tone makes the reordering
         * EXACT rather than merely close: each accumulator still sums the same
         * products in the same j order, so every result is bit-identical to
         * what the tone-outer loop produced. That is the whole reason to do it
         * this way instead of a single running sum - a change to the hottest
         * path of a decoder with no CRC has to be provably output-preserving,
         * or the agreement scores shift underneath the thresholds tuned to
         * them. Verified: the four reference WAVs decode the same 23 stations
         * with the same Fano cycle counts and the same agreement figures. */
        float re0 = 0, re1 = 0, re2 = 0, re3 = 0;
        float im0 = 0, im1 = 0, im2 = 0, im3 = 0;
        /* ⚠ Indexed as bb->i[base + j], NOT hoisted to a `bb->i + base`
         * pointer. The clamp above contemplates a NEGATIVE base, and forming a
         * pointer before the start of an array is undefined behaviour even
         * when it is never dereferenced out of range. Today start_dec is
         * always >= 0 so it cannot happen - but the guard is right there,
         * which means someone eventually makes it happen. The hoist bought
         * nothing anyway: the win here is the loop ORDER. */
        for (long j = j0; j < j1; j += stride) {
            const float xi = bb->i[base + j], xq = bb->q[base + j];
            const float *c = tw->cos_tab[j], *st = tw->sin_tab[j];
            /* (xi + j*xq) * e^(-j*w*j) */
            re0 += xi * c[0] + xq * st[0];  im0 += xq * c[0] - xi * st[0];
            re1 += xi * c[1] + xq * st[1];  im1 += xq * c[1] - xi * st[1];
            re2 += xi * c[2] + xq * st[2];  im2 += xq * c[2] - xi * st[2];
            re3 += xi * c[3] + xq * st[3];  im3 += xq * c[3] - xi * st[3];
        }
        tone_power[sym][0] = re0 * re0 + im0 * im0;
        tone_power[sym][1] = re1 * re1 + im1 * im1;
        tone_power[sym][2] = re2 * re2 + im2 * im2;
        tone_power[sym][3] = re3 * re3 + im3 * im3;
    }
}

/* The full-rate correlation every accuracy-critical caller wants. */
static void extract_tone_powers(const baseband_t *bb, const tone_tw_t *tw,
                                 long start_dec, wspr_tp_t tone_power[WSPR_NSYM][4])
{
    extract_tone_powers_s(bb, tw, start_dec, tone_power, 1);
}

/* sync=1 tones are {1,3} (odd tone index), sync=0 tones are {0,2} - the
 * higher this is, the better the (f0, dt) alignment matches WSPR's known
 * 162-bit sync vector. */
/* ⛔ THIS USED TO BE AN UNNORMALISED SUM OF POWERS, AND BOTH HALVES OF THAT
 * WERE WRONG in a way that quietly mis-aimed every decode.
 *
 * UNNORMALISED: the search that picks the start time maximises this, so with
 * total energy left in it the winner was partly "whichever alignment caught
 * the most power" - a strong neighbour sliding into the window scores well
 * without being in sync at all. Dividing by the total makes the metric a pure
 * correlation in [-1, 1], answering "how well does this alignment match the
 * sync vector" and nothing else. It is also then comparable ACROSS candidates
 * and across frequency trials, which the frequency refinement below needs.
 *
 * POWER, NOT AMPLITUDE: squaring hands the sum to the loudest few symbols. On
 * a fading signal - the normal case on HF - those are whichever symbols
 * happened to arrive during a peak, so the alignment gets chosen by a fraction
 * of the transmission. wsprd sums amplitudes here for the same reason.
 *
 * Both changes together are what let the frequency search work at all: the old
 * metric grew with any extra energy admitted, so the best "frequency" was
 * simply wherever the most noise sat. */
static double sync_score(wspr_tp_t tone_power[WSPR_NSYM][4])
{
    /* ⛔ sqrtF, NOT sqrt, AND FLOAT ACCUMULATORS. This function is called once
     * per trial alignment - roughly 160 times per candidate - and each call
     * takes 648 square roots, so it runs about 100,000 times per candidate.
     * The ESP32-P4 has a SINGLE-PRECISION FPU, so every `double` operation
     * here is a software-library call (CLAUDE.md records 80 s vs 2.8 s for the
     * same synthesis, and the decimation filter has the identical warning on
     * its inner loop). The result is a ratio used only to rank alignments;
     * float carries far more precision than that comparison needs. */
    float s = 0, tot = 0;
    for (int i = 0; i < WSPR_NSYM; i++) {
        float a0 = sqrtf((float)tone_power[i][0]), a1 = sqrtf((float)tone_power[i][1]);
        float a2 = sqrtf((float)tone_power[i][2]), a3 = sqrtf((float)tone_power[i][3]);
        float favor1 = (a1 + a3) - (a0 + a2);
        s += wspr_sync_vector[i] ? favor1 : -favor1;
        tot += a0 + a1 + a2 + a3;
    }
    return (tot > 0) ? (double)(s / tot) : -1.0;
}

static int is_legal_power(int dbm)
{
    static const int legal[] = { 0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37,
                                  40, 43, 47, 50, 53, 57, 60 };
    for (size_t i = 0; i < sizeof(legal) / sizeof(legal[0]); i++) {
        if (legal[i] == dbm) return 1;
    }
    return 0;
}

/* A clean decode converges fast — every genuine signal found in the
 * reference WAV (see wspr_decode.h) took 82-102 Fano cycles; the file's
 * own strongest candidate, which fails to decode plausibly (cause not
 * confirmed - frequency drift was tried and did NOT explain it, see
 * docs/wspr-phase1-status.md), took 49400. Picked with real margin: ~20x
 * the clean cluster's ceiling, far below any observed false-decode cycle
 * count. */
#define WSPR_CYCLES_SUSPECT 2000

/* ⛔ LEGAL IS NOT THE SAME AS PLAUSIBLE. The protocol allows 0..60 dBm, and
 * is_legal_power() only checks the 3 dB quantisation - so a garbage decode
 * claiming 1 kW passes.
 *
 * MEASURED over a 7 h 20 m run on 40 m, 615 decodes, 141 unique calls:
 *
 *   - 75 calls were heard REPEATEDLY. A real station transmits again; a
 *     fabrication does not, so repetition is ground truth that needs no
 *     external decoder. NOT ONE of those 75 exceeded 40 dBm.
 *   - 10 calls claimed 47-60 dBm. EVERY ONE was heard exactly once.
 *
 * Real <= 40 dBm and fabricated >= 47 dBm, with a clean gap between. 43 sits in
 * the middle with margin on both sides, so this removed all 10 fabrications and
 * cost ZERO of the 615 real decodes.
 *
 * ⚠ It does reject a LEGAL value, deliberately. 43 dBm is 20 W on a mode built
 * for milliwatts, and a WSPR spot is a reception report: publishing a station
 * that was never on the air is worse than missing a rare high-power one. Same
 * reasoning as the ADIF "599" that was deleted and the PSK Reporter callsign
 * rules.
 *
 * ⚠ One night, one band. If a real >43 dBm station is ever confirmed, raise
 * this rather than deleting it - the gap is what matters, not the number. */
#define WSPR_PLAUSIBLE_MAX_DBM 43

/* ⛔ A CALLSIGN CAN SATISFY WSPR'S ENCODING AND STILL BE IMPOSSIBLE.
 *
 * The 28-bit field only demands a digit in the third character slot, so
 * `0C0RCS` packs, unpacks and REPACKS perfectly - it passed every check here
 * and was reported as a received station. wsprd, given the same audio, reports
 * no such call. A WSPR spot is a reception report, so publishing that is a
 * fabricated measurement, which is the one thing this project refuses
 * everywhere else (see the deleted ADIF "599" and the PSK Reporter rules).
 *
 * The rule is ITU Article 19: a callsign's first character is a letter, or a
 * digit 2-9. **`0` and `1` are never allocated.**
 *
 * ⚠ IT MUST BE EXACTLY THAT AND NO BROADER. Leading digits in general are
 * perfectly legitimate - `2E0DLC` is in wsprd's own 19:06 list, and 3DA0, 4X,
 * 5B, 8P and 9A are all real prefixes. Rejecting "starts with a digit" would
 * throw away real stations to catch a fake one, which is a worse trade than
 * the bug.
 *
 * Verified against all four reference windows: rejects 0C0RCS, keeps every
 * wsprd-confirmed decode including 2E0DLC.
 *
 * ⚠ This does NOT catch every fabrication. `E48XFU` (also absent from wsprd)
 * has a legal shape - E4 is Palestine, so prefix + area digit + 3-letter suffix
 * is structurally fine - and no shape test can reject it without rejecting real
 * calls. Its tell is elsewhere: it needed 1987 Fano cycles where every
 * wsprd-confirmed decode across these files converged in <= 453. That is a
 * threshold judgement on thin data, so it is deliberately left alone here. */
static int callsign_shape_ok(const char *call)
{
    if (!call || !call[0]) return 0;
    if (call[0] == '0' || call[0] == '1') return 0;
    /* A real call has both: all-letters or all-digits is not a callsign. */
    int letters = 0, digits = 0;
    for (const char *c = call; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') letters++;
        else if (*c >= '0' && *c <= '9') digits++;
        else if (*c != '/') return 0;      /* nothing else belongs in one */
    }
    return letters > 0 && digits > 0;
}

/* Runs the three shared plausibility checks (message shape, legal power
 * quantization, Fano convergence speed - see wspr_decode.h) and fills
 * *result if a decoded message passes all three. Shared by both decode
 * attempts below so the two can't silently apply different standards. */
/* Re-encode the decoded message and score it against the audio we received.
 * See wspr_decode_result_t for why this is the only check that can catch a
 * wrong codeword. Fills result->agree_hard / agree_soft. */
static void score_agreement(const char *call, const char *grid, int dbm,
                             wspr_tp_t tp[WSPR_NSYM][4], double noise_ref,
                             wspr_decode_result_t *result)
{
    result->agree_hard = 0.0f;
    result->agree_soft = 0.0f;
    uint8_t tones[WSPR_NSYM];
    if (!wspr_tones_from_message(call, grid, dbm, tones)) return;

    /* The same soft symbol the decoder used, normalised the same way, so the
     * two numbers are directly comparable across captures and stations. */
    double d[WSPR_NSYM], sum = 0, sq = 0;
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double a1 = sqrt(sync ? tp[i][3] : tp[i][2]);
        double a0 = sqrt(sync ? tp[i][1] : tp[i][0]);
        d[i] = a1 - a0;
        sum += d[i]; sq += d[i] * d[i];
    }
    double mean = sum / WSPR_NSYM;
    double var  = sq / WSPR_NSYM - mean * mean;
    double sd   = (var > 1e-30) ? sqrt(var) : 1e-15;

    /* ---- SNR, MEASURED FROM THE DECODE WE JUST CONFIRMED ----------------
     *
     * Once the message is known, so is which of the four tones was transmitted
     * in every symbol - which turns each symbol into one signal sample and
     * THREE noise samples at exactly the same bandwidth, taken at exactly the
     * same time. That is a self-calibrating measurement: no noise-floor
     * estimate from elsewhere in the band, nothing assumed about the receiver
     * gain, and it cannot be fooled by a strong neighbour the way a
     * spectrum-percentile floor can.
     *
     * S is the mean power in the correct tone, N the mean over the other
     * three. (S - N) / N is the signal-to-noise ratio in ONE tone bin, whose
     * bandwidth is 1 / symbol period = 1.4648 Hz. WSPR quotes SNR in a 2500 Hz
     * reference bandwidth, so the conversion is a constant:
     *     10 * log10(1.4648 / 2500) = -32.3 dB
     *
     * ⚠ Validated against wsprd on the reference recordings rather than
     * asserted - see docs/wspr-phase3-sensitivity.md. An SNR is a published
     * measurement: it goes to wsprnet as a reception report, so it is exactly
     * the kind of number this project refuses to invent (CLAUDE.md, the RST
     * placeholder). If it ever cannot be measured it stays UNKNOWN. */
    double sig = 0;
    for (int i = 0; i < WSPR_NSYM; i++) sig += tp[i][tones[i]];
    sig /= WSPR_NSYM;
    const double noi = noise_ref;
    if (noi > 0 && sig > noi) {
        const double snr_bin = (sig - noi) / noi;
        const double db = 10.0 * log10(snr_bin) + WSPR_SNR_BW_OFFSET_DB;
        result->snr_db = (int16_t)lround(db < -40 ? -40 : (db > 30 ? 30 : db));
    } else {
        /* Buried in its own noise - report the floor of the scale, not a
         * positive number produced by a negative ratio. */
        result->snr_db = -40;
    }

    int agree = 0;
    double soft = 0;
    for (int i = 0; i < WSPR_NSYM; i++) {
        /* WSPR tone = sync bit + 2 * data bit, so the data bit is the high one. */
        int bit = tones[i] >> 1;
        double want = bit ? d[i] : -d[i];       /* positive when the audio agrees */
        if (want > 0) agree++;
        soft += want / sd;
    }
    result->agree_hard = (float)agree / (float)WSPR_NSYM;
    result->agree_soft = (float)(soft / WSPR_NSYM);
}

/* ---- THE NOISE REFERENCE, MEASURED WHERE THE SIGNAL IS NOT ---------------
 *
 * ⛔ THE OBVIOUS METHOD IS WRONG AND THE ERROR HIDES ITSELF. Once the message
 * is known, each symbol has one correct tone and three wrong ones, so the
 * wrong ones look like three free noise samples at exactly the right
 * bandwidth. Measured against wsprd on the reference recordings, that reads
 * 2-4 dB low on weak signals and TWENTY-THREE dB low on the strongest one -
 * and the giveaway is that the error GROWS WITH SIGNAL STRENGTH, which no
 * noise measurement should do.
 *
 * The reason is that WSPR is continuous-phase FSK: during every symbol
 * transition the tone is sweeping, so real signal energy lands in the other
 * three bins. For a weak signal that is lost in the noise; for a strong one it
 * IS the measurement, and the ratio saturates. A stronger station cannot then
 * report a better SNR, which is exactly the behaviour observed.
 *
 * So the noise is sampled at frequency offsets clear of the transmission -
 * still inside the 50 Hz the decimation filter passes, so it is the same
 * receiver noise through the same path - and combined with a MEDIAN, which is
 * what makes a neighbouring station 12 Hz away cost nothing rather than
 * inflating the floor. wsprd does the same thing with a 30th percentile over
 * the whole band. */
static double measure_noise_ref(const baseband_t *bb, tone_tw_t *tw, long dt,
                                 wspr_tp_t tp[WSPR_NSYM][4])
{
    /* A WSPR signal is 5.9 Hz wide, so +/-12 Hz is already clear of it, and
     * +/-33 Hz stays well inside the filter's passband. */
    static const double offs[] = { 10, 14, 18, 22, 26, 30, 34,
                                  -10, -14, -18, -22, -26, -30, -34 };
    const int NOFF = (int)(sizeof(offs) / sizeof(offs[0]));
    /* The caller's tone table is borrowed as scratch - it is 8 KB and this task
     * has already reserved a 16 KB frame; a second one is not free. The caller
     * rebuilds it immediately after, which the code below relies on. */
    double samp[14];
    for (int o = 0; o < NOFF; o++) {
        build_tone_tw(tw, offs[o]);
        extract_tone_powers(bb, tw, dt, tp);
        double acc = 0;
        for (int i = 0; i < WSPR_NSYM; i++)
            for (int k = 0; k < 4; k++) acc += tp[i][k];
        samp[o] = acc / (WSPR_NSYM * 4);
    }
    for (int i = 1; i < NOFF; i++) {          /* insertion sort - eight values */
        double key = samp[i]; int j = i - 1;
        while (j >= 0 && samp[j] > key) { samp[j + 1] = samp[j]; j--; }
        samp[j + 1] = key;
    }
    /* A LOW PERCENTILE, NOT THE MEDIAN. Contamination is one-sided: another
     * station inside a sample can only push it UP, never down, so the low end
     * of the distribution is the honest estimate and a median is already
     * biased on a crowded band. Measured: with a median, KI7CI and W5BIT in
     * the (busy) WSJT sample read 19 and 11 dB low, because half the offsets
     * around them are occupied. wsprd takes the 30th percentile over the whole
     * band for the same reason. */
    return samp[(NOFF * 3) / 10];
}

/* ---- DRIFT, FROM THE TRANSMISSION'S OWN TWO HALVES -----------------------
 *
 * A search over drift rates is what wsprd does, and it is expensive: it
 * multiplies the whole (frequency x time) grid by another axis. But drift only
 * has to be REPORTED here, not searched for, and once the message is decoded
 * the transmitted tone of every symbol is known - which turns the measurement
 * into two ordinary frequency estimates.
 *
 * If the frequency moves linearly by d Hz across the transmission, the first
 * half sits at -d/4 from centre and the second at +d/4, so d = 2 * (f2 - f1).
 * Each half's frequency is found by correlating against the KNOWN tones at a
 * range of offsets and taking the peak, refined by parabolic interpolation so
 * the answer is not quantised to the search step.
 *
 * ⚠ WEAKLY VALIDATED. The four reference recordings contain only three
 * non-zero drifts (+1, -2, -3), so this is checked mostly by agreeing with
 * wsprd's ZEROS. It reports a number rather than a dash, which is an
 * improvement, but do not treat a single drift reading as precise until it has
 * been checked against a station with known, deliberate drift. */
static int measure_drift(const baseband_t *bb, tone_tw_t *tw, long dt,
                          const uint8_t tones[WSPR_NSYM],
                          wspr_tp_t tp[WSPR_NSYM][4])
{
    /* +/-4 Hz is the range wsprd searches, and a WSPR transmitter drifting
     * further than that is not decodable anyway. */
    const double LO = -4.0, STEP = 0.5;
    const int N = 17;
    double h1[17], h2[17];
    for (int j = 0; j < N; j++) {
        build_tone_tw(tw, LO + j * STEP);
        extract_tone_powers(bb, tw, dt, tp);
        double a = 0, b = 0;
        for (int i = 0; i < WSPR_NSYM / 2; i++)              a += tp[i][tones[i]];
        for (int i = WSPR_NSYM / 2; i < WSPR_NSYM; i++)      b += tp[i][tones[i]];
        h1[j] = a; h2[j] = b;
    }
    double f[2];
    for (int half = 0; half < 2; half++) {
        const double *h = half ? h2 : h1;
        int best = 0;
        for (int j = 1; j < N; j++) if (h[j] > h[best]) best = j;
        double refine = 0.0;
        if (best > 0 && best < N - 1) {
            /* Parabolic peak through three points - sub-step resolution
             * without a finer (and 3x more expensive) grid. */
            const double y0 = h[best - 1], y1 = h[best], y2 = h[best + 1];
            const double den = y0 - 2 * y1 + y2;
            if (den < -1e-30) refine = 0.5 * (y0 - y2) / den;
            if (refine > 1.0) refine = 1.0;
            if (refine < -1.0) refine = -1.0;
        }
        f[half] = LO + (best + refine) * STEP;
    }
    double d = 2.0 * (f[1] - f[0]);
    if (d >  9.0) d =  9.0;
    if (d < -9.0) d = -9.0;
    return (int)lround(d);
}

static int accept_if_plausible(const wspr_msg_bytes_t *msg, unsigned int cycles,
                                wspr_tp_t tp[WSPR_NSYM][4], double noise_ref,
                                wspr_decode_result_t *result)
{
    result->cycles = cycles;
    char call[7], grid[5];
    int dbm;
    if (!wspr_unpack_message(msg, call, grid, &dbm)) return 0;
    if (strlen(call) < 3) return 0;
    if (!callsign_shape_ok(call)) return 0;
    if (!is_legal_power(dbm)) return 0;
    if (dbm > WSPR_PLAUSIBLE_MAX_DBM) return 0;
    if (cycles > WSPR_CYCLES_SUSPECT) return 0;
    wspr_msg_bytes_t repack;
    if (!wspr_pack_message(call, grid, dbm, &repack)) return 0;

    score_agreement(call, grid, dbm, tp, noise_ref, result);

    strcpy(result->callsign, call);
    strcpy(result->grid, grid);
    result->power_dbm = dbm;
    result->ok = 1;
    return 1;
}

/* ---- soft-decision demodulation (the sensitivity path) ------------------
 *
 * ⭐ THIS IS THE PATH THAT CLOSED MOST OF THE GAP TO wsprd. Measured against
 * the four reference WAVs, which between them hold 41 decodes wsprd finds:
 * hard-decision alone found 17. See docs/wspr-phase3-sensitivity.md for the
 * per-file table and the sweep that set ESNO_DB and BIAS.
 *
 * A K=32 rate-1/2 convolutional code is worth roughly 2 dB more with soft
 * decisions than hard ones, and we were throwing all of it away: the hard
 * path collapses each symbol to one bit and tells the Fano search that a
 * symbol which barely favoured 1 is exactly as trustworthy as one that
 * screamed it.
 *
 * ⚠ A SOFT METRIC WAS TRIED BEFORE AND REGRESSED THE REAL WAV BADLY - 1 of 8
 * plausible decodes instead of 5 (docs/wspr-phase1-status.md). That attempt is
 * not this one, and the difference is the whole lesson:
 *
 *   1. IT USED POWER, NOT AMPLITUDE. Squaring hands the sum to whichever few
 *      symbols happened to be loudest, which under fading is exactly the
 *      wrong emphasis. Every amplitude here is sqrt(power).
 *   2. ITS TABLE AND ITS NORMALISATION WERE FITTED SEPARATELY. The table is
 *      the statistics of a normalised soft symbol, so if the decoder
 *      normalises differently the table describes a distribution that never
 *      arrives. tools/gen_wspr_metric.py simulates whole 162-symbol blocks
 *      and normalises each block exactly the way this function does.
 *
 * The normalisation is per TRANSMISSION, not per symbol: subtract the mean,
 * divide by the standard deviation of the 162 values, scale, clip to a byte.
 * That makes the metric independent of how loud the station is, which is what
 * lets one fitted table serve every signal.
 */
#define WSPR_SOFT_SYMFAC   50.0    /* must match tools/gen_wspr_metric.py */

/* How many (frequency, start-time) hypotheses to try before giving up, and
 * the agreement at which an answer is convincing enough to stop looking.
 * Both are cost knobs: on the device a candidate must fit inside the decode
 * budget, and hypothesis 0 answers nearly every strong signal on its own. */
/* ⚠ MEASURED AT 1, NOT ASSUMED. Trying the 2nd and 3rd sync peaks as well
 * costs 25-45 % more time and found NOT ONE additional station across the four
 * reference WAVs - the second station of a cluster is reached from its own
 * candidate instead, once the sidelobe suppression stops erasing it (see
 * wspr_find_candidates). The mechanism is kept because it is what makes the
 * agreement check meaningful - a rejected answer has somewhere to fall back to
 * - and because it should start paying once weaker signals are reachable, but
 * shipping it above 1 would be paying for a result nothing has demonstrated. */
#ifndef WSPR_HYPOTHESES
#define WSPR_HYPOTHESES 1
#endif
#ifndef WSPR_AGREE_CONFIDENT
#define WSPR_AGREE_CONFIDENT 0.70f
#endif

/* ⭐ THE ONE CHECK THAT CONSULTS THE AUDIO. Set from the measured gap, the
 * same way the power and cycles guards were: across the four reference WAVs
 * and three search settings, every one of NINE fabrications scored 0.355 to
 * 0.513, and every one of the 21 wsprd-confirmed decodes scored 0.655 to
 * 0.914. There is no overlap and the gap is wide, so the threshold sits in
 * the middle of it rather than being tuned to either edge.
 *
 * ⚠ RE-MEASURE THIS AFTER ANY FRONT-END CHANGE, exactly as CLAUDE.md already
 * requires for the Fano cycles threshold - agreement is a property of the
 * demodulator, not of the signal. */
#ifndef WSPR_AGREE_MIN
#define WSPR_AGREE_MIN 0.58f
#endif

/* The rate term. A Fano search needs a random path's metric to drift DOWN,
 * or it cannot tell a good path from a lucky one and never backs up. For a
 * rate-1/2 code the textbook value is 0.5 bits per branch; slightly under
 * that buys sensitivity at the cost of search time, which is the trade wsprd
 * also makes (its own bias is 0.45). Swept - see the design note above. */
#ifndef WSPR_SOFT_BIAS
#define WSPR_SOFT_BIAS     0.45
#endif

/* ⛔ THE FANO THRESHOLD STEP IS IN THE SAME UNITS AS THE METRIC, AND GETTING
 * THAT WRONG LOOKS EXACTLY LIKE A DECODER THAT CANNOT HEAR. The first version
 * of this function passed delta=2 - correct for the hard table, whose metrics
 * are +1/-3 - against a table scaled by WSPR_METRIC_SCALE (1000). The search
 * then tightened its threshold in steps a five-hundredth of a symbol's worth
 * of evidence, so every hard candidate ran to the 1,620,001-cycle ceiling and
 * gave up, and the whole soft path measured as WORTH NOTHING (17 decodes, the
 * same as before it existed). wsprd's delta is 60 against metrics scaled by
 * 10, i.e. SIX BITS; this is the same figure in our units. */
#ifndef WSPR_SOFT_DELTA_BITS
#define WSPR_SOFT_DELTA_BITS  6
#endif
#define WSPR_SOFT_DELTA   (WSPR_SOFT_DELTA_BITS * WSPR_METRIC_SCALE)

static int try_soft_decision(wspr_tp_t tp[WSPR_NSYM][4], double noise_ref,
                             wspr_decode_result_t *result)
{
    double fsym[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        /* AMPLITUDES. tp[][] holds power; the soft symbol is a difference of
         * magnitudes, which is what the fitted table describes. */
        double a1 = sqrt(sync ? tp[i][3] : tp[i][2]);
        double a0 = sqrt(sync ? tp[i][1] : tp[i][0]);
        fsym[i] = a1 - a0;
    }

    double sum = 0, sq = 0;
    for (int i = 0; i < WSPR_NSYM; i++) { sum += fsym[i]; sq += fsym[i] * fsym[i]; }
    double mean = sum / WSPR_NSYM;
    double var  = sq / WSPR_NSYM - mean * mean;
    double sd   = (var > 1e-30) ? sqrt(var) : 1e-15;

    uint8_t chan[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double v = WSPR_SOFT_SYMFAC * fsym[i] / sd;
        if (v >  127.0) v =  127.0;
        if (v < -128.0) v = -128.0;
        chan[i] = (uint8_t)((int)lround(v) + 128);
    }

    uint8_t sym[WSPR_NSYM];
    wspr_deinterleave(chan, sym);

    /* Per-symbol branch metrics rather than a 2x256 table, purely for memory:
     * this runs on a 16 KB task stack that has already overflowed once (see
     * the tp[] note in wspr_decode_candidate), and 162x2 ints is 1.3 KB where
     * the table form is 2 KB. Identical arithmetic either way. */
    const int bias_q = (int)lround(WSPR_SOFT_BIAS * WSPR_METRIC_SCALE);
    int branch_metric[WSPR_NSYM][2];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int v = sym[i];
        branch_metric[i][0] = wspr_metric0[v]       - bias_q;
        branch_metric[i][1] = wspr_metric0[255 - v] - bias_q;
    }

    wspr_msg_bytes_t msg;
    unsigned int metric = 0, cycles = 0;
    if (!wspr_fano_decode_weighted(branch_metric, WSPR_SOFT_DELTA, 20000,
                                   &msg, &metric, &cycles)) {
        result->cycles = cycles;
        return 0;
    }
    return accept_if_plausible(&msg, cycles, tp, noise_ref, result);
}

/* Hard-decision per symbol, conditioned on the known sync bit. Simple and
 * robust - no calibration dependency at all - and what decodes 5 of the
 * reference WAV's 8 candidates.
 *
 * A per-capture-normalized SOFT metric (wspr_build_soft_metric_table())
 * was tried in place of this and reverted - worth recording why, since
 * it's a real trap. In a synthetic single/multi-tone AWGN sweep
 * (test/wspr_metric_sim.c, test/wspr_synth_harness.c) it measured ~2-3 dB
 * more sensitive than this hard-decision table. Wired in here, it
 * REGRESSED real-world performance badly: 1/8 plausible decodes from the
 * reference WAV instead of 5/8. Lesson worth keeping: a synthetic
 * sensitivity sweep passing is evidence, not proof - the real WAV test is
 * what actually decides whether a decoder change is a genuine
 * improvement. Full account in docs/wspr-phase1-status.md. */
static int try_hard_decision(wspr_tp_t tp[WSPR_NSYM][4], double noise_ref,
                             wspr_decode_result_t *result)
{
    uint8_t channel_bits[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double p_data1 = sync ? tp[i][3] : tp[i][2];
        double p_data0 = sync ? tp[i][1] : tp[i][0];
        channel_bits[i] = (p_data1 > p_data0) ? 1 : 0;
    }
    uint8_t raw[WSPR_NSYM];
    wspr_deinterleave(channel_bits, raw);
    uint8_t soft[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) soft[i] = raw[i] ? 255 : 0;

    int mettab[2][256];
    wspr_build_hard_metric_table(mettab);
    wspr_msg_bytes_t msg;
    unsigned int metric = 0, cycles = 0;
    if (!wspr_fano_decode(soft, mettab, 2, 20000, &msg, &metric, &cycles)) {
        result->cycles = cycles;
        return 0;
    }
    return accept_if_plausible(&msg, cycles, tp, noise_ref, result);
}

/* PER-SYMBOL reliability-weighted decision - targets a specific failure
 * mode the hard-decision path can't handle at all: real HF fading (QSB)
 * WITHIN a single 110 s transmission (test/wspr_diag_candidate0.c,
 * docs/wspr-phase1-status.md). A signal can be strong for half the
 * transmission and near-noise for the other half; hard-decision (and the
 * reverted per-CAPTURE soft table) both give every symbol the same
 * weight regardless of which half it came from, so ~40+ genuinely
 * unreliable symbols from the weak half poison the decode even though
 * the strong half alone would likely be enough.
 *
 * The fix: weight each symbol's contribution to the Fano metric by that
 * symbol's own LOCAL signal strength (smoothed over nearby symbols, since
 * fading is a slow, continuous envelope, not an independent per-symbol
 * event) relative to the capture's overall level. A symbol from a deep
 * fade contributes almost nothing either way; a symbol from a strong
 * stretch contributes with full confidence. This is a per-symbol analytic
 * formula, not a trained table (deliberately - the per-capture SOFT TABLE
 * attempt above regressed real data despite a careful synthetic
 * validation, and a formula that doesn't depend on matching a pre-trained
 * distribution shape is less exposed to that same synthetic/real
 * mismatch). Called as a FALLBACK, only when hard-decision doesn't
 * already produce a plausible result - see wspr_decode_candidate() -  so
 * candidates hard-decision already handles correctly can't regress. */
static int try_weighted_decision(wspr_tp_t tp[WSPR_NSYM][4], double noise_ref,
                             wspr_decode_result_t *result)
{
    double d_channel[WSPR_NSYM], power_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double p_data1 = sync ? tp[i][3] : tp[i][2];
        double p_data0 = sync ? tp[i][1] : tp[i][0];
        d_channel[i] = p_data1 - p_data0;
        power_channel[i] = tp[i][0] + tp[i][1] + tp[i][2] + tp[i][3];
    }

    /* Smooth the power envelope over a window of neighboring symbols, IN
     * TRANSMISSION-TIME (channel) ORDER - fading is a slow envelope over
     * real time, and only channel order reflects real time; raw/encode
     * order is scrambled by the interleaver. Window ~15 symbols (~10.2 s)
     * - short enough to track fading on the timescale
     * test/wspr_diag_candidate0.c actually observed (structure visible at
     * ~18 s granularity), long enough to average out symbol-to-symbol
     * jitter rather than chasing it. */
    const int half_window = 7;
    double smoothed[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int lo = i - half_window, hi = i + half_window;
        if (lo < 0) lo = 0;
        if (hi > WSPR_NSYM - 1) hi = WSPR_NSYM - 1;
        double sum = 0;
        for (int j = lo; j <= hi; j++) sum += power_channel[j];
        smoothed[i] = sum / (hi - lo + 1);
    }

    /* Reliability weight relative to the capture's own median smoothed
     * power - median rather than mean so a handful of very strong or very
     * weak symbols can't drag the reference point around. Clamped to keep
     * the search numerically well-behaved (an unclamped near-zero weight
     * would make a symbol contribute nothing at all rather than "very
     * little", which loses information a genuinely marginal symbol might
     * still carry). */
    double sorted[WSPR_NSYM];
    memcpy(sorted, smoothed, sizeof(sorted));
    for (int i = 1; i < WSPR_NSYM; i++) { /* insertion sort - 162 elements, not worth qsort overhead */
        double key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    double median = sorted[WSPR_NSYM / 2];
    if (median < 1e-9) median = 1e-9;

    double weight_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double w = smoothed[i] / median;
        if (w < 0.2) w = 0.2;
        if (w > 3.0) w = 3.0;
        weight_channel[i] = w;
    }

    double d_raw[WSPR_NSYM], weight_raw[WSPR_NSYM];
    wspr_deinterleave_scores(d_channel, d_raw);
    wspr_deinterleave_scores(weight_channel, weight_raw);

    double abs_sum = 0;
    for (int i = 0; i < WSPR_NSYM; i++) abs_sum += fabs(d_raw[i]);
    double scale = abs_sum / WSPR_NSYM;
    if (scale < 1e-9) scale = 1e-9;

    /* ALPHA/BETA/CAP shape the metric the same way the hard-decision
     * table's match=+1/mismatch=-3 does: BETA is a fixed per-symbol
     * penalty ensuring an uninformative (near-zero D) symbol still nets
     * negative for BOTH candidate bits (the Fano algorithm's structural
     * requirement - see wspr_build_hard_metric_table()'s comment), and
     * ALPHA sets how strongly a confident D actually moves the metric.
     * weight[] then scales the WHOLE contribution - informative part and
     * fixed penalty alike - so a low-reliability symbol's vote barely
     * counts either way instead of confidently voting the wrong way. */
    const double ALPHA = 6.0, BETA = 3.0, CAP = 4.0;
    int branch_metric[WSPR_NSYM][2];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double dn = d_raw[i] / scale;
        if (dn > CAP) dn = CAP;
        if (dn < -CAP) dn = -CAP;
        double base = ALPHA * dn;
        branch_metric[i][1] = (int)lround(weight_raw[i] * (base - BETA));
        branch_metric[i][0] = (int)lround(weight_raw[i] * (-base - BETA));
    }

    wspr_msg_bytes_t msg;
    unsigned int metric = 0, cycles = 0;
    if (!wspr_fano_decode_weighted(branch_metric, 2, 20000, &msg, &metric, &cycles)) {
        result->cycles = cycles;
        return 0;
    }
    return accept_if_plausible(&msg, cycles, tp, noise_ref, result);
}

/* Local start-time refinement around `centre`, at the fine step. Shared by
 * the first pass and by the re-take after the frequency moves. */
static long refine_dt(const baseband_t *bb, const tone_tw_t *tw, long centre,
                       long span, long step, long slack,
                       wspr_tp_t tp[WSPR_NSYM][4], double *best_score)
{
    long lo = centre - span, hi = centre + span;
    if (lo < 0) lo = 0;
    if (hi > slack) hi = slack;
    long best = centre;
    for (long dt = lo; dt <= hi; dt += step) {
        extract_tone_powers(bb, tw, dt, tp);
        double s = sync_score(tp);
        if (s > *best_score) { *best_score = s; best = dt; }
    }
    return best;
}

void wspr_decode_candidate(const int16_t *samples, long n, double f0_hz,
                            wspr_decode_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->freq_hz = f0_hz;
    result->snr_db   = WSPR_SNR_UNKNOWN;
    result->drift_hz = WSPR_DRIFT_UNKNOWN;

    /* Everything below works in DECIMATED samples. The start-time search is
     * where the cost lives - ~119 correlations per candidate - so it is the
     * part that had to move off the 12 kHz stream. */
    const int64_t t_start = wspr_now_us();
    baseband_t bb;
    if (!mix_decimate(samples, n, f0_hz, &bb)) { free_baseband(&bb); return; }
    const int64_t t_mix = wspr_now_us();
    result->ms_mix = (float)((t_mix - t_start) / 1000.0);

    tone_tw_t tw;
    build_tone_tw(&tw, 0.0);

    long slack_dec = bb.n - (long)WSPR_NSYM * WSPR_DEC_SPS;
    if (slack_dec < 0) slack_dec = 0;

    /* ONE tone-power array, reused. There used to be three - two loop-scoped
     * and one at function scope - at 162*4*8 = 5184 bytes each. They are never
     * live at the same time, so the compiler was free to overlap them and did
     * not: on the device this function overflowed a 16 KB task stack by 1268
     * bytes. That matters here far more than it would on a host, because
     * xTaskCreate() takes its stack from INTERNAL RAM and this board runs with
     * roughly 40 KB of it free (see CLAUDE.md's task-stack and .bss notes). */
    wspr_tp_t tp[WSPR_NSYM][4];

    /* Coarse start-time search, then refine around the best coarse hit -
     * exhaustive at the fine step would be needlessly slow; this two-pass
     * search finds the same optimum in testing.
     *
     * The steps are the SAME real-time intervals as before, expressed in
     * decimated samples: 8192/8 original = 32 decimated, 8192/32 = 8. So the
     * search grid did not get coarser when the rate dropped. */
    long best_dt = 0;
    double best_score = -1e300;
    double best_df = 0.0;
#ifndef WSPR_COARSE_STRIDE
#define WSPR_COARSE_STRIDE 4
#endif
    long coarse_step = WSPR_DEC_SPS / 8;          /* 32 dec = 1024 orig */
    if (coarse_step < 1) coarse_step = 1;
    long fine_step = WSPR_DEC_SPS / 32;           /* 8 dec = 256 orig */
    if (fine_step < 1) fine_step = 1;

    /* ⛔ THE COARSE SCAN RUNS AT STRIDE 4 AND ITS SCORES LIVE ON THEIR OWN
     * SCALE. Both halves of that matter.
     *
     * The scan is ~111 of the ~150 correlations a candidate costs, and all it
     * has to do is pick the right 1/8th of a symbol for the fine refinement
     * below - which is a far weaker requirement than the one the full-rate
     * correlation was being paid for. At stride 4 it reads 64 samples per
     * symbol instead of 256 (legal because of the 50 Hz decimation filter -
     * see extract_tone_powers_s).
     *
     * ⚠ A STRIDED SCORE MUST NEVER BE COMPARED WITH A FULL-RATE ONE. It is
     * normalised, so it is on the same [-1,1] axis, but it is measured through
     * a quarter of the integration and is therefore noisier and systematically
     * a little different. refine_dt() only replaces its incumbent when a trial
     * BEATS it, so seeding it with a strided score would let a strided
     * high-water mark veto every full-rate trial - the refinement would
     * silently stop refining. Hence the single full-rate re-score at the
     * winning dt before refining, and hence the gate below reading a full-rate
     * number. */
    for (long dt = 0; dt <= slack_dec; dt += coarse_step) {
        extract_tone_powers_s(&bb, &tw, dt, tp, WSPR_COARSE_STRIDE);
        double s = sync_score(tp);
        if (s > best_score) { best_score = s; best_dt = dt; }
    }
    extract_tone_powers(&bb, &tw, best_dt, tp);
    best_score = sync_score(tp);
    best_dt = refine_dt(&bb, &tw, best_dt, coarse_step, fine_step,
                        slack_dec, tp, &best_score);
    const int64_t t_coarse = wspr_now_us();
    result->ms_coarse = (float)((t_coarse - t_mix) / 1000.0);

    /* ---- STOP HERE IF NOTHING IS SYNCED --------------------------------
     *
     * ⚠ THIS GATE WAS WRITTEN, MEASURED, AND THEN LOST - reverted along with a
     * failed experiment in the same file - while the commit message and the
     * design note both went on describing it. So for one flash the device ran
     * WITHOUT it, and a log line reading `cycles=0 rejected` was read as "the
     * gate did its job" when in fact the candidate had run the entire search
     * and up to nine Fano attempts. Check that a thing is in the file before
     * reasoning about what it did.
     *
     * What it does: everything below - the frequency curve, per-hypothesis
     * start times, the Fano attempts - is spent on candidates that are mostly
     * NOISE. The comb finder ranks by energy, so in a 300 Hz window most of
     * the 20 candidates are the loudest patches of noise floor. The normalised
     * sync correlation separates them cheaply, which is the second reason it
     * had to stop being an unnormalised power sum: an absolute threshold is
     * only meaningful on a scale-free metric.
     *
     * Measured over the four reference WAVs: 133 candidates that decoded
     * nothing against 28 that did, the weakest real decode scoring 0.0815 and
     * more than half the rejects below it. wsprd gates the same quantity at
     * 0.10; this sits below our own weakest observed decode rather than on
     * wsprd's figure, because the two front ends are not identical.
     *
     * ⚠ A COST GATE, NOT A QUALITY GATE. It must never be the reason a station
     * is missed - re-measure it after any front-end change. */
#ifndef WSPR_MIN_SYNC
#define WSPR_MIN_SYNC 0.075
#endif
    if (best_score < WSPR_MIN_SYNC) {
        free_baseband(&bb);
        result->sync_score = best_score;
        result->best_dt_samples = best_dt * WSPR_DECIM;
        result->freq_hz = f0_hz;
        return;   /* ms_curve and ms_decode stay 0 - the gate is why */
    }


    /* ---- FINE FREQUENCY, THEN THE START TIME AGAIN --------------------
     *
     * The candidate's frequency comes from an averaged periodogram over the
     * whole 120 s window, so it is the frequency of the strongest BIN, not of
     * the signal: a fading station's peak bin is pulled around by noise, and a
     * neighbour 5 Hz away pulls it further. Being half a tone spacing out
     * (0.73 Hz) puts a third of every symbol's energy in the wrong tone.
     *
     * So refine it the way wsprd does - on the sync correlation, which only
     * peaks when the four tones actually line up, rather than on raw energy,
     * which peaks wherever the most noise is. Sequential, not a product: best
     * time, then best frequency, then best time again. Three cheap passes
     * instead of one expensive 2-D grid, and it converges because the two are
     * nearly independent.
     *
     * +/-0.7 Hz covers half a tone spacing either way, which is the whole
     * range in which a wrong answer is even possible - beyond it the search
     * would just lock onto the neighbouring tone. */
#ifndef WSPR_DF_RANGE
#define WSPR_DF_RANGE 1.5
#endif
#ifndef WSPR_DF_STEP
/* 0.2, not 0.1. Measured on the four reference WAVs: the same 23 stations at
 * half the trials. 0.3 loses one. The curve is 31 correlations at 0.1 Hz
 * against a ~120-correlation baseline, i.e. a quarter of the whole decode, and
 * on hardware a candidate costs 11.7 s - so this is worth about 1.5 s each. */
#define WSPR_DF_STEP  0.2
#endif
#define WSPR_DF_NPT   ((int)(2 * WSPR_DF_RANGE / WSPR_DF_STEP) + 1)

    /* Only for candidates that got past the gate: it is fourteen extra
     * correlations (one per offset in measure_noise_ref's table - this said
     * "eight" while the table had already grown to fourteen, and the phase
     * accounting above needs the real number), and a candidate that is pure
     * noise will never need an SNR.
     * Taken BEFORE the frequency curve because both use `tp` as scratch and
     * the curve rebuilds the tone table on its first iteration anyway. */
    const double noise_ref = measure_noise_ref(&bb, &tw, best_dt, tp);

    /* ---- THE SYNC-vs-FREQUENCY CURVE, AND WHY ITS PEAKS ARE THE HYPOTHESES
     *
     * The candidate frequency comes from an averaged periodogram, so it is the
     * strongest BIN in a neighbourhood, not the frequency of any one station.
     * Measured on the 19:10 reference window, where wsprd finds 14 stations:
     * every one of them has a candidate within ~1 Hz, but THREE of them
     * (G4FBA, PD2LEO, PA5CA - 1471.8, 1472.8, 1474.8 Hz) share a single
     * candidate at 1473.08, and DK8AF and DD3MS share another 2.8 Hz from one
     * of them. So the misses were never "not detected"; they were "detected as
     * somebody else".
     *
     * A single best-sync refinement can only ever return one of a cluster. The
     * sync correlation, though, has a distinct local maximum at EACH real
     * station - it only peaks where four tones line up on the sync vector, so
     * a neighbour 2 Hz away makes its own peak rather than smearing this one.
     * Taking the top few local maxima therefore hands the decoder one
     * hypothesis per station present, which is exactly what it needs.
     *
     * +/-1.5 Hz is a full tone spacing either way. That is far too wide to be
     * safe under "first answer wins" - it reaches neighbouring tones, and a
     * wrong-frequency decode looks just like a right one. It is only usable
     * because the re-encode check below can tell them apart. */
    double curve[WSPR_DF_NPT];
    for (int k = 0; k < WSPR_DF_NPT; k++) {
        double df = -WSPR_DF_RANGE + k * WSPR_DF_STEP;
        build_tone_tw(&tw, df);
        extract_tone_powers(&bb, &tw, best_dt, tp);
        curve[k] = sync_score(tp);
    }

    /* Local maxima, strongest first. A plateau counts once (>= on the left,
     * > on the right), and the ends count so a station at the edge of the
     * window is not silently dropped. */
    double hyp_df[WSPR_HYPOTHESES];
    double hyp_sc[WSPR_HYPOTHESES];
    int nhyp = 0;
    for (int k = 0; k < WSPR_DF_NPT; k++) {
        int rise = (k == 0)                || curve[k] >= curve[k - 1];
        int fall = (k == WSPR_DF_NPT - 1)  || curve[k] >  curve[k + 1];
        if (!(rise && fall)) continue;
        double df = -WSPR_DF_RANGE + k * WSPR_DF_STEP, sc = curve[k];
        int pos = nhyp < WSPR_HYPOTHESES ? nhyp : WSPR_HYPOTHESES;
        while (pos > 0 && hyp_sc[pos - 1] < sc) {
            if (pos < WSPR_HYPOTHESES) { hyp_sc[pos] = hyp_sc[pos - 1]; hyp_df[pos] = hyp_df[pos - 1]; }
            pos--;
        }
        if (pos < WSPR_HYPOTHESES) { hyp_sc[pos] = sc; hyp_df[pos] = df; }
        if (nhyp < WSPR_HYPOTHESES) nhyp++;
    }
    if (nhyp == 0) { hyp_df[0] = 0.0; hyp_sc[0] = best_score; nhyp = 1; }
    best_df = hyp_df[0];
    best_score = hyp_sc[0];

    result->sync_score = best_score;

    /* ---- TRY, SCORE, AND KEEP THE BEST - NOT THE FIRST -----------------
     *
     * ⭐ THIS ORDERING ONLY BECAME POSSIBLE ONCE THE RE-ENCODE CHECK EXISTED,
     * and it is the difference between a wider search helping and hurting.
     * Measured: widening the frequency search from +/-0.0 to +/-0.7 Hz under
     * "first answer wins" GAINED four decodes and LOST three real ones
     * (PA4JAM, PE1JXI, G8ORM) to wrong codewords found at a wrong frequency,
     * which is a wash bought with three fabrications. The search was not the
     * problem; having no way to tell a good answer from a bad one was.
     *
     * With agreement in hand every hypothesis can be tried and scored, and the
     * best-agreeing answer kept - so a wrong-frequency decode simply loses to
     * the right one instead of pre-empting it.
     *
     * Cost is paid only when needed: the loop stops as soon as an answer
     * agrees convincingly, which is the common case for anything but the
     * weakest signals. */
    const int64_t t_curve = wspr_now_us();
    result->ms_curve = (float)((t_curve - t_coarse) / 1000.0);

    wspr_decode_result_t best;
    memset(&best, 0, sizeof(best));
    best.agree_soft = -1e9f;
    unsigned int worst_cycles = 0;

    for (int h = 0; h < nhyp; h++) {
        double df = hyp_df[h];
        build_tone_tw(&tw, df);
        /* Each peak gets its own start time: two stations 2 Hz apart are
         * unrelated transmissions and will not have started together. */
        extract_tone_powers(&bb, &tw, best_dt, tp);
        double sc = sync_score(tp);
        long dt = refine_dt(&bb, &tw, best_dt, coarse_step, fine_step,
                             slack_dec, tp, &sc);
        extract_tone_powers(&bb, &tw, dt, tp);

        wspr_decode_result_t r;
        for (int path = 0; path < 3; path++) {
            memset(&r, 0, sizeof(r));
            r.freq_hz = f0_hz + df;
            r.best_dt_samples = dt * WSPR_DECIM;
            r.sync_score = sc;
            int got = (path == 0) ? try_soft_decision(tp, noise_ref, &r)
                    : (path == 1) ? try_hard_decision(tp, noise_ref, &r)
                                  : try_weighted_decision(tp, noise_ref, &r);
            /* ⚠ KEEP THE CYCLE COUNT EVEN WHEN NOTHING IS ACCEPTED. `r` is
             * discarded on failure, and with it went the only number that says
             * HOW the attempt failed - so every rejected candidate logged
             * `cycles=0`, which reads like "the search never ran" and is what
             * led to one wrong conclusion already. Worst case across the
             * attempts is the informative one. */
            if (r.cycles > worst_cycles) worst_cycles = r.cycles;
            if (got && r.agree_soft > best.agree_soft) best = r;
        }
        if (best.ok && best.agree_soft >= WSPR_AGREE_CONFIDENT) break;
    }

    /* Drift is measured only for an accepted decode, and BEFORE the baseband
     * is released - it needs the audio, and the transmitted tones, and both
     * are only available together right here. */
    if (best.ok) {
        uint8_t dtones[WSPR_NSYM];
        if (wspr_tones_from_message(best.callsign, best.grid, best.power_dbm, dtones))
            best.drift_hz = (int16_t)measure_drift(&bb, &tw,
                                                    best.best_dt_samples / WSPR_DECIM,
                                                    dtones, tp);
    }

    free_baseband(&bb);
    const float ms_decode = (float)((wspr_now_us() - t_curve) / 1000.0);

    /* WSPR has no CRC, so this is where a wrong-but-valid codeword is caught.
     * Nothing above can do it: every other check tests the MESSAGE against
     * itself, and a near-miss decode of a real station is a perfectly
     * well-formed message. */
    if (best.ok && best.agree_soft < WSPR_AGREE_MIN) {
        best.ok = 0;
    }

    if (best.ok) {
        /* `best` came from one attempt, so it carries none of the phase
         * timings - those belong to the candidate, not to the attempt. */
        const double sync_keep = result->sync_score;
        const float mix = result->ms_mix, coarse = result->ms_coarse,
                    curve = result->ms_curve;
        *result = best;
        result->sync_score = sync_keep;
        result->ms_mix = mix; result->ms_coarse = coarse; result->ms_curve = curve;
    } else {
        result->best_dt_samples = best_dt * WSPR_DECIM;
        result->freq_hz = f0_hz + best_df;
        result->cycles = worst_cycles;
    }
    result->ms_decode = ms_decode;
}

/* ---- false-decode guards (see wspr_decode.h for the evidence) ---------- */

void wspr_guards_defaults(wspr_guards_t *g)
{
    if (!g) return;
    /* NEAR on: it is the surgical one and should cost no genuine decode.
     *
     * ⭐ SLOW IS NOW ON TOO, and the reason it took real-world data is exactly
     * what this comment used to demand. Two continuous runs, 16 h, 807 decodes
     * from 100 stations confirmed real by REPETITION (a real station transmits
     * again; a fabrication does not - ground truth needing no second decoder):
     *
     *   - EVERY one of those 100 stations has at least one decode converging in
     *     81-175 cycles. Not a single station's easiest sighting exceeded 175.
     *   - The one-off population sits at a median of ~1238 cycles, and its
     *     callsigns give it away: 740UIU, 805KIM, 859IKW, C28K - legal to the
     *     encoder, impossible as callsigns.
     *   - At 1000 cycles: 0 of 100 real STATIONS lost, 68 of 112 slow one-offs
     *     rejected, and no confirmed-real reference decode rejected either.
     *
     * ⛔ THE UNIT MATTERS, AND I HAD IT WRONG. This gate does reject genuine
     * DECODES - 40 of 807 - because a real station's individual sighting can be
     * slow (G8MCD needed 826 live, and wsprd confirms 428 and 453 in the
     * reference files). What it does not reject is a real STATION, because a
     * station transmitting every few minutes always lands a fast one. Counting
     * decodes said "no safe threshold"; counting stations says 600 is free.
     *
     * 1000 specifically: above the 823 that another implementation confirms as
     * real ON THE CURRENT DECODE PATH, far above the 175 worst-case easiest
     * decode, and below most of the fabrication population.
     *
     * ⚠ 600 was chosen first, from cycle counts measured BEFORE the
     * windowed-sinc filter - and the filter moved them (PA3BCA: 336 -> 823), so
     * 600 would have rejected a confirmed-real decode. A cycle count is a
     * property of the DECODE PATH, not the signal: any front-end change
     * invalidates every cycles threshold. Re-measure against
     * test/wav_reference/wspr/ before trusting one.
     *
     * ⚠ THE RESIDUAL COST is a genuinely weak station heard EXACTLY ONCE and
     * slowly - that is now dropped, and it cannot be quantified without wsprd
     * on those same windows. If a confirmed real station is ever lost this way,
     * RAISE the threshold; the gap is what matters, not the number. */
    /* ⛔ BOTH STOOD DOWN 2026-08-27, AND EVERYTHING ABOVE IS THE REASON THIS
     * NOTE EXISTS RATHER THAN A DELETION. Neither guard was wrong. Both were
     * measured carefully, on real data, and both did what they claimed. They
     * are off because the re-encode agreement check (wspr_decode_result_t,
     * WSPR_AGREE_MIN) now does their job better, and because on the current
     * decode path they cost real stations:
     *
     *   SLOW at 1000 cycles rejects THREE wsprd-confirmed decodes - PA4JAM
     *     (1716), PA2PGU (1491), DK8AF (1423). This is precisely the failure
     *     the paragraph above predicts: a cycle count is a property of the
     *     DECODE PATH, and the soft-decision metric is a different path. The
     *     16-hour field measurement that set 1000 was taken on the hard
     *     decision path and no longer describes this one. Re-deriving the
     *     number is possible but pointless when a better test exists.
     *
     *   NEAR at 10 Hz rejects TWO confirmed decodes, and its own premise has
     *     been overtaken: "two WSPR signals closer than ~6 Hz overlap anyway"
     *     was true when a cluster yielded one candidate, but the candidate
     *     finder now resolves them (2E0DLC and OE5OSP are 5.8 Hz apart and
     *     both real; DK8AF and DD3MS are 4 Hz apart and both real).
     *
     * WHAT REPLACES THEM. The LG9TPW fabrication that motivated NEAR was a
     * candidate on a strong signal's skirt - a decode that the received audio
     * does not support, which is exactly what agreement measures. On the noise
     * ladder, four fabrications survived all four guards before; with
     * agreement enforced, none do, while the confirmed count under 1-2 dB of
     * added noise doubled.
     *
     * ⭐ THEY ARE STILL MEASURED ON EVERY DECODE, which is the property the
     * comment above rightly insisted on - so the day one of them is wanted
     * back, the evidence for it is already in an ordinary session's log rather
     * than needing two flashes and two different band conditions.
     *
     * The power guard (>43 dBm, in accept_if_plausible) is untouched: it costs
     * nothing on any measured data and is free to keep. */
    g->enforce_near = 0;
    g->near_hz      = WSPR_GUARD_NEAR_HZ;
    g->enforce_slow = 0;
    g->slow_cycles  = WSPR_GUARD_SLOW_CYCLES;
}

double wspr_nearest_accepted_hz(const wspr_accepted_t *acc, double freq_hz)
{
    if (!acc || acc->n <= 0) return -1.0;
    double best = -1.0;
    for (int i = 0; i < acc->n; i++) {
        double d = freq_hz - acc->freq_hz[i];
        if (d < 0) d = -d;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

wspr_guard_verdict_t wspr_guard_check(const wspr_guards_t *g,
                                      const wspr_accepted_t *acc,
                                      const wspr_decode_result_t *r,
                                      double *nearest_hz_out,
                                      int *would_near, int *would_slow)
{
    const double near_hz = g ? g->near_hz : WSPR_GUARD_NEAR_HZ;
    const unsigned int slow = g ? g->slow_cycles : WSPR_GUARD_SLOW_CYCLES;

    const double d = wspr_nearest_accepted_hz(acc, r->freq_hz);
    /* d < 0 means nothing accepted yet, which is NOT "near". Writing this out
     * because `d < near_hz` alone would silently reject the cycle's FIRST
     * decode every time - the sentinel is negative. */
    const int near_hit = (d >= 0.0 && d < near_hz);
    const int slow_hit = (r->cycles > slow);

    if (nearest_hz_out) *nearest_hz_out = d;
    if (would_near)     *would_near     = near_hit;
    if (would_slow)     *would_slow     = slow_hit;

    if (g && g->enforce_near && near_hit) return WSPR_GUARD_REJECT_NEAR;
    if (g && g->enforce_slow && slow_hit) return WSPR_GUARD_REJECT_SLOW;
    return WSPR_GUARD_PASS;
}

void wspr_accepted_add(wspr_accepted_t *acc, double freq_hz)
{
    if (!acc || acc->n >= WSPR_ACCEPTED_MAX) return;
    acc->freq_hz[acc->n++] = freq_hz;
}
