// Host harness for the RTTY RX core (T196, phase R1). Links the REAL
// main/rtty_demod.c and main/rtty_baudot.c - no copy that can drift.
//
//   gcc -std=c11 -O2 -Wall -Wextra -I main -o rtty_harness test/rtty_harness.c main/rtty_demod.c main/rtty_baudot.c -lm
//   ./rtty_harness                 # synthetic suite, exit 0 = pass
//   ./rtty_harness --wav f.wav --centre 2210 [--lsb] [--squelch 0.3]
//                                  # decode a real recording, print the text
//
// What the synthetic suite proves, and what it does not:
//   * The ITA2 table is checked against facts that do not come from the
//     table itself (RYRY is the alternating bit pattern; letter codes from
//     the bit-order form of ITA2).
//   * The demod is measured against THEORY: character error rate vs SNR,
//     next to non-coherent binary FSK, p = 0.5*exp(-Eb/2N0).
//   * ⚠ The generator uses this project's own encoder, so generator and
//     decoder share the table. A shared mistake passes here. The arbiter is
//     a REAL recording (--wav), decoded and compared with what fldigi or
//     MMTTY printed for the same audio.
//
// SNR is in 2500 Hz, the WSJT convention. Eb/N0 = SNR * 2500 / baud.

#include "rtty_baudot.h"
#include "rtty_demod.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---------- deterministic noise ---------- */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static double urand(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return ((g_rng >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
static double grand(void)
{
    return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand());
}

/* ---------- table checks ---------- */
static void test_table(void)
{
    rtty_baudot_t b;
    uint8_t out[2];
    struct { char c; uint8_t code; } known[] = {
        /* value = ITA2 bits 1..5 read LSB first; bit-order form in comments */
        {'A', 3},   /* 11000 */  {'B', 25},  /* 10011 */  {'E', 1},   /* 10000 */
        {'Q', 23},  /* 11101 */  {'T', 16},  /* 00001 */  {'Z', 17},  /* 10001 */
        {'M', 28},  /* 00111 */  {'R', 10},  /* 01010 */  {'Y', 21},  /* 10101 */
        {' ', 4},   /* 00100 */
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        rtty_baudot_init(&b, false);
        int n = rtty_baudot_encode(&b, known[i].c, out);
        CHECK(n == 1 && out[0] == known[i].code, "encode '%c' -> n=%d code=%d, want %d",
              known[i].c, n, n > 0 ? out[0] : -1, known[i].code);
    }
    /* RY: start(0) R(0,1,0,1,0) stop(1) start(0) Y(1,0,1,0,1) stop(1). From
     * R's first data bit to Y's last data bit, every element alternates - the
     * reason RYRY is the classic test pattern. (Y's last bit and its stop are
     * both mark, so the check stops before the stop bit.) */
    {
        int bits[14], k = 0;
        rtty_baudot_init(&b, false);
        const char *s = "RY";
        for (int i = 0; i < 2; i++) {
            rtty_baudot_encode(&b, s[i], out);
            bits[k++] = 0;
            for (int j = 0; j < 5; j++) bits[k++] = (out[0] >> j) & 1;
            bits[k++] = 1;
        }
        int alt = 1;
        for (int i = 2; i < 13; i++) if (bits[i] == bits[i - 1]) alt = 0;
        CHECK(alt, "RY is not an alternating bit pattern");
    }
    /* Digits: the QWERTYUIOP row in FIGS. */
    {
        const char *row = "QWERTYUIOP", *dig = "1234567890";
        for (int i = 0; i < 10; i++) {
            rtty_baudot_t e; rtty_baudot_init(&e, false);
            uint8_t lc[2], dc[2];
            rtty_baudot_encode(&e, row[i], lc);
            rtty_baudot_init(&e, false);
            int n = rtty_baudot_encode(&e, dig[i], dc);
            CHECK(n == 2 && dc[0] == RTTY_CODE_FIGS && dc[1] == lc[0],
                  "digit %c is not FIGS+%c", dig[i], row[i]);
        }
    }
    /* Round trip every encodable printable character. */
    for (int c = 32; c < 127; c++) {
        rtty_baudot_t e, d;
        rtty_baudot_init(&e, false); rtty_baudot_init(&d, false);
        uint8_t o[2];
        int n = rtty_baudot_encode(&e, (char)c, o);
        if (n < 0) continue;
        int got = 0;
        for (int i = 0; i < n; i++) { int r = rtty_baudot_decode(&d, o[i]); if (r) got = r; }
        int want = (c >= 'a' && c <= 'z') ? c - 32 : c;
        CHECK(got == want, "round trip '%c' -> '%c'", c, got);
    }
    /* USOS: a space after FIGS returns the decoder to LTRS. */
    {
        rtty_baudot_t d; rtty_baudot_init(&d, true);
        rtty_baudot_decode(&d, RTTY_CODE_FIGS);
        rtty_baudot_decode(&d, 4);
        CHECK(rtty_baudot_decode(&d, 1) == 'E', "USOS: E after FIGS+SPACE is not E");
        rtty_baudot_init(&d, false);
        rtty_baudot_decode(&d, RTTY_CODE_FIGS);
        rtty_baudot_decode(&d, 4);
        CHECK(rtty_baudot_decode(&d, 1) == '3', "no USOS: E after FIGS+SPACE is not 3");
    }
}

/* ---------- signal generator ---------- */
typedef struct {
    float *re, *im;
    size_t n, cap;
} sig_t;

static void sig_push(sig_t *s, float r, float i)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 65536;
        s->re = realloc(s->re, s->cap * sizeof(float));
        s->im = realloc(s->im, s->cap * sizeof(float));
    }
    s->re[s->n] = r; s->im[s->n] = i; s->n++;
}

typedef struct {
    float fs, baud, shift, centre;
    float baud_err;    /* fractional, e.g. +0.01 = sender 1 % fast */
    float f_off;       /* Hz, sender off tune */
    float snr_db;      /* in 2500 Hz; > 90 = no noise */
} gen_t;

static float g_drift;        /* sender drift, Hz/s, from t = 0 */

/* Continuous-phase FSK: unit amplitude, mark = centre + shift/2. */
static void gen_elements(sig_t *s, const gen_t *g, const int *bits, const float *len,
                         int nel, double *phase, double *tacc)
{
    double T = g->fs / (g->baud * (1.0 + g->baud_err));
    for (int e = 0; e < nel; e++) {
        double f = g->centre + g->f_off + g_drift * (*tacc / g->fs)
                 + (bits[e] ? 0.5 : -0.5) * g->shift;
        double w = 2.0 * M_PI * f / g->fs;
        *tacc += len[e] * T;
        while ((double)s->n < *tacc) {
            sig_push(s, (float)cos(*phase), (float)sin(*phase));
            *phase += w;
            if (*phase > 2 * M_PI) *phase -= 2 * M_PI;
        }
    }
}

static void gen_code(sig_t *s, const gen_t *g, uint8_t code, double *ph, double *t)
{
    int bits[7]; float len[7];
    bits[0] = 0; len[0] = 1.0f;
    for (int j = 0; j < 5; j++) { bits[1 + j] = (code >> j) & 1; len[1 + j] = 1.0f; }
    bits[6] = 1; len[6] = 1.5f;
    gen_elements(s, g, bits, len, 7, ph, t);
}

static void gen_mark(sig_t *s, const gen_t *g, float nbits, double *ph, double *t)
{
    int b = 1; float l = nbits;
    gen_elements(s, g, &b, &l, 1, ph, t);
}

static void add_noise(sig_t *s, const gen_t *g)
{
    if (g->snr_db > 90) return;
    /* complex noise, total variance sigma2 over fs; in 2500 Hz: sigma2*2500/fs */
    double snr = pow(10.0, g->snr_db / 10.0);
    double sigma2 = 1.0 / snr * g->fs / 2500.0;
    double sd = sqrt(sigma2 / 2.0);
    for (size_t i = 0; i < s->n; i++) {
        s->re[i] += (float)(sd * grand());
        s->im[i] += (float)(sd * grand());
    }
}

/* Build a transmission of `text`: 1 s mark idle, 8 LTRS diddles, text, 1 s idle. */
static sig_t make_signal(const gen_t *g, const char *text)
{
    sig_t s = {0};
    double ph = 0, t = 0;
    rtty_baudot_t e; rtty_baudot_init(&e, false);
    gen_mark(&s, g, g->baud * 1.0f, &ph, &t);
    for (int i = 0; i < 8; i++) gen_code(&s, g, RTTY_CODE_LTRS, &ph, &t);
    for (const char *p = text; *p; p++) {
        uint8_t o[2];
        int n = rtty_baudot_encode(&e, *p, o);
        for (int i = 0; i < n; i++) gen_code(&s, g, o[i], &ph, &t);
    }
    gen_mark(&s, g, g->baud * 1.0f, &ph, &t);
    add_noise(&s, g);
    return s;
}

/* ---------- decode + score ---------- */
typedef struct { char *buf; size_t n, cap; rtty_baudot_t b; } sink_t;

static void on_char(const rtty_char_t *c, void *user)
{
    sink_t *k = user;
    int ch = rtty_baudot_decode(&k->b, c->code);
    if (!ch || ch == '\r' || ch == '\n' || ch == '\a') return;
    if (k->n + 2 > k->cap) { k->cap = k->cap ? k->cap * 2 : 4096; k->buf = realloc(k->buf, k->cap); }
    k->buf[k->n++] = (char)ch; k->buf[k->n] = 0;
}

static size_t lev(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    size_t *p = malloc((nb + 1) * sizeof(size_t)), *q = malloc((nb + 1) * sizeof(size_t));
    for (size_t j = 0; j <= nb; j++) p[j] = j;
    for (size_t i = 1; i <= na; i++) {
        q[0] = i;
        for (size_t j = 1; j <= nb; j++) {
            size_t c = p[j - 1] + (a[i - 1] != b[j - 1]);
            if (p[j] + 1 < c) c = p[j] + 1;
            if (q[j - 1] + 1 < c) c = q[j - 1] + 1;
            q[j] = c;
        }
        size_t *t = p; p = q; q = t;
    }
    size_t r = p[nb];
    free(p); free(q);
    return r;
}

typedef struct { double cer; rtty_demod_t st; char *text; } res_t;

static rtty_demod_t g_dem;   /* 8 KB: keep it off the stack */
static float g_afc;          /* cfg.afc_hz for run(); 0 = off */

static double run(const gen_t *g, const char *text, bool rx_reverse, float rx_centre_err,
                  float squelch, rtty_demod_t *stats_out)
{
    sig_t s = make_signal(g, text);
    rtty_demod_cfg_t cfg = { g->fs, 45.45f, 170.0f, g->centre + rx_centre_err, rx_reverse, squelch, g_afc };
    if (!rtty_demod_init(&g_dem, &cfg)) { printf("init failed\n"); exit(2); }
    sink_t k = {0};
    rtty_baudot_init(&k.b, false);
    /* feed in odd-sized blocks: block boundaries must not matter */
    for (size_t i = 0; i < s.n; i += 333) {
        int n = (int)((s.n - i) < 333 ? (s.n - i) : 333);
        rtty_demod_feed(&g_dem, &s.re[i], &s.im[i], n, on_char, &k);
    }
    const char *got = k.buf ? k.buf : "";
    double cer = (double)lev(text, got) / (double)strlen(text);
    if (stats_out) *stats_out = g_dem;
    free(s.re); free(s.im); free(k.buf);
    return cer;
}

static char *random_text(size_t nchar)
{
    static const char *words[] = { "CQ", "TEST", "DE", "OZ1LAV", "599", "TU", "QRZ", "UR",
        "NAME", "STEFFEN", "QTH", "JO65", "RST", "73", "K", "BK", "RTTY", "HW", "CPY",
        "THE", "QUICK", "BROWN", "FOX", "JUMPS", "OVER", "LAZY", "DOG", "1234567890",
        "?", "/", "-", ".", "," };
    char *t = malloc(nchar + 32);
    size_t n = 0;
    while (n < nchar) {
        const char *w = words[(size_t)(urand() * (sizeof words / sizeof words[0]))];
        size_t l = strlen(w);
        memcpy(&t[n], w, l); n += l;
        t[n++] = ' ';
    }
    t[n] = 0;
    return t;
}

static double theory_cer(double snr_db, double baud)
{
    double ebn0 = pow(10.0, snr_db / 10.0) * 2500.0 / baud;
    double p = 0.5 * exp(-ebn0 / 2.0);
    return 1.0 - pow(1.0 - p, 7.0);
}

static void suite(void)
{
    test_table();
    printf("table checks: %s\n", g_fail ? "FAIL" : "pass");

    char *text = random_text(1500);
    const float rates[2] = { 6000.0f, 12000.0f };   /* 6 kHz = rx_audio stage-1 rate */

    for (int r = 0; r < 2; r++) {
        gen_t g = { rates[r], 45.45f, 170.0f, 0.0f, 0, 0, 99 };
        rtty_demod_t st;
        double cer = run(&g, text, false, 0, 0, &st);
        printf("clean fs=%5.0f centre 0: CER %.4f  chars %u framing %u\n",
               rates[r], cer, (unsigned)st.n_chars, (unsigned)st.n_framing);
        CHECK(cer == 0.0, "clean decode at fs=%.0f has CER %.4f", rates[r], cer);
        g.centre = 1000.0f;   /* off-centre, as on the device */
        cer = run(&g, text, false, 0, 0, NULL);
        CHECK(cer == 0.0, "clean decode at fs=%.0f centre 1000 has CER %.4f", rates[r], cer);
    }

    printf("\nSNR sweep, fs=6000 (CER measured | theory, non-coherent FSK):\n");
    double cer_m6 = 1;
    for (int snr = -16; snr <= -2; snr += 2) {
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, (float)snr };
        double cer = run(&g, text, false, 0, 0, NULL);
        printf("  SNR %+3d dB (Eb/N0 %4.1f dB): CER %6.3f | %6.3f\n", snr,
               snr + 10 * log10(2500.0 / 45.45), cer, theory_cer(snr, 45.45));
        if (snr == -6) cer_m6 = cer;
    }
    CHECK(cer_m6 < 0.03, "CER at -6 dB is %.3f, want < 0.03", cer_m6);

    printf("\nimpairments at SNR -6 dB:\n");
    const float offs[] = { -20, -10, 10, 20 };
    for (int i = 0; i < 4; i++) {
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, offs[i], -6 };
        double cer = run(&g, text, false, 0, 0, NULL);
        printf("  tuning error %+3.0f Hz: CER %.3f\n", offs[i], cer);
        if (fabsf(offs[i]) <= 10) CHECK(cer < 0.05, "CER %.3f at %+.0f Hz", cer, offs[i]);
    }
    printf("\nAFC on (+/-150 Hz, with acquisition) at SNR -6 dB:\n");
    g_afc = 150.0f;
    {
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, -6 };
        rtty_demod_t st;
        double cer = run(&g, text, false, 0, 0, &st);
        printf("  tuning error   0 Hz: CER %.3f (AFC off %.3f) offset end %+5.1f Hz\n", cer, cer_m6, st.afc_off);
        CHECK(cer < cer_m6 + 0.01, "AFC costs at 0 Hz: CER %.3f vs %.3f", cer, cer_m6);
    }
    const float offa[] = { -140, -100, -40, -10, 10, 40, 100, 140 };
    for (int i = 0; i < 8; i++) {
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, offa[i], -6 };
        rtty_demod_t st;
        double cer = run(&g, text, false, 0, 0, &st);
        printf("  tuning error %+3.0f Hz: CER %.3f offset end %+5.1f Hz\n", offa[i], cer, st.afc_off);
        CHECK(cer < 0.04, "AFC: CER %.3f at %+.0f Hz", cer, offa[i]);
    }
    {
        /* drift 0.2 Hz/s from -25 Hz: about +25 Hz at the end of the text */
        g_drift = 0.2f;
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, -25, -6 };
        rtty_demod_t st;
        double cer = run(&g, text, false, 0, 0, &st);
        g_drift = 0.0f;
        printf("  drift 0.2 Hz/s from -25 Hz: CER %.3f offset end %+5.1f Hz\n", cer, st.afc_off);
        CHECK(cer < 0.02, "AFC: CER %.3f with drift", cer);
    }
    {
        /* 60 s of noise first, then the signal on 0 Hz, same demodulator:
         * where did the offset wander, and does it come back? */
        sig_t n = {0};
        for (int i = 0; i < 6000 * 60; i++) sig_push(&n, (float)grand(), (float)grand());
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, -6 };
        sig_t s = make_signal(&g, text);
        rtty_demod_cfg_t cfg = { 6000, 45.45f, 170.0f, 0, false, 0, g_afc };
        rtty_demod_init(&g_dem, &cfg);
        sink_t k = {0}; rtty_baudot_init(&k.b, false);
        rtty_demod_feed(&g_dem, n.re, n.im, (int)n.n, NULL, NULL);
        float wander = g_dem.afc_off;
        rtty_demod_feed(&g_dem, s.re, s.im, (int)s.n, on_char, &k);
        /* the noise prints junk before the signal: score the tail only */
        const char *got = k.buf ? k.buf : "";
        size_t lt = strlen(text), lg = strlen(got);
        double cer = (double)lev(text, lg > lt ? got + (lg - lt) : got) / (double)lt;
        printf("  60 s noise first: offset after noise %+5.1f Hz, then CER %.3f offset end %+5.1f Hz\n",
               wander, cer, g_dem.afc_off);
        CHECK(cer < 0.03, "AFC after noise: CER %.3f", cer);
        free(n.re); free(n.im); free(s.re); free(s.im); free(k.buf);
    }
    g_afc = 0.0f;
    const float berr[] = { -0.02f, -0.01f, 0.01f, 0.02f };
    for (int i = 0; i < 4; i++) {
        gen_t g = { 6000, 45.45f, 170.0f, 0, berr[i], 0, -6 };
        double cer = run(&g, text, false, 0, 0, NULL);
        printf("  baud error %+4.1f %%:   CER %.3f\n", berr[i] * 100, cer);
        CHECK(cer < 0.05, "CER %.3f at baud error %+.0f %%", cer, berr[i] * 100);
    }
    {
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, 99 };
        double cer = run(&g, text, true, 0, 0, NULL);
        printf("  reverse mismatch (clean): CER %.3f (must be high)\n", cer);
        CHECK(cer > 0.5, "reverse flag has no effect (CER %.3f)", cer);
    }

    printf("\nnoise only, 60 s, fs=6000 - characters printed:\n");
    for (int q = 0; q < 4; q++) {
        static const float sq[] = { 0.0f, 0.4f, 0.44f, 0.48f };
        sig_t s = {0};
        for (int i = 0; i < 6000 * 60; i++) sig_push(&s, (float)grand(), (float)grand());
        rtty_demod_cfg_t cfg = { 6000, 45.45f, 170.0f, 0, false, sq[q], g_afc };
        rtty_demod_init(&g_dem, &cfg);
        sink_t k = {0}; rtty_baudot_init(&k.b, false);
        rtty_demod_feed(&g_dem, s.re, s.im, (int)s.n, on_char, &k);
        printf("  squelch %.2f: %u chars (%u framing, %u squelched)\n", sq[q],
               (unsigned)g_dem.n_chars, (unsigned)g_dem.n_framing, (unsigned)g_dem.n_squelched);
        if (q == 3) CHECK(g_dem.n_chars <= 5, "squelch 0.48 prints %u noise chars in 60 s", (unsigned)g_dem.n_chars);
        free(s.re); free(s.im); free(k.buf);
    }
    printf("\nsquelch cost on signal at SNR -6 dB:\n");
    for (int q = 1; q < 4; q++) {
        static const float sq[] = { 0.0f, 0.4f, 0.44f, 0.48f };
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, -6 };
        double cer = run(&g, text, false, 0, sq[q], NULL);
        printf("  squelch %.2f: CER %.3f\n", sq[q], cer);
        if (q == 3) CHECK(cer < 0.02, "squelch 0.48 costs CER %.3f on signal", cer);
    }
    free(text);
}

/* ---------- real recording ---------- */
static float g_baud = 45.45f, g_shift = 170.0f;

static int decode_wav(const char *path, float centre, bool lsb, float squelch, bool usos)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        printf("%s: not a RIFF/WAVE file\n", path); fclose(f); return 2;
    }
    int ch = 0, bits = 0; uint32_t rate = 0; long data_len = -1;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char fm[40] = {0};
            size_t rd = fread(fm, 1, len < 40 ? len : 40, f);
            if (len > 40) fseek(f, (long)(len - 40), SEEK_CUR);
            (void)rd;
            ch = fm[2] | fm[3] << 8;
            rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
        } else if (!memcmp(c, "data", 4)) { data_len = (long)len; break; }
        else fseek(f, (long)(len + (len & 1)), SEEK_CUR);
    }
    if (data_len < 0 || bits != 16 || ch < 1) {
        printf("%s: need 16-bit PCM (got %d bit, %d ch)\n", path, bits, ch); fclose(f); return 2;
    }
    long frames = data_len / (2 * ch);
    int16_t *pcm = malloc((size_t)data_len);
    frames = (long)fread(pcm, (size_t)(2 * ch), (size_t)frames, f);
    fclose(f);
    float *x = malloc((size_t)frames * sizeof(float));
    for (long i = 0; i < frames; i++) x[i] = pcm[i * ch] / 32768.0f;   /* left channel */
    free(pcm);

    /* rxcap WAVs are 48 kHz: one bit is 1056 samples, past the demod's ring.
     * Low-pass (windowed sinc, 2.8 kHz, 127 taps) and decimate to 6 kHz -
     * the rate the firmware tap will run at. */
    if (rate > 12000 && rate % 6000 == 0) {
        int D = (int)(rate / 6000), NT = 127;
        float h[127]; double fc = 2800.0 / rate, hs = 0;
        for (int i = 0; i < NT; i++) {
            double m = i - (NT - 1) / 2.0;
            double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
            h[i] = (float)(s * (0.42 - 0.5 * cos(2 * M_PI * i / (NT - 1)) + 0.08 * cos(4 * M_PI * i / (NT - 1))));
            hs += h[i];
        }
        long fo = (frames - NT) / D;
        float *y = malloc((size_t)(fo > 0 ? fo : 1) * sizeof(float));
        for (long o = 0; o < fo; o++) {
            double a = 0; const float *p = &x[o * D];
            for (int i = 0; i < NT; i++) a += h[i] * p[i];
            y[o] = (float)(a / hs);
        }
        free(x); x = y; frames = fo; rate = 6000;
    }

    /* Real audio: mark is the higher RF frequency. In USB audio that is the
     * higher tone; in LSB audio the lower one, so LSB = reverse. */
    rtty_demod_cfg_t cfg = { (float)rate, g_baud, g_shift, centre, lsb, squelch, g_afc };
    if (!rtty_demod_init(&g_dem, &cfg)) { printf("init failed for %u Hz\n", rate); return 2; }
    sink_t k = {0}; rtty_baudot_init(&k.b, usos);
    rtty_demod_feed(&g_dem, x, NULL, (int)frames, on_char, &k);
    printf("%s: %u Hz, %.1f s, centre %.0f Hz %s\n", path, rate, frames / (double)rate,
           centre, lsb ? "(LSB: mark low)" : "(USB: mark high)");
    printf("chars %u, framing errors %u, false starts %u, squelched %u, mark %.4f space %.4f, AFC %+.1f Hz\n",
           (unsigned)g_dem.n_chars, (unsigned)g_dem.n_framing, (unsigned)g_dem.n_false_start,
           (unsigned)g_dem.n_squelched, g_dem.mark_lvl, g_dem.space_lvl, g_dem.afc_off);
    printf("---\n%s\n---\n", k.buf ? k.buf : "");
    free(x); free(k.buf);
    return 0;
}

int main(int argc, char **argv)
{
    const char *wav = NULL; float centre = 2210; bool lsb = false, usos = true; float sq = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--centre") && i + 1 < argc) centre = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--lsb")) lsb = true;
        else if (!strcmp(argv[i], "--no-usos")) usos = false;
        else if (!strcmp(argv[i], "--squelch") && i + 1 < argc) sq = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--afc") && i + 1 < argc) g_afc = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--baud") && i + 1 < argc) g_baud = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--shift") && i + 1 < argc) g_shift = (float)atof(argv[++i]);
    }
    if (wav) return decode_wav(wav, centre, lsb, sq, usos);
    suite();
    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
