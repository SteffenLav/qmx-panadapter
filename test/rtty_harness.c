// Host harness for the RTTY RX core (T196, phase R1). Links the REAL
// main/rtty_demod.c and main/rtty_baudot.c - no copy that can drift.
//
//   gcc -std=c11 -O2 -Wall -Wextra -I main -o rtty_harness test/rtty_harness.c main/rtty_demod.c main/rtty_baudot.c main/rtty_tx_core.c -lm
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
/* min(mark, space) dB of every 0.5 s acquisition block in the last run() */
static float g_tone_min[4096];
static int   g_tone_n;
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

static double run(const gen_t *g, const char *text, bool rx_reverse, float rx_centre_err,
                  float squelch, rtty_demod_t *stats_out)
{
    sig_t s = make_signal(g, text);
    rtty_demod_cfg_t cfg = { g->fs, 45.45f, 170.0f, g->centre + rx_centre_err, rx_reverse, squelch, g_afc };
    if (!rtty_demod_init(&g_dem, &cfg)) { printf("init failed\n"); exit(2); }
    sink_t k = {0};
    rtty_baudot_init(&k.b, false);
    /* feed in odd-sized blocks: block boundaries must not matter */
    g_tone_n = 0;
    int blocks = 0;
    for (size_t i = 0; i < s.n; i += 333) {
        int n = (int)((s.n - i) < 333 ? (s.n - i) : 333);
        rtty_demod_feed(&g_dem, &s.re[i], &s.im[i], n, on_char, &k);
        if (g_dem.acq_blocks != blocks && g_tone_n < 4096) {
            blocks = g_dem.acq_blocks;
            g_tone_min[g_tone_n++] = fminf(g_dem.mark_snr_db, g_dem.space_snr_db);
        }
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

/* R012: mean locked period g_P against the sender's true period, sampled at
 * every grid slot, as a fraction (0 = exact). *sd_out = slot-to-slot spread.
 * The note "settles ~0.15 % short at -6 dB" was one reading of this jitter:
 * sd is 0.12 % at -6 dB, the mean is within +/-0.001 % (4 seeds, 2026-11-12,
 * same on the R001 code). Loop gains: steer() is type 2, so no bias is due. */
static double period_bias(const gen_t *g, const char *text, double *sd_out)
{
    sig_t s = make_signal(g, text);
    rtty_demod_cfg_t cfg = { g->fs, 45.45f, 170.0f, g->centre, false, 0, 0 };
    if (!rtty_demod_init(&g_dem, &cfg)) { printf("init failed\n"); exit(2); }
    double want = 1.0 / (1.0 + g->baud_err);
    double sum = 0, sq = 0; long n = 0; uint32_t grid0 = 0;
    for (size_t i = 0; i < s.n; i += 333) {
        int m = (int)((s.n - i) < 333 ? (s.n - i) : 333);
        rtty_demod_feed(&g_dem, &s.re[i], &s.im[i], m, NULL, NULL);
        if (g_dem.g_lock && g_dem.n_grid != grid0) {
            grid0 = g_dem.n_grid;
            double r = g_dem.g_P / (g_dem.g_bits * g_dem.T) / want - 1.0;
            sum += r; sq += r * r; n++;
        }
    }
    free(s.re); free(s.im);
    double mu = n ? sum / n : 1.0;
    if (sd_out) *sd_out = n ? sqrt(sq / n - mu * mu) : 0.0;
    return mu;
}

static double theory_cer(double snr_db, double baud)
{
    double ebn0 = pow(10.0, snr_db / 10.0) * 2500.0 / baud;
    double p = 0.5 * exp(-ebn0 / 2.0);
    return 1.0 - pow(1.0 - p, 7.0);
}

/* QRM: the wanted signal (g, with its noise) plus a second RTTY station,
 * noise-free, d_hz away and rel_db stronger, sending other text. */
static double run_qrm(const gen_t *g, const char *text, float d_hz, float rel_db)
{
    sig_t s = make_signal(g, text);
    gen_t gi = *g;
    gi.centre += d_hz; gi.snr_db = 99; gi.baud_err = 0.003f;
    char *other = random_text(strlen(text) + 200);
    sig_t q = make_signal(&gi, other);
    float a = powf(10.0f, rel_db / 20.0f);
    for (size_t i = 0; i < s.n && i < q.n; i++) { s.re[i] += a * q.re[i]; s.im[i] += a * q.im[i]; }
    rtty_demod_cfg_t cfg = { g->fs, 45.45f, 170.0f, g->centre, false, 0, g_afc };
    rtty_demod_init(&g_dem, &cfg);
    sink_t k = {0};
    rtty_baudot_init(&k.b, false);
    rtty_demod_feed(&g_dem, s.re, s.im, (int)s.n, on_char, &k);
    double cer = (double)lev(text, k.buf ? k.buf : "") / (double)strlen(text);
    free(s.re); free(s.im); free(q.re); free(q.im); free(k.buf); free(other);
    return cer;
}


/* ---------- R010: transmit core round trip ---------- */
#include "rtty_tx_core.h"

/* Run the core to the end of the over and render its codes as FSK the way the
 * device sequencer keys them: 0.5 s mark, then one code per slot, 0.2 s tail. */
static sig_t tx_render(rtty_txc_t *c, const gen_t *g, int idle_max, int *ncodes, int *ndiddle)
{
    sig_t s = {0};
    double ph = 0, t = 0;
    gen_mark(&s, g, g->baud * 0.5f, &ph, &t);
    *ncodes = 0; *ndiddle = 0;
    int code;
    while ((code = rtty_txc_next(c, idle_max)) >= 0 && *ncodes < 20000) {
        if (code == RTTY_CODE_LTRS && c->len == 0 && c->ipend >= c->npend && c->preamble == 0) (*ndiddle)++;
        gen_code(&s, g, (uint8_t)code, &ph, &t);
        (*ncodes)++;
    }
    gen_mark(&s, g, g->baud * 0.2f, &ph, &t);
    add_noise(&s, g);
    return s;
}

static char *tx_decode(const gen_t *g, sig_t *s)
{
    rtty_demod_cfg_t cfg = { g->fs, g->baud, g->shift, g->centre, false, RTTY_SQ_OPEN, 0 };
    rtty_demod_init(&g_dem, &cfg);
    sink_t k = {0}; rtty_baudot_init(&k.b, true);
    rtty_demod_feed(&g_dem, s->re, s->im, (int)s->n, on_char, &k);
    return k.buf ? k.buf : strdup("");
}

static void test_tx_core(void)
{
    printf("\nR010 transmit core -> FSK -> demod:\n");
    static rtty_txc_t c;
    gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, 99 };
    int nc, nd;

    /* 1. Figures and letters across spaces (USOS), CR LF, a dropped char. */
    const char *msg = "CQ TEST DE OZ1LAV 599 073-073 TU 5NN? 1A2B3C\n";
    const char *want = "CQ TEST DE OZ1LAV 599 073-073 TU 5NN? 1A2B3C";   /* CR/LF not printed by on_char */
    rtty_txc_init(&c);
    CHECK(rtty_txc_push(&c, msg), "push refused");
    CHECK(rtty_txc_push(&c, "~"), "push of an unencodable char refused");   /* dropped at encode */
    rtty_txc_end(&c);
    sig_t s = tx_render(&c, &g, 100, &nc, &nd);
    char *got = tx_decode(&g, &s);
    double cer = (double)lev(want, got) / (double)strlen(want);
    printf("  clean: %d codes, %d diddles, CER %.3f: \"%s\"\n", nc, nd, cer, got);
    CHECK(cer == 0.0, "TX round trip CER %.3f", cer);
    CHECK(nd == 0, "end of over sent %d diddles", nd);
    free(got); free(s.re); free(s.im);

    /* 2. Same at -6 dB, the RX test level. */
    g.snr_db = -6;
    rtty_txc_init(&c); rtty_txc_push(&c, msg); rtty_txc_end(&c);
    s = tx_render(&c, &g, 100, &nc, &nd);
    got = tx_decode(&g, &s);
    cer = (double)lev(want, got) / (double)strlen(want);
    printf("  -6 dB: CER %.3f\n", cer);
    CHECK(cer < 0.05, "TX round trip at -6 dB CER %.3f", cer);
    free(got); free(s.re); free(s.im);
    g.snr_db = 99;

    /* 3. No end: the queue runs dry, then idle_max diddles, then -1. */
    rtty_txc_init(&c); rtty_txc_push(&c, "RYRY");
    s = tx_render(&c, &g, 7, &nc, &nd);
    printf("  open over: %d codes, %d diddles (want 7)\n", nc, nd);
    CHECK(nd == 7, "diddles %d, want 7", nd);
    free(s.re); free(s.im);

    /* 4. Typing during the diddle resets the idle count and clears end. */
    rtty_txc_init(&c); rtty_txc_push(&c, "AB"); rtty_txc_end(&c);
    for (int i = 0; i < RTTY_TXC_PREAMBLE + 2; i++) rtty_txc_next(&c, 5);
    rtty_txc_push(&c, "C");
    int n_after = 0, code;
    while ((code = rtty_txc_next(&c, 5)) >= 0 && n_after < 100) n_after++;
    printf("  push after end: %d more codes (C + 5 diddles = 6)\n", n_after);
    CHECK(n_after == 6, "push after end: %d codes, want 6", n_after);

    /* 5. Cancel drops the queue; a push that does not fit is refused whole. */
    rtty_txc_init(&c);
    static char big[RTTY_TXC_CAP + 2];
    memset(big, 'E', RTTY_TXC_CAP); big[RTTY_TXC_CAP] = 0;
    CHECK(rtty_txc_push(&c, big), "full-size push refused");
    CHECK(!rtty_txc_push(&c, "X"), "overfull push accepted");
    CHECK(rtty_txc_cancel(&c) == RTTY_TXC_CAP, "cancel count");
    CHECK(rtty_txc_queued(&c) == 0, "queue not empty after cancel");
    big[RTTY_TXC_CAP - 1] = '\n';
    CHECK(!rtty_txc_push(&c, big), "push with trailing newline over cap accepted");
    printf("  queue limits: checked\n");
}

static void suite(void)
{
    test_table();
    test_tx_core();
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

    printf("\ncharacter clock period vs sender, SNR -6 dB (R012):\n");
    {
        char *pt = random_text(6000);
        const float errs[] = { 0.0f, +0.003f, -0.003f };
        for (int i = 0; i < 3; i++) {
            gen_t g = { 6000, 45.45f, 170.0f, 1500, errs[i], 0, -6 };
            double sd, mu = period_bias(&g, pt, &sd);
            printf("  sender %+.1f %%: mean %+.4f %%, slot sd %.3f %%\n", errs[i] * 100, mu * 100, sd * 100);
            /* standard error ~0.01 %: slots correlate over ~1/beta = 20 */
            CHECK(fabs(mu) < 0.0003, "clock period off by %+.4f %% (sender %+.1f %%)", mu * 100, errs[i] * 100);
        }
        free(pt);
    }

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
    {
        /* R026: THE GAP BETWEEN OVERS. Station A on 0 Hz sends, stops; in the
         * gap station B, 6 dB stronger, sends 110 Hz away (inside acquisition's
         * +/-150); A comes back. Operator, 2026-11-14: "the afc is probably too
         * sensitive - it tends to have offsprings in the break between
         * transmissions"; bench2 had afc -150 (railed) and 13 jumps after one
         * tap. The decoder must still be on A when A returns. */
        char *t1 = random_text(150), *t2 = random_text(150), *tb = random_text(150);
        gen_t ga = { 6000, 45.45f, 170.0f, 0, 0, 0, -6 };
        gen_t gb = ga; gb.f_off = 110; gb.snr_db = 0; gb.baud_err = 0.003f;
        sig_t a1 = make_signal(&ga, t1), b = make_signal(&gb, tb), a2 = make_signal(&ga, t2);
        rtty_demod_cfg_t cfg = { 6000, 45.45f, 170.0f, 0, false, RTTY_SQ_OPEN, g_afc };
        rtty_demod_init(&g_dem, &cfg);
        sink_t k1 = {0}, kb = {0}, k2 = {0};
        rtty_baudot_init(&k1.b, false); rtty_baudot_init(&kb.b, false); rtty_baudot_init(&k2.b, false);
        rtty_demod_feed(&g_dem, a1.re, a1.im, (int)a1.n, on_char, &k1);
        float off_a1 = g_dem.afc_off;
        uint32_t j0 = g_dem.n_acq;
        rtty_demod_feed(&g_dem, b.re, b.im, (int)b.n, on_char, &kb);
        float off_gap = g_dem.afc_off;
        uint32_t jumps = g_dem.n_acq - j0;
        rtty_demod_feed(&g_dem, a2.re, a2.im, (int)a2.n, on_char, &k2);
        double cer2 = (double)lev(t2, k2.buf ? k2.buf : "") / (double)strlen(t2);
        printf("  gap with QRM +110 Hz +6 dB: offset after A %+5.1f, after gap %+5.1f Hz, "
               "%u jump(s) in gap, %zu chars in gap, A again CER %.3f\n",
               off_a1, off_gap, (unsigned)jumps, kb.n, cer2);
        CHECK(jumps == 0, "%u acquisition jump(s) in the gap", (unsigned)jumps);
        CHECK(fabsf(off_gap - off_a1) <= RTTY_HOLD_HZ + 1.0f, "gap moved the decoder %+.1f -> %+.1f Hz", off_a1, off_gap);
        CHECK(cer2 < 0.05, "after the gap: CER %.3f", cer2);
        free(a1.re); free(a1.im); free(b.re); free(b.im); free(a2.re); free(a2.im);
        free(k1.buf); free(kb.buf); free(k2.buf); free(t1); free(t2); free(tb);
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

    printf("\nQRM: wanted at SNR 0 dB, a second station d Hz away (CER):\n");
    printf("  d Hz     equal   +10 dB  +20 dB\n");
    {
        static const float dd[] = { 250, -300, 400, -500 };
        static const float rr[] = { 0, 10, 20 };
        for (int i = 0; i < 4; i++) {
            printf("  %+5.0f ", dd[i]);
            for (int r = 0; r < 3; r++) {
                gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, 0 };
                printf("  %6.3f", run_qrm(&g, text, dd[i], rr[r]));
            }
            printf("\n");
        }
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
        /* 3 %, not 2 %: on 1500 characters this read 0.3 % in one run and
         * 1.6-2.1 % in others with the SAME demod (the draws moved when the
         * QRM test was added before it). Over 12000 characters it is 0.5-0.9 %. */
        if (q == 3) CHECK(cer < 0.03, "squelch 0.48 costs CER %.3f on signal", cer);
    }

    /* R014: a signal on ONE tone. Seen on bench2 2026-11-12: a ~50 Hz wide
     * non-RTTY signal under the mark line opened the squelch (sig 0.62) and
     * printed junk. A tone keyed on and off looks like mark/space to the
     * decision d = (|M|-|S|)/(|M|+|S|), because the space filter then holds
     * only noise. Three shapes, all on the mark tone, 60 s, 10 dB over the
     * 2500 Hz noise (strong, like the one seen). */
    printf("\none tone only (R014), 60 s, squelch 0.48 - characters printed:\n");
    for (int shape = 0; shape < 3; shape++) {
        static const char *nm[] = { "steady carrier", "keyed carrier (CW-like)", "31 Hz FSK at the mark" };
        const float fs = 6000.0f, fm = 85.0f, amp = sqrtf(powf(10.0f, 10.0f / 10.0f) * fs / 2500.0f);
        sig_t s = {0};
        double ph = 0.0; int left = 0; int on = 1; float df = 0.0f;
        for (int i = 0; i < (int)fs * 60; i++) {
            if (--left <= 0) {
                if (shape == 1) { on = !on; left = (int)(fs * (0.05 + 0.10 * urand())); }
                else if (shape == 2) { df = urand() < 0.5 ? -15.6f : 15.6f; left = (int)(fs / 31.25f); }
                else left = 1 << 30;
            }
            float a = (shape == 1 && !on) ? 0.0f : amp;
            ph += 2.0 * M_PI * (fm + df) / fs;
            sig_push(&s, a * (float)cos(ph) + (float)(grand() * sqrt(0.5 * fs / 2500.0)),
                         a * (float)sin(ph) + (float)(grand() * sqrt(0.5 * fs / 2500.0)));
        }
        rtty_demod_cfg_t cfg = { fs, 45.45f, 170.0f, 0, false, 0.48f, g_afc };
        rtty_demod_init(&g_dem, &cfg);
        sink_t k = {0}; rtty_baudot_init(&k.b, false);
        rtty_demod_feed(&g_dem, s.re, s.im, (int)s.n, on_char, &k);
        printf("  %-24s: %u chars (%u squelched), tones %.1f / %.1f dB\n", nm[shape],
               (unsigned)g_dem.n_chars, (unsigned)g_dem.n_squelched,
               g_dem.mark_snr_db, g_dem.space_snr_db);
        CHECK(g_dem.n_chars <= 5, "one tone (%s) prints %u chars in 60 s", nm[shape], (unsigned)g_dem.n_chars);
        free(s.re); free(s.im); free(k.buf);
    }

    /* ...and what the tone gate costs a real pair at the weak end. */
    printf("\nsquelch 0.48 on signal, both tones present (R014 cost):\n");
    for (int i = 0; i < 3; i++) {
        static const float snr[] = { -6.0f, -8.0f, -10.0f };
        gen_t g = { 6000, 45.45f, 170.0f, 0, 0, 0, snr[i] };
        rtty_demod_t st;
        double c0 = run(&g, text, false, 0, 0.0f, NULL);
        double c1 = run(&g, text, false, 0, 0.48f, &st);
        qsort(g_tone_min, (size_t)g_tone_n, sizeof(float), cmpf);
        printf("  SNR %3.0f dB: CER squelch off %.3f, on %.3f; weaker tone per 0.5 s: "
               "p1 %.1f p5 %.1f p50 %.1f dB (%d blocks)\n", snr[i], c0, c1,
               g_tone_min[g_tone_n / 100], g_tone_min[g_tone_n / 20], g_tone_min[g_tone_n / 2], g_tone_n);
        (void)st;
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
    printf("tones over floor (last 0.5 s block): mark %.1f dB, space %.1f dB\n",
           g_dem.mark_snr_db, g_dem.space_snr_db);
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
