// Host harness for R028 auto speed + shift. Links the REAL main/rtty_auto.c.
//
//   gcc -std=c11 -O2 -Wall -Wextra -I main -o rtty_auto_harness test/rtty_auto_harness.c main/rtty_auto.c -lm
//   ./rtty_auto_harness                       # synthetic grid, exit 0 = pass
//   ./rtty_auto_harness --wav f.wav --centre 2210   # real recording: verdict over time
//
// The synthetic generator is independent of rtty_auto.c: continuous-phase FSK
// with random ITA2 codes, start bit, 5 data bits, and 1 / 1.5 / 2 stop bits.
// SNR in 2500 Hz (the WSJT convention, as in rtty_harness.c).

#include "rtty_auto.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static double urand(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return ((g_rng >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
static double grand(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand()); }

static rtty_auto_t g_a;

/* secs of signal: idle 0.5 s, then random characters. Returns samples. */
static int gen(float *re, float *im, int nmax, float fs, float baud, float shift, float centre,
               float stop, float snr_db)
{
    double ph = 0, t = 0; int n = 0;
    double T = fs / baud;
    double sd = snr_db > 90 ? 0 : sqrt(1.0 / pow(10.0, snr_db / 10.0) * fs / 2500.0 / 2.0);
    #define EMIT(bit, nb) do { double f = centre + ((bit) ? 0.5 : -0.5) * shift; \
        double w = 2 * M_PI * f / fs; t += (nb) * T; \
        while (n < t && n < nmax) { re[n] = (float)(cos(ph) + sd * grand()); \
            im[n] = (float)(sin(ph) + sd * grand()); ph += w; n++; } } while (0)
    EMIT(1, 0.5 * baud);
    while (n < nmax) {
        int code = (int)(urand() * 32);
        EMIT(0, 1);
        for (int j = 0; j < 5; j++) EMIT((code >> j) & 1, 1);
        EMIT(1, stop);
    }
    #undef EMIT
    return n;
}

/* Feed in 128-sample chunks; return the time (s) from which the verdict was
 * right and stayed right, or -1. */
static double settle(const float *re, const float *im, int n, float fs, float want_b, float want_s,
                     float *got_b, float *got_s)
{
    double ok_from = -1;
    for (int i = 0; i < n; i += 128) {
        int m = n - i < 128 ? n - i : 128;
        rtty_auto_feed(&g_a, re + i, im + i, m);
        /* 425 and 450 count as one: 12.5 Hz per tone, inside every
         * candidate's tone filter (see rtty_auto.h). 170/200 likewise, 15 Hz;
         * what that costs the decoder is measured separately. */
        bool s_ok = g_a.shift == want_s ||
                    (want_s >= 425 && want_s <= 450 && g_a.shift >= 425 && g_a.shift <= 450) ||
                    (want_s <= 200 && g_a.shift <= 200);   /* 170/200: see rtty_auto.c */
        bool ok = g_a.shift_ok && g_a.baud_ok && s_ok && g_a.baud == want_b;
        if (ok && ok_from < 0) ok_from = (i + m) / fs;
        if (!ok) ok_from = -1;
    }
    *got_b = g_a.baud_ok ? g_a.baud : 0;
    *got_s = g_a.shift_ok ? g_a.shift : 0;
    return ok_from;
}

/* R028 trial: quality from the decoder counters of REAL decodes of a clean
 * 75 Bd WAV (rtty_harness, 2026-11-15, 60 s): chars, framing, false starts. */
static int trial_tests(void)
{
    /* Mean character confidence of REAL decodes of a clean 75 Bd WAV at each
     * setting (rtty_demod, 2026-11-15, 594/404/237/266 chars); wrong shift
     * decodes nothing (squelched). */
    struct { const char *what; unsigned n; float mean; } d[] = {
        { "75/170 right", 594, 0.844f }, { "45.45/170", 266, 0.574f }, { "50/170", 237, 0.612f },
        { "100/170", 404, 0.651f }, { "75/850", 0, 0.0f },
    };
    float q[5]; int fails = 0;
    for (int i = 0; i < 5; i++) {
        q[i] = rtty_auto_quality(d[i].n, d[i].mean * (float)d[i].n);
        printf("quality %-12s %.2f\n", d[i].what, q[i]);
    }
    for (int i = 1; i < 5; i++) if (!rtty_auto_trial_keep(q[i], q[0])) { printf("FAIL keep %s -> right\n", d[i].what); fails++; }
    for (int i = 1; i < 5; i++) if (rtty_auto_trial_keep(q[0], q[i])) { printf("FAIL revert right -> %s\n", d[i].what); fails++; }
    if (rtty_auto_trial_keep(q[1], 0.0f)) { printf("FAIL keep on silence\n"); fails++; }
    printf("trial tests: %d failure(s)\n", fails);
    return fails;
}

static int synthetic(void)
{
    const float fs = 6000, secs = 20;
    int nmax = (int)(fs * secs);
    float *re = malloc(nmax * sizeof(float)), *im = malloc(nmax * sizeof(float));
    const float snrs[] = { 20, 0, -3, -6 };
    const float stops[] = { 1.0f, 1.5f, 2.0f };
    const float offs[] = { 0, 60 };
    int fails = 0, total = 0, below = 0;
    printf("baud  shift stop  snr  off | verdict          settle\n");
    for (int b = 0; b < RTTY_AUTO_NBAUD; b++)
    for (int s = 0; s < RTTY_AUTO_NSHIFT; s++)
    for (int st = 0; st < 3; st++)
    for (int q = 0; q < 4; q++)
    for (int o = 0; o < 2; o++) {
        float baud = rtty_auto_bauds[b], shift = rtty_auto_shifts[s];
        int n = gen(re, im, nmax, fs, baud, shift, 1500 + offs[o], stops[st], snrs[q]);
        rtty_auto_init(&g_a, fs, 1500);
        float gb, gs;
        double t = settle(re, im, n, fs, baud, shift, &gb, &gs);
        /* Required only where the DECODER copies: theory CER <= 1 %
         * (non-coherent FSK, p = 0.5 exp(-Eb/2N0), 7 elements). Below that
         * a miss is reported, not failed - auto cannot beat the decode. */
        double eb = pow(10.0, snrs[q] / 10.0) * 2500.0 / baud;
        double cer = 1.0 - pow(1.0 - 0.5 * exp(-eb / 2.0), 7);
        bool required = cer <= 0.01;
        bool pass = t >= 0 && t <= 10.0;
        if (required) { total++; if (!pass) fails++; }
        else if (!pass) below++;
        if (!pass || (st == 1 && o == 0))
            printf("%5.2f %5.0f %4.1f %4.0f %4.0f | %5.2f Bd %4.0f Hz  %s%.1f s\n", baud, shift, stops[st],
                   snrs[q], offs[o], gb, gs, pass ? "" : "FAIL ", t);
        if (!pass) {
            printf("      pair %+.0f Hz %.1f dB |", g_a.pair_centre, g_a.pair_db);
            for (int k = 0; k < RTTY_AUTO_NBAUD; k++)
                printf(" %.2f: sc %5.2f sh %4.2f n %4d |", rtty_auto_bauds[k], g_a.score[k], g_a.short_frac[k], g_a.det[k].ntrans);
            printf("\n");
        }
    }
    /* noise only: no verdict may appear */
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < nmax; i++) { re[i] = (float)grand(); im[i] = (float)grand(); }
        rtty_auto_init(&g_a, fs, 1500);
        rtty_auto_feed(&g_a, re, im, nmax);
        total++;
        if (g_a.shift_ok && g_a.baud_ok) { fails++; printf("noise %d: FALSE verdict %.2f Bd %.0f Hz\n", k, g_a.baud, g_a.shift); }
        else printf("noise %d: no verdict (shift_ok %d, pair %.1f dB)\n", k, g_a.shift_ok, g_a.pair_db);
    }
    /* Not RTTY but a pair: two steady carriers 170 Hz apart (no transitions),
     * and one carrier switched between the two tones at RANDOM times
     * (transitions with no bit clock). Neither may give a speed verdict. */
    for (int k = 0; k < 2; k++) {
        double p1 = 0, p2 = 0; int hold = 0, on = 1;
        for (int i = 0; i < nmax; i++) {
            double nr = 0.3 * grand(), ni = 0.3 * grand();
            if (k == 0) {
                re[i] = (float)(cos(p1) + cos(p2) + nr); im[i] = (float)(sin(p1) + sin(p2) + ni);
            } else {
                if (--hold <= 0) { on = !on; hold = 20 + (int)(urand() * 400); }
                double p = on ? p1 : p2;
                re[i] = (float)(cos(p) + nr); im[i] = (float)(sin(p) + ni);
            }
            p1 += 2 * M_PI * 1585 / fs; p2 += 2 * M_PI * 1415 / fs;
        }
        rtty_auto_init(&g_a, fs, 1500);
        rtty_auto_feed(&g_a, re, im, nmax);
        total++;
        if (g_a.baud_ok) { fails++; printf("%s: FALSE speed %.2f Bd\n", k ? "random keying" : "two carriers", g_a.baud); }
        else printf("%s: no speed verdict (shift_ok %d %.0f Hz, runs %d)\n", k ? "random keying" : "two carriers",
                    g_a.shift_ok, g_a.shift, g_a.ntrans);
    }
    printf("\n%d of %d required cases settled right within 10 s (%d misses below the decode limit, not counted)\n",
           total - fails, total, below);
    free(re); free(im);
    return fails ? 1 : 0;
}

static int wav(const char *path, float centre)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12) { fclose(f); return 2; }
    int ch = 0, bits = 0; uint32_t rate = 0; long data_len = -1;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char fm[40] = {0};
            if (fread(fm, 1, len < 40 ? len : 40, f)) {}
            if (len > 40) fseek(f, (long)(len - 40), SEEK_CUR);
            ch = fm[2] | fm[3] << 8;
            rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
        } else if (!memcmp(c, "data", 4)) { data_len = (long)len; break; }
        else fseek(f, (long)(len + (len & 1)), SEEK_CUR);
    }
    if (data_len < 0 || bits != 16) { fclose(f); return 2; }
    long frames = data_len / (2 * ch);
    int16_t *pcm = malloc((size_t)data_len);
    frames = (long)fread(pcm, (size_t)(2 * ch), (size_t)frames, f);
    fclose(f);
    float *x = malloc((size_t)frames * sizeof(float));
    for (long i = 0; i < frames; i++) x[i] = pcm[i * ch] / 32768.0f;
    free(pcm);
    if (rate > 12000 && rate % 6000 == 0) {   /* same decimation as rtty_harness.c */
        int D = (int)(rate / 6000), NT = 127; float hh[127]; double fc = 2800.0 / rate, hs = 0;
        for (int i = 0; i < NT; i++) {
            double m = i - (NT - 1) / 2.0, s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
            hh[i] = (float)(s * (0.42 - 0.5 * cos(2 * M_PI * i / (NT - 1)) + 0.08 * cos(4 * M_PI * i / (NT - 1))));
            hs += hh[i];
        }
        long fo = (frames - NT) / D; float *y = malloc((size_t)fo * sizeof(float));
        for (long o = 0; o < fo; o++) { double a = 0; for (int i = 0; i < NT; i++) a += hh[i] * x[o * D + i]; y[o] = (float)(a / hs); }
        free(x); x = y; frames = fo; rate = 6000;
    }
    /* real audio: im = 0. A real tone at +f is also at -f; the search spans
     * centre +/-750 Hz, so with centre >= 1000 Hz the image stays outside. */
    rtty_auto_init(&g_a, (float)rate, centre);
    long step = rate;   /* report each second */
    for (long i = 0; i < frames; i += step) {
        int m = (int)(frames - i < step ? frames - i : step);
        rtty_auto_feed(&g_a, x + i, NULL, m);
        printf("%3lds shift %s%4.0f (pair %+5.0f Hz, %4.1f dB)  baud %s%5.2f %s  trans %4d  scores",
               (i + m) / (long)rate, g_a.shift_ok ? "" : "?", g_a.shift, g_a.pair_centre, g_a.pair_db,
               g_a.baud_ok ? "" : "?", g_a.baud, g_a.mark_low ? "markLOW " : "markHIGH", g_a.ntrans);
        for (int b = 0; b < RTTY_AUTO_NBAUD; b++) printf(" %5.2f", g_a.score[b]);
        printf("\n");
    }
#ifdef RTTY_AUTO_DEBUG
    { extern int rtty_auto_dbg_hist[2][48];
      printf("45.45 Bd run lengths, quarter bits: low tone / high tone\n");
      for (int i = 1; i < 48; i++) if (rtty_auto_dbg_hist[0][i] + rtty_auto_dbg_hist[1][i])
          printf("  %5.2f bits: %5d %5d\n", i / 4.0, rtty_auto_dbg_hist[0][i], rtty_auto_dbg_hist[1][i]); }
#endif
    free(x);
    return 0;
}

int main(int argc, char **argv)
{
    const char *w = NULL; float centre = 2210;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) w = argv[++i];
        else if (!strcmp(argv[i], "--centre") && i + 1 < argc) centre = (float)atof(argv[++i]);
    }
    if (w) return wav(w, centre);
    int tf = trial_tests();
    return synthetic() | (tf ? 1 : 0);
}
