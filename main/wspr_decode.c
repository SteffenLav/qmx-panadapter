#include "wspr_decode.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fft/kiss_fftr.h" /* ft8_lib's component INCLUDE_DIRS is its own root
                             ("."), not fft/ specifically - matters once this
                             file is built as part of the real firmware
                             (main/CMakeLists.txt REQUIRES ft8_lib), not just
                             host test builds that pass -I .../ft8_lib/fft
                             directly. */

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
            int supp = tone_step_bins * 4;
            for (int i = best - supp; i <= best + supp; i++) {
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
#define LPF_TAPS   256   /* POWER OF TWO on purpose - see the ring below */
#define LPF_CUT_HZ 50.0

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

    const double w = 2.0 * M_PI * f0 / WSPR_SAMPLE_RATE_HZ;
    const double cs = cos(-w), sn = sin(-w);   /* step: e^(-j*w) */
    double ore = 1.0, oim = 0.0;

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
    static float hi[LPF_TAPS], hq[LPF_TAPS];
    memset(hi, 0, sizeof(hi));
    memset(hq, 0, sizeof(hq));
    int hp = 0;

    long o = 0;
    int cnt = 0;
    for (long idx = 0; idx < out_n * WSPR_DECIM; idx++) {
        const float x = samples[idx] * (1.0f / 32768.0f);
        hi[hp] = x * (float)ore;
        hq[hp] = x * (float)oim;
        hp = (hp + 1) & (LPF_TAPS - 1);

        double nre = ore * cs - oim * sn;
        double nim = ore * sn + oim * cs;
        ore = nre; oim = nim;
        if ((idx & 1023) == 1023) {           /* renormalise: |osc| -> 1 */
            double m = sqrt(ore * ore + oim * oim);
            if (m > 0) { ore /= m; oim /= m; }
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
            int r = hp;
            for (int k = 0; k < LPF_TAPS; k++) {
                ai += s_lpf[k] * hi[r];
                aq += s_lpf[k] * hq[r];
                r = (r + 1) & (LPF_TAPS - 1);
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
    float cos_tab[4][WSPR_DEC_SPS];
    float sin_tab[4][WSPR_DEC_SPS];
} tone_tw_t;

static void build_tone_tw(tone_tw_t *tw)
{
    for (int k = 0; k < 4; k++) {
        double w = 2.0 * M_PI * (k * TONE_SPACING) / WSPR_DEC_RATE_HZ;
        for (int j = 0; j < WSPR_DEC_SPS; j++) {
            tw->cos_tab[k][j] = (float)cos(w * j);
            tw->sin_tab[k][j] = (float)sin(w * j);
        }
    }
}

/* Correlate each symbol against each of the 4 tones, on the decimated complex
 * baseband. start_dec is the transmission start in DECIMATED samples.
 *
 * As before only the magnitude is kept, so the per-symbol phase of the local
 * oscillator cancels and the tables can be indexed by the offset within the
 * symbol. */
static void extract_tone_powers(const baseband_t *bb, const tone_tw_t *tw,
                                 long start_dec, double tone_power[WSPR_NSYM][4])
{
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        long base = start_dec + (long)sym * WSPR_DEC_SPS;
        long j0 = 0, j1 = WSPR_DEC_SPS;
        if (base < 0)               j0 = -base;
        if (base + j1 > bb->n)      j1 = bb->n - base;
        for (int k = 0; k < 4; k++) {
            const float *ct = tw->cos_tab[k], *st = tw->sin_tab[k];
            float re = 0, im = 0;
            for (long j = j0; j < j1; j++) {
                float xi = bb->i[base + j], xq = bb->q[base + j];
                /* (xi + j*xq) * e^(-j*w*j) */
                re += xi * ct[j] + xq * st[j];
                im += xq * ct[j] - xi * st[j];
            }
            tone_power[sym][k] = (double)re * re + (double)im * im;
        }
    }
}

/* sync=1 tones are {1,3} (odd tone index), sync=0 tones are {0,2} - the
 * higher this is, the better the (f0, dt) alignment matches WSPR's known
 * 162-bit sync vector. */
static double sync_score(double tone_power[WSPR_NSYM][4])
{
    double s = 0;
    for (int i = 0; i < WSPR_NSYM; i++) {
        double p1 = tone_power[i][1] + tone_power[i][3];
        double p0 = tone_power[i][0] + tone_power[i][2];
        double favor1 = p1 - p0;
        s += wspr_sync_vector[i] ? favor1 : -favor1;
    }
    return s;
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
static int accept_if_plausible(const wspr_msg_bytes_t *msg, unsigned int cycles,
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

    strcpy(result->callsign, call);
    strcpy(result->grid, grid);
    result->power_dbm = dbm;
    result->ok = 1;
    return 1;
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
static int try_hard_decision(double tp[WSPR_NSYM][4], wspr_decode_result_t *result)
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
    return accept_if_plausible(&msg, cycles, result);
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
static int try_weighted_decision(double tp[WSPR_NSYM][4], wspr_decode_result_t *result)
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
    return accept_if_plausible(&msg, cycles, result);
}

void wspr_decode_candidate(const int16_t *samples, long n, double f0_hz,
                            wspr_decode_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->freq_hz = f0_hz;

    /* Everything below works in DECIMATED samples. The start-time search is
     * where the cost lives - ~119 correlations per candidate - so it is the
     * part that had to move off the 12 kHz stream. */
    baseband_t bb;
    if (!mix_decimate(samples, n, f0_hz, &bb)) { free_baseband(&bb); return; }

    tone_tw_t tw;
    build_tone_tw(&tw);

    long slack_dec = bb.n - (long)WSPR_NSYM * WSPR_DEC_SPS;
    if (slack_dec < 0) slack_dec = 0;

    /* ONE tone-power array, reused. There used to be three - two loop-scoped
     * and one at function scope - at 162*4*8 = 5184 bytes each. They are never
     * live at the same time, so the compiler was free to overlap them and did
     * not: on the device this function overflowed a 16 KB task stack by 1268
     * bytes. That matters here far more than it would on a host, because
     * xTaskCreate() takes its stack from INTERNAL RAM and this board runs with
     * roughly 40 KB of it free (see CLAUDE.md's task-stack and .bss notes). */
    double tp[WSPR_NSYM][4];

    /* Coarse start-time search, then refine around the best coarse hit -
     * exhaustive at the fine step would be needlessly slow; this two-pass
     * search finds the same optimum in testing.
     *
     * The steps are the SAME real-time intervals as before, expressed in
     * decimated samples: 8192/8 original = 32 decimated, 8192/32 = 8. So the
     * search grid did not get coarser when the rate dropped. */
    long best_dt = 0;
    double best_score = -1e300;
    long coarse_step = WSPR_DEC_SPS / 8;          /* 32 dec = 1024 orig */
    if (coarse_step < 1) coarse_step = 1;
    for (long dt = 0; dt <= slack_dec; dt += coarse_step) {
        extract_tone_powers(&bb, &tw, dt, tp);
        double s = sync_score(tp);
        if (s > best_score) { best_score = s; best_dt = dt; }
    }
    long fine_lo = best_dt - coarse_step, fine_hi = best_dt + coarse_step;
    if (fine_lo < 0) fine_lo = 0;
    if (fine_hi > slack_dec) fine_hi = slack_dec;
    long fine_step = WSPR_DEC_SPS / 32;           /* 8 dec = 256 orig */
    if (fine_step < 1) fine_step = 1;
    for (long dt = fine_lo; dt <= fine_hi; dt += fine_step) {
        extract_tone_powers(&bb, &tw, dt, tp);
        double s = sync_score(tp);
        if (s > best_score) { best_score = s; best_dt = dt; }
    }

    extract_tone_powers(&bb, &tw, best_dt, tp);
    free_baseband(&bb);

    /* reported in ORIGINAL samples, so the API is unchanged for callers */
    result->best_dt_samples = best_dt * WSPR_DECIM;
    result->sync_score = best_score;

    if (try_hard_decision(tp, result)) return;
    /* Hard decision failed or was implausible - give the per-symbol
     * weighted attempt a shot before giving up on this candidate. */
    try_weighted_decision(tp, result);
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
    g->enforce_near = 1;
    g->near_hz      = WSPR_GUARD_NEAR_HZ;
    g->enforce_slow = 1;
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
