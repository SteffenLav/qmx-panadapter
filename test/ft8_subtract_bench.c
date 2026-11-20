/* FT8 SUBTRACT-AND-REPEAT: what a second and third decode pass are worth.
 *
 * ft8_decode_bench.c answers "what does one pass cost and find". This answers
 * the question that bench left open: its sweep showed candidates, min score
 * and LDPC iterations are all NOT binding (140 -> 600 moves nothing), so the
 * messages we miss are not reachable by turning a knob. 235 of 345
 * ground-truth messages on the reference corpus; the other 110 are hidden
 * under stronger signals.
 *
 * WSJT-X decodes, subtracts what it decoded from the audio, and decodes
 * again. ft8_lib has no such pass. This measures whether adding one is worth
 * the work, on known-correct data, before any of it goes near the firmware.
 *
 * Build (from the repo root):
 *   gcc -O2 -o ft8_subtract_bench test/ft8_subtract_bench.c \
 *       components/ft8_lib/ft8/{decode,encode,ldpc,text,message,crc,constants}.c \
 *       components/ft8_lib/ft8/{js8_codec,js8_tables}.c \
 *       components/ft8_lib/common/monitor.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       main/ft8_subtract.c -Imain \
 *       -Icomponents/ft8_lib -Icomponents/ft8_lib/ft8 \
 *       -Icomponents/ft8_lib/common -Icomponents/ft8_lib/fft -lm
 *
 *   for w in test/wav_reference/*.wav; do \
 *       ./ft8_subtract_bench $w ${w%.wav}.txt; done
 *
 * HOW THE SUBTRACTION WORKS, and why it is built this way.
 *
 * ft8_lib ships monitor_resynth() behind WATERFALL_USE_PHASE, which rebuilds
 * a signal from the waterfall's own magnitude and phase. It is not used here:
 * it is unfinished (its `int offset = 1; // candidate->time_offset;` discards
 * the candidate's actual time) and it reconstructs whatever was in those bins
 * rather than the message we decoded. Re-encoding is strictly better - we
 * know the exact 79 symbols, because they survived a 14-bit CRC.
 *
 *   1. ft8_encode() the decoded payload back to 79 tones. This is the same
 *      waveform the far end transmitted, bit for bit.
 *   2. Synthesise its GFSK phase at the candidate's own frequency and start
 *      time (the conversion is ft8_lib's own, demo/decode_ft8.c:153).
 *   3. [*] FIT IN QUADRATURE, PER SYMBOL. The synthesised phase will not
 *      match the received signal's, and a real-valued scale factor cannot
 *      correct a phase error - it would leave most of the energy behind and
 *      the whole pass would read as "subtraction does not help". So fit
 *      a*sin(phi) + b*cos(phi) by least squares, which is a 2x2 solve, and do
 *      it per symbol block so the fit tracks fading across the 12.64 s burst.
 *   4. Subtract, re-run the STFT on the residual, decode again.
 *
 * MEASURED 2026-11-20, the 22 ground-truth WAVs, one binary per setting.
 * All four rows are THIS harness, so the accounting is identical and the
 * deltas are the result (its absolute numbers differ from
 * ft8_decode_bench.c's because that one dedups differently - do not mix the
 * two files' totals):
 *
 *   passes   osr    uniq   hit/362   total ms
 *     1      2x2     264     260        404
 *     2      2x2     285     280       1994     <-- +21 uniq, +8.0 %
 *     3      2x2     285     280       2413
 *     4      2x2     285     280       2407
 *     1      4x2     274     268        616
 *     2      4x2     285     279       2389
 *     3      4x2     285     279       2709
 *
 * [*] ONE SUBTRACTION PASS IS WORTH +21 UNIQUE DECODES, +8.0 %. A third pass
 * adds nothing at all - pass 2's finds are weak and hide nothing behind them.
 *
 * [*] IT DOES NOT COMPOUND WITH time_osr=4. Both reach the same 285 ceiling
 * by different routes: finer time resolution finds +10 on its own,
 * subtraction finds +21 on its own, and together they still find 285 for
 * 20 % more time. So subtraction is the stronger lever AND the cheaper one,
 * and the time_osr branch can be dropped where subtraction is available.
 *
 * Cost: 404 -> 1994 ms for 22 slots is about 90 ms per slot. The device's own
 * budget is FT8_DECODE_BUDGET_MS = 11000.
 *
 * [!] ONLY MESSAGES NEW IN THIS PASS ARE SUBTRACTED, and each is subtracted
 * once. Subtracting the same signal twice removes energy that is not there
 * and digs a hole that later passes decode as noise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <ft8/message.h>
#include <ft8/constants.h>
#include <common/monitor.h>
#include "ft8_subtract.h"

#define SR        12000
#define MAXC      140
#define MINSCORE  5
#define ITERS     30
#ifndef TIME_OSR
#define TIME_OSR  2
#endif
#ifndef FREQ_OSR
#define FREQ_OSR  2
#endif
#ifndef NPASS
#define NPASS     3        /* total decode passes, 1 = today's behaviour */
#endif
#define GFSK_BT   2.0f
#define GFSK_K    5.336446f          /* pi * sqrt(2 / log(2)) */
#define NSIG      (SR * 20)
#define MAXMSG    400

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------- wav in */
static int load_wav(float *sig, int *n, int *sr, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char riff[4];
    if (fread(riff, 1, 4, f) != 4) { fclose(f); return -1; }
    fseek(f, 12, SEEK_SET);
    int cid, csz, srt, fmt;
    while (fread(&cid, 4, 1, f) == 1) {
        if (fread(&csz, 4, 1, f) != 1) break;
        if (cid == 0x20746d66) {
            if (fread(&fmt, 2, 1, f) != 1) break;
            short ch;
            if (fread(&ch, 2, 1, f) != 1) break;
            if (fread(&srt, 4, 1, f) != 1) break;
            *sr = srt;
            fseek(f, csz - 8, SEEK_CUR);
        } else if (cid == 0x61746164) {
            short *p = malloc(csz);
            if (fread(p, 1, csz, f) != (size_t)csz) { /* short read: use what came */ }
            int nr = csz / 2;
            *n = (nr < *n) ? nr : *n;
            for (int i = 0; i < *n; i++) sig[i] = (float)p[i] / 32768.0f;
            free(p);
            break;
        } else fseek(f, csz, SEEK_CUR);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------ truth file */
#define MAXE 200
static char exp_txt[MAXE][64];
static int  n_exp;

static void load_expected(const char *p)
{
    n_exp = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && n_exp < MAXE) {
        char *s = strchr(line, '~');
        if (!s) continue;
        s++;
        while (*s == ' ') s++;
        int len = strlen(s);
        while (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
        int sp = 0, rl = len;
        for (int j = 0; j < len; j++)
            if (s[j] == ' ') { if (++sp == 3) { rl = j; break; } }
        while (rl && s[rl - 1] == ' ') rl--;
        if (rl > 0 && rl < 63) { memcpy(exp_txt[n_exp], s, rl); exp_txt[n_exp][rl] = 0; n_exp++; }
    }
    fclose(f);
}

static int match(const char *a, const char *b)
{
    while (*a && *b) {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (!*a || !*b) break;
        if (tolower(*a) != tolower(*b)) return 0;
        a++; b++;
    }
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    return !*a && !*b;
}

/* [!] THE ALGORITHM IS NOT HERE ANY MORE. It lives in main/ft8_subtract.c
 * and this harness LINKS THE REAL FUNCTION, like every other harness in this
 * directory - a copy here would drift from the firmware's and the number this
 * prints would stop meaning anything. */
static double subtract_signal(float *sig, int n_sig, const uint8_t *tones,
                              int n_sym, float f0, int start)
{
    static float scratch[(79 * FT8_SUB_SPSYM) + (2 * FT8_SUB_SPSYM)
                         + (3 * FT8_SUB_SPSYM) + (79 * FT8_SUB_SPSYM)];
    return ft8_subtract_signal(sig, n_sig, tones, n_sym, f0, start, SR, scratch);
}

/* Alignment probe: which start offset maximises the energy the fit takes?
 * ft8_lib's own monitor_resynth() carries the comment "Starting offset is 3
 * subblocks due to analysis buffer loading", so the waterfall's time index is
 * NOT simply the sample index, and a subtraction aligned by the obvious
 * arithmetic can be a whole symbol out. Rather than reason about it, sweep
 * it and look. Enabled with -DPROBE_ALIGN. */
#ifdef PROBE_ALIGN
static void probe_align(const float *sig, int n_sig, const uint8_t *tones,
                        float f0, int start)
{
    const int n_spsym = (int)(0.5f + SR * FT8_SYMBOL_PERIOD);
    printf("    align probe f0=%.1f start=%d:", f0, start);
    for (int d = -2; d <= 2; d++) {
        float *copy = malloc((size_t)n_sig * sizeof(float));
        memcpy(copy, sig, (size_t)n_sig * sizeof(float));
        double f = subtract_signal(copy, n_sig, tones, FT8_NN, f0,
                                   start + d * n_spsym);
        printf("  %+d:%.3f", d, f);
        free(copy);
    }
    printf("\n");
}
#endif

/* ------------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <wav> [expected.txt]\n", argv[0]); return 1; }
    static float sig[NSIG];
    int n = NSIG, sr = SR;
    if (load_wav(sig, &n, &sr, argv[1]) < 0) { fprintf(stderr, "bad wav %s\n", argv[1]); return 1; }
    if (argc > 2) load_expected(argv[2]);

    char  seen[MAXMSG][64];
    int   n_seen = 0;
    int   per_pass[8] = { 0 };
    double t_total = now_ms();
    double worst_fit = 1e9, sum_fit = 0; int n_fit = 0;

    for (int pass = 0; pass < NPASS; pass++) {
        monitor_t mon;
        monitor_config_t cfg = { 200, 3000, sr, TIME_OSR, FREQ_OSR, FTX_PROTOCOL_FT8 };
        monitor_init(&mon, &cfg);
        for (int p = 0; p + mon.block_size <= n; p += mon.block_size)
            monitor_process(&mon, sig + p);

        ftx_candidate_t cands[MAXC];
        int nc = ftx_find_candidates(&mon.wf, MAXC, cands, MINSCORE);

        /* Collected first, subtracted after the whole pass: subtracting while
         * still decoding would change the waterfall under the candidate list
         * we are walking. */
        struct { uint8_t tones[FT8_NN]; float f0; int start; } todo[MAXC];
        int n_todo = 0;

        for (int i = 0; i < nc; i++) {
            ftx_message_t m;
            ftx_decode_status_t st;
            if (!ftx_decode_candidate(&mon.wf, &cands[i], ITERS, &m, &st)) continue;
            char txt[64];
            ftx_message_offsets_t off;
            if (ftx_message_decode(&m, NULL, txt, &off) != FTX_MESSAGE_RC_OK) continue;

            int dup = 0;
            for (int j = 0; j < n_seen; j++) if (match(seen[j], txt)) { dup = 1; break; }
            if (dup) continue;
            if (n_seen < MAXMSG) { strncpy(seen[n_seen], txt, 63); seen[n_seen][63] = 0; n_seen++; }
            per_pass[pass < 8 ? pass : 7]++;

            if (pass + 1 < NPASS && n_todo < MAXC) {
                ft8_encode(m.payload, todo[n_todo].tones);
                todo[n_todo].f0 = (mon.min_bin + cands[i].freq_offset
                                   + (float)cands[i].freq_sub / mon.wf.freq_osr)
                                  / mon.symbol_period;
                float t_sec = (cands[i].time_offset
                               + (float)cands[i].time_sub / mon.wf.time_osr)
                              * mon.symbol_period;
                todo[n_todo].start = ft8_subtract_start_sample(t_sec, SR);
                n_todo++;
            }
        }
        monitor_free(&mon);

        for (int i = 0; i < n_todo; i++) {
#ifdef PROBE_ALIGN
            if (pass == 0 && i < 4)
                probe_align(sig, n, todo[i].tones, todo[i].f0, todo[i].start);
#endif
            double frac = subtract_signal(sig, n, todo[i].tones, FT8_NN,
                                          todo[i].f0, todo[i].start);
            sum_fit += frac; n_fit++;
            if (frac < worst_fit) worst_fit = frac;
        }
        if (n_todo == 0 && pass > 0) break;      /* nothing new: stop early */
    }
    t_total = now_ms() - t_total;

    int hit = 0;
    for (int e = 0; e < n_exp; e++)
        for (int i = 0; i < n_seen; i++)
            if (match(seen[i], exp_txt[e])) { hit++; break; }

    const char *base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    printf("%-28s uniq=%3d truth=%3d hit=%3d | pass", base, n_seen, n_exp, hit);
    for (int p = 0; p < NPASS && p < 8; p++) printf(" %d:%d", p + 1, per_pass[p]);
    printf(" | %.0f ms | fit mean %.2f worst %.2f\n",
           t_total, n_fit ? sum_fit / n_fit : 0.0, n_fit ? worst_fit : 0.0);
    return 0;
}
