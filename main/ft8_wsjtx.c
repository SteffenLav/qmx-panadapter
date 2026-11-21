/* ft8_wsjtx.c - WS 3.2.1's FT8 decoder ported to C. See ft8_wsjtx.h.
 *
 * Porting rules, so a later reader can check any line against the source:
 *  - Each static function names the Fortran routine it ports and keeps its
 *    variable names. Fortran arrays are 1-based; where an index is kept
 *    1-based here (tables, indexx), the comment says so.
 *  - Fortran default REAL is float, so the arithmetic is float unless the
 *    source is REAL*8 (baseline, polyfit, determ).
 *  - nint() is round-half-away-from-zero (lroundf); int assignment from a
 *    REAL truncates toward zero, as a C cast does.
 *  - four2a(...,-1,...) is a forward FFT, e^-i, unnormalised: kiss_fft's
 *    forward. +1 is the unnormalised inverse.
 *  - cshift(a,s): result(i) = a(1+mod(i-1+s,n)).
 *
 * Not ported: ft8_a7d / ft8_a8d (AP from the PREVIOUS slot's decodes - jt9
 * decoding one WAV has none), the live-timing bail-outs (ldiskdat is true for
 * a file), contest modes other than ncontest=0, and OSD's npre2 branch (only
 * reached at norder>=3; ft8b always uses norder=2).
 */
/* ⛔ COMPILED ONLY WITH -DFT8_DECODER_WSJTX. Its static buffers are ~9 MB
 * (the 180000-point subtraction and the 192000-point downsample FFTs, the
 * 372x1920 symbol spectra), which no Tab5 .bss can hold. The host tools
 * define it; the Tab5 firmware build compiles this file to nothing. */
#ifdef FT8_DECODER_WSJTX

#include "ft8_wsjtx.h"
#include "ft8_wsjtx_tables.h"

#include <complex.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fft/kiss_fft.h"
#include "fft/kiss_fftr.h"
#include "ft8/encode.h"
#include "ft8/message.h"

typedef float complex cf;

/* Every sizeable static goes to PSRAM on the Tab5 (internal RAM has no room:
 * see the internal RAM audit); host builds ignore the attribute.
 * Allocations over CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) - the kiss
 * FFT plans - land in PSRAM by themselves. */
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define WSX_BSS EXT_RAM_BSS_ATTR
#else
#define WSX_BSS
#endif

/* ft8_params.f90 */
#define KK 91
#define ND 58
#define NS 21
#define NN 79
#define NSPS 1920
#define NZ (NSPS * NN)
#define NMAX WSX_NMAX
#define NFFT1 (2 * NSPS)
#define NH1 (NFFT1 / 2)
#define NSTEP (NSPS / 4)
#define NHSYM (NMAX / NSTEP - 3)
#define NDOWN 60
#define NP2 2812
#define MAXCAND 1000
#define MAX_EARLY 200

static const int icos7[7] = {3, 1, 4, 0, 6, 5, 2};
static const int graymap[8] = {0, 1, 3, 2, 5, 6, 4, 7};

/* -DWSX_PROF: time per stage, printed by wsx_prof_print(). Host only. */
#ifdef WSX_PROF
#include <stdio.h>
#include <time.h>
static double prof_t[8]; static long prof_n[8];
static const char* prof_name[8] = {"sync8", "downsample", "ft8b-other", "bp", "osd", "subtract", "piece-try", "piece-setup"};
static double prof_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
#define PROF_BEGIN(k) double _pt##k = prof_now()
#define PROF_END(k) do { prof_t[k] += prof_now() - _pt##k; prof_n[k]++; } while (0)
void wsx_prof_print(void) { for (int i = 0; i < 8; i++) fprintf(stderr, "  %-11s %7.0f ms  %ld calls\n", prof_name[i], 1e3 * prof_t[i], prof_n[i]); }
#else
#define PROF_BEGIN(k)
#define PROF_END(k)
void wsx_prof_print(void) {}
#endif

static int nint(float x) { return (int)lroundf(x); }

/* ------------------------------------------------------------------ FFT
 * One cached kiss plan per (size, direction). Sizes used: real 3840,
 * 180000, 192000; complex 32, 3200, 180000. */
typedef struct { int n, inv, real; void* cfg; } plan_t;
static plan_t plans[12];
static void* plan_get(int n, int inv, int real)
{
    for (int i = 0; i < 12; i++)
        if (plans[i].cfg && plans[i].n == n && plans[i].inv == inv && plans[i].real == real)
            return plans[i].cfg;
    for (int i = 0; i < 12; i++)
        if (!plans[i].cfg) {
            plans[i].n = n; plans[i].inv = inv; plans[i].real = real;
            plans[i].cfg = real ? (void*)kiss_fftr_alloc(n, inv, NULL, NULL)
                                : (void*)kiss_fft_alloc(n, inv, NULL, NULL);
            return plans[i].cfg;
        }
    abort();
}
/* four2a(x,n,1,-1,0): real in[n] -> out[0..n/2] */
static void r2c(const float* in, cf* out, int n)
{
    kiss_fftr((kiss_fftr_cfg)plan_get(n, 0, 1), in, (kiss_fft_cpx*)out);
}
/* four2a(c,n,1,isign,1): complex in place */
static void c2c(cf* c, int n, int isign)
{
    kiss_fft((kiss_fft_cfg)plan_get(n, isign > 0, 0), (kiss_fft_cpx*)c, (kiss_fft_cpx*)c);
}

/* ----------------------------------------------------------- small helpers */

/* indexx.f90 (Numerical Recipes). arr and indx are 1-based: arr[1..n]. */
static void indexx(const float* arr, int n, int* indx)
{
    enum { M = 7, NSTACK = 50 };
    int i, indxt, ir, itemp, j, jstack, k, l, istack[NSTACK + 1];
    float a;
    for (j = 1; j <= n; j++) indx[j] = j;
    jstack = 0; l = 1; ir = n;
    for (;;) {
        if (ir - l < M) {
            for (j = l + 1; j <= ir; j++) {
                indxt = indx[j]; a = arr[indxt];
                for (i = j - 1; i >= 1; i--) {
                    if (arr[indx[i]] <= a) goto two;
                    indx[i + 1] = indx[i];
                }
                i = 0;
two:            indx[i + 1] = indxt;
            }
            if (jstack == 0) return;
            ir = istack[jstack]; l = istack[jstack - 1]; jstack -= 2;
        } else {
            k = (l + ir) / 2;
            itemp = indx[k]; indx[k] = indx[l + 1]; indx[l + 1] = itemp;
            if (arr[indx[l + 1]] > arr[indx[ir]]) { itemp = indx[l + 1]; indx[l + 1] = indx[ir]; indx[ir] = itemp; }
            if (arr[indx[l]] > arr[indx[ir]]) { itemp = indx[l]; indx[l] = indx[ir]; indx[ir] = itemp; }
            if (arr[indx[l + 1]] > arr[indx[l]]) { itemp = indx[l + 1]; indx[l + 1] = indx[l]; indx[l] = itemp; }
            i = l + 1; j = ir; indxt = indx[l]; a = arr[indxt];
            for (;;) {
                do i++; while (arr[indx[i]] < a);
                do j--; while (arr[indx[j]] > a);
                if (j < i) break;
                itemp = indx[i]; indx[i] = indx[j]; indx[j] = itemp;
            }
            indx[l] = indx[j]; indx[j] = indxt;
            jstack += 2;
            if (jstack > NSTACK) abort();
            if (ir - i + 1 >= j - l) { istack[jstack] = ir; istack[jstack - 1] = i; ir = j - 1; }
            else { istack[jstack] = j - 1; istack[jstack - 1] = l; l = i; }
        }
    }
}

static int cmp_float(const void* a, const void* b)
{
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}
/* pctile.f90 (shell sort; any correct sort gives the same value) */
static float pctile(const float* x, int npts, int npct)
{
    if (npts < 0 || npct < 0 || npct > 100) return 1.0f;
    float* tmp = malloc(sizeof(float) * (npts > 0 ? npts : 1));
    memcpy(tmp, x, sizeof(float) * npts);
    qsort(tmp, npts, sizeof(float), cmp_float);
    int j = nint((float)npts * 0.01f * (float)npct);
    if (j < 1) j = 1;
    if (j > npts) j = npts;
    float r = tmp[j - 1];
    free(tmp);
    return r;
}

/* determ.f90: array is 10x10, a[i][j] = array(i+1,j+1); destroys it */
static double determ(double a[10][10], int norder)
{
    double d = 1.0;
    for (int k = 0; k < norder; k++) {
        if (a[k][k] == 0) {
            int j;
            for (j = k; j < norder; j++)
                if (a[k][j] != 0) break;
            if (j == norder) return 0.0;
            for (int i = k; i < norder; i++) { double s8 = a[i][j]; a[i][j] = a[i][k]; a[i][k] = s8; }
            d = -1.0 * d;
        }
        d = d * a[k][k];
        if (k < norder - 1)
            for (int i = k + 1; i < norder; i++)
                for (int j = k + 1; j < norder; j++)
                    a[i][j] = a[i][j] - a[i][k] * a[k][j] / a[k][k];
    }
    return d;
}

/* polyfit.f90, mode 0 (unit weights) - the only mode baseline uses */
static void polyfit(const double* x, const double* y, int npts, int nterms, double* a)
{
    double sumx[10] = {0}, sumy[10] = {0}, array[10][10];
    int nmax = 2 * nterms - 1;
    for (int i = 0; i < npts; i++) {
        double xi = x[i], yi = y[i], xterm = 1.0, yterm = yi;
        for (int n = 0; n < nmax; n++) { sumx[n] += xterm; xterm *= xi; }
        for (int n = 0; n < nterms; n++) { sumy[n] += yterm; yterm *= xi; }
    }
    for (int j = 0; j < nterms; j++)
        for (int k = 0; k < nterms; k++) array[j][k] = sumx[j + k];
    double delta = determ(array, nterms);
    if (delta == 0) { for (int l = 0; l < nterms; l++) a[l] = 0; return; }
    for (int l = 0; l < nterms; l++) {
        for (int j = 0; j < nterms; j++) {
            for (int k = 0; k < nterms; k++) array[j][k] = sumx[j + k];
            array[j][l] = sumy[j];
        }
        a[l] = determ(array, nterms) / delta;
    }
}

/* platanh.f90 */
static float platanh(float x)
{
    int isign = +1;
    float z = x;
    if (x < 0) { isign = -1; z = fabsf(x); }
    if (z <= 0.664f) return x / 0.83f;
    if (z <= 0.9217f) return isign * (z - 0.4064f) / 0.322f;
    if (z <= 0.9951f) return isign * (z - 0.8378f) / 0.0524f;
    if (z <= 0.9998f) return isign * (z - 0.9914f) / 0.0012f;
    return isign * 7.0f;
}

/* get_crc14.f90: mc[0..len-1] bits; returns the 14-bit remainder */
static int get_crc14(const int8_t* mc, int len)
{
    static const int8_t p[15] = {1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1};
    int8_t r[15];
    memcpy(r, mc, 15);
    for (int i = 0; i <= len - 15; i++) {
        r[14] = mc[i + 14];
        int8_t r1 = r[0];
        for (int q = 0; q < 15; q++) r[q] = (int8_t)((r[q] + r1 * p[q]) % 2);
        int8_t t = r[0];                                /* cshift(r,1) */
        memmove(r, r + 1, 14);
        r[14] = t;
    }
    int ncrc = 0;
    for (int q = 0; q < 14; q++) ncrc = (ncrc << 1) | r[q];
    return ncrc;
}
static int crc_bad(const int8_t* cw91)  /* decode174_91's m96 check */
{
    int8_t m96[96] = {0};
    memcpy(m96, cw91, 77);
    memcpy(m96 + 82, cw91 + 77, 14);
    return get_crc14(m96, 96) != 0;
}

/* ------------------------------------------------- LDPC (174,91) encoder
 * encode174_91_nocrc.f90: generator rows from the hex table. */
static WSX_BSS int8_t gen_par[83][91];   /* gen(i,icol), 0-based */
static void gen_init(void)
{
    static int done;
    if (done) return;
    for (int i = 0; i < 83; i++)
        for (int j = 0; j < 23; j++) {
            char c = WSX_G[i][j];
            int istr = (c <= '9') ? c - '0' : (c | 0x20) - 'a' + 10;
            int ibmax = (j == 22) ? 3 : 4;
            for (int jj = 1; jj <= ibmax; jj++) {
                int icol = j * 4 + jj;                  /* 1-based */
                gen_par[i][icol - 1] = (istr >> (4 - jj)) & 1;
            }
        }
    done = 1;
}
static void encode174_91_nocrc(const int8_t* message, int8_t* codeword)
{
    gen_init();
    memcpy(codeword, message, KK);
    for (int i = 0; i < 83; i++) {
        int nsum = 0;
        for (int j = 0; j < KK; j++) nsum += message[j] * gen_par[i][j];
        codeword[KK + i] = (int8_t)(nsum % 2);
    }
}

/* -------------------------------------------------------------- osd174_91 */
#define N 174
static WSX_BSS int8_t osd_gen[KK][N];
static void osd_gen_init(void)
{
    static int done;
    if (done) return;
    /* k = 91: message91(78:k) = 0 wipes the CRC bits again, so every row is
     * just the code's encoding of a unit message. */
    for (int i = 0; i < KK; i++) {
        int8_t m[KK] = {0};
        m[i] = 1;
        encode174_91_nocrc(m, osd_gen[i]);
    }
    done = 1;
}

/* nextpat91.f90; mi[1..k] */
static void nextpat91(int8_t* mi, int k, int iorder, int* iflag)
{
    int8_t ms[KK + 1];
    int ind = -1;
    for (int i = 1; i <= k - 1; i++)
        if (mi[i] == 0 && mi[i + 1] == 1) ind = i;
    if (ind < 0) { *iflag = ind; return; }
    memset(ms, 0, sizeof ms);
    for (int i = 1; i <= ind - 1; i++) ms[i] = mi[i];
    ms[ind] = 1;
    ms[ind + 1] = 0;
    if (ind + 1 < k) {
        int s = 0;
        for (int i = 1; i <= k; i++) s += ms[i];
        int nz = iorder - s;
        for (int i = k - nz + 1; i <= k; i++) ms[i] = 1;
    }
    memcpy(mi, ms, k + 1);
    for (int i = 1; i <= k; i++)
        if (mi[i] == 1) { *iflag = i; break; }
}

#ifdef OSD_INT8
static WSX_BSS int8_t genmrb[KK + 1][N + 1];    /* 1-based, (row i, column) */
#define GMRB(r, c) genmrb[r][c]

/* mrbencode91.f90: codeword[1..N] = XOR of genmrb rows where me[i]=1 */
static void mrbencode91(const int8_t* me, int8_t* codeword, int k)
{
    memset(codeword, 0, N + 1);
    for (int i = 1; i <= k; i++)
        if (me[i] == 1)
            for (int c = 1; c <= N; c++) codeword[c] ^= genmrb[i][c];
}
#else
/* ⭐ The same matrix, one BIT per element: row r is 174 bits in three words,
 * column c (1-based) is bit c-1. It is GF(2) arithmetic either way, so every
 * XOR, swap and test gives the identical result - on 64 elements at once.
 * Measured 2026-11-22: OSD was half the cost of a piece-pass candidate on
 * the host (the int8 Gaussian elimination and the 91 re-encodings), and the
 * piece pass is time-bound on the P4. The int8 original stays behind
 * -DOSD_INT8 for the byte-identical A/B (test/ft8_piece_harness.c). */
static WSX_BSS uint64_t gpk[KK + 1][3];
#define GMRB(r, c) ((int8_t)((gpk[r][((c) - 1) >> 6] >> (((c) - 1) & 63)) & 1u))

static void mrbencode91(const int8_t* me, int8_t* codeword, int k)
{
    uint64_t w0 = 0, w1 = 0, w2 = 0;
    for (int i = 1; i <= k; i++)
        if (me[i] == 1) { w0 ^= gpk[i][0]; w1 ^= gpk[i][1]; w2 ^= gpk[i][2]; }
    codeword[0] = 0;
    for (int c = 1; c <= 64; c++)  codeword[c] = (int8_t)((w0 >> (c - 1)) & 1u);
    for (int c = 65; c <= 128; c++) codeword[c] = (int8_t)((w1 >> (c - 65)) & 1u);
    for (int c = 129; c <= N; c++)  codeword[c] = (int8_t)((w2 >> (c - 129)) & 1u);
}
#endif

/* osd174_91.f90, ndeep <= 2. llr, apmask, cw 0-based in and out. */
static void osd174_91(const float* llr, int k, const int8_t* apmask, int ndeep,
                      int8_t* message91, int8_t* cw, int* nhardmin_out, float* dmin_out)
{
    osd_gen_init();
    float rx[N + 1], absrx[N + 1], absrx0[N + 1];
    int8_t apmaskr[N + 1], hdec[N + 1], c0[N + 1], ce[N + 1], cwr[N + 1];
    int8_t m0[KK + 1], me[KK + 1], mi[KK + 1], misub[KK + 1], e2sub[N + 1], e2[N + 1];
    int indx[N + 1], indices[N + 1];

    for (int i = 1; i <= N; i++) {
        rx[i] = llr[i - 1];
        apmaskr[i] = apmask[i - 1];
        hdec[i] = rx[i] >= 0;
        absrx0[i] = fabsf(rx[i]);
    }
    indexx(absrx0, N, indx);
#ifdef OSD_INT8
    for (int i = 1; i <= N; i++) {
        int c = indx[N + 1 - i];
        for (int r = 1; r <= k; r++) genmrb[r][i] = osd_gen[r - 1][c - 1];
        indices[i] = c;
    }
    for (int id = 1; id <= k; id++) {
        for (int icol = id; icol <= k + 20; icol++) {
            if (genmrb[id][icol] == 1) {
                if (icol != id) {
                    for (int r = 1; r <= k; r++) {
                        int8_t t = genmrb[r][id]; genmrb[r][id] = genmrb[r][icol]; genmrb[r][icol] = t;
                    }
                    int itmp = indices[id]; indices[id] = indices[icol]; indices[icol] = itmp;
                }
                for (int ii = 1; ii <= k; ii++)
                    if (ii != id && genmrb[ii][id] == 1)
                        for (int c = 1; c <= N; c++) genmrb[ii][c] ^= genmrb[id][c];
                break;
            }
        }
    }
#else
    memset(gpk, 0, sizeof gpk);
    for (int i = 1; i <= N; i++) {
        int c = indx[N + 1 - i];
        const uint64_t bit = 1ull << ((i - 1) & 63);
        const int w = (i - 1) >> 6;
        for (int r = 1; r <= k; r++) if (osd_gen[r - 1][c - 1]) gpk[r][w] |= bit;
        indices[i] = c;
    }
    for (int id = 1; id <= k; id++) {
        for (int icol = id; icol <= k + 20; icol++) {
            if (GMRB(id, icol) == 1) {
                if (icol != id) {
                    /* swap columns id and icol in every row */
                    const int wa = (id - 1) >> 6, wb = (icol - 1) >> 6;
                    const int sa = (id - 1) & 63, sb = (icol - 1) & 63;
                    for (int r = 1; r <= k; r++) {
                        uint64_t a = (gpk[r][wa] >> sa) & 1u, b = (gpk[r][wb] >> sb) & 1u;
                        if (a != b) { gpk[r][wa] ^= 1ull << sa; gpk[r][wb] ^= 1ull << sb; }
                    }
                    int itmp = indices[id]; indices[id] = indices[icol]; indices[icol] = itmp;
                }
                const uint64_t p0 = gpk[id][0], p1 = gpk[id][1], p2 = gpk[id][2];
                for (int ii = 1; ii <= k; ii++)
                    if (ii != id && GMRB(ii, id) == 1) { gpk[ii][0] ^= p0; gpk[ii][1] ^= p1; gpk[ii][2] ^= p2; }
                break;
            }
        }
    }
#endif
    /* hdec=hdec(indices) etc. */
    {
        int8_t h2[N + 1], a2[N + 1];
        float r2[N + 1], ab2[N + 1];
        for (int i = 1; i <= N; i++) {
            int s = indices[i];
            h2[i] = hdec[s]; ab2[i] = absrx0[s]; r2[i] = rx[s]; a2[i] = apmaskr[s];
        }
        memcpy(hdec, h2, sizeof hdec); memcpy(absrx, ab2, sizeof absrx);
        memcpy(rx, r2, sizeof rx); memcpy(apmaskr, a2, sizeof apmaskr);
    }
    for (int i = 1; i <= k; i++) m0[i] = hdec[i];
    mrbencode91(m0, c0, k);
    int nhardmin = 0;
    float dmin = 0;
    for (int i = 1; i <= N; i++) { int x = c0[i] ^ hdec[i]; nhardmin += x; dmin += x * absrx[i]; }
    memcpy(cwr, c0, sizeof cwr);

    if (ndeep > 0) {
        int nord, npre1, nt, ntheta;
        if (ndeep == 1) { nord = 1; npre1 = 0; nt = 40; ntheta = 12; }
        else            { nord = 1; npre1 = 1; nt = 40; ntheta = 10; }  /* ndeep 2 */
        float d1 = 0;
        memset(e2sub, 0, sizeof e2sub);
        for (int iorder = 1; iorder <= nord; iorder++) {
            memset(misub, 0, sizeof misub);
            for (int i = k - iorder + 1; i <= k; i++) misub[i] = 1;
            int iflag = k - iorder + 1;
            while (iflag >= 0) {
                int iend = (iorder == nord && npre1 == 0) ? iflag : 1;
                d1 = 0.f;
                for (int n1 = iflag; n1 >= iend; n1--) {
                    memcpy(mi, misub, sizeof mi);
                    mi[n1] = 1;
                    int ap_hit = 0;
                    for (int i = 1; i <= k; i++) if (apmaskr[i] & mi[i]) { ap_hit = 1; break; }
                    if (ap_hit) continue;
                    for (int i = 1; i <= k; i++) me[i] = m0[i] ^ mi[i];
                    int nd1kpt;
                    if (n1 == iflag) {
                        mrbencode91(me, ce, k);
                        for (int i = k + 1; i <= N; i++) e2sub[i] = ce[i] ^ hdec[i];
                        memcpy(e2, e2sub, sizeof e2);
                        nd1kpt = 1;
                        for (int i = k + 1; i <= k + nt; i++) nd1kpt += e2sub[i];
                        d1 = 0.f;
                        for (int i = 1; i <= k; i++) d1 += (me[i] ^ hdec[i]) * absrx[i];
                    } else {
                        for (int i = k + 1; i <= N; i++) e2[i] = e2sub[i] ^ GMRB(n1, i);
                        nd1kpt = 2;
                        for (int i = k + 1; i <= k + nt; i++) nd1kpt += e2[i];
                    }
                    if (nd1kpt <= ntheta) {
                        mrbencode91(me, ce, k);
                        float dd;
                        if (n1 == iflag) {
                            dd = d1;
                            for (int i = k + 1; i <= N; i++) dd += e2sub[i] * absrx[i];
                        } else {
                            dd = d1 + (ce[n1] ^ hdec[n1]) * absrx[n1];
                            for (int i = k + 1; i <= N; i++) dd += e2[i] * absrx[i];
                        }
                        if (dd < dmin) {
                            dmin = dd;
                            memcpy(cwr, ce, sizeof cwr);
                            nhardmin = 0;
                            for (int i = 1; i <= N; i++) nhardmin += ce[i] ^ hdec[i];
                        }
                    }
                }
                nextpat91(misub, k, iorder, &iflag);
            }
        }
    }
    /* 998: cw(indices)=cw */
    for (int i = 1; i <= N; i++) cw[indices[i] - 1] = cwr[i];
    memcpy(message91, cw, KK);
    if (crc_bad(cw)) nhardmin = -nhardmin;
    *nhardmin_out = nhardmin;
    *dmin_out = dmin;
}

/* ----------------------------------------------------------- decode174_91 */
#define M 83
static void decode174_91(const float* llr, int maxosd, int norder, const int8_t* apmask,
                         int8_t* message91, int8_t* cw, int* ntype, int* nharderror, float* dmin)
{
    static WSX_BSS float tov[N][3], toc[M][7], tanhtoc[M][7], zsave[3][N];
    float zn[N], zsum[N];
    int nosd = 0;
    const int maxiterations = 30;
    if (maxosd > 3) maxosd = 3;
    if (maxosd == 0) { nosd = 1; memcpy(zsave[0], llr, sizeof zn); }
    else if (maxosd > 0) nosd = maxosd;
    memset(toc, 0, sizeof toc); memset(tov, 0, sizeof tov); memset(tanhtoc, 0, sizeof tanhtoc);
    for (int j = 0; j < M; j++)
        for (int i = 0; i < WSX_NRW[j]; i++) toc[j][i] = llr[WSX_NM[j][i] - 1];
    int ncnt = 0, nclast = 0;
    memset(zsum, 0, sizeof zsum);
    *nharderror = -1;
    for (int iter = 0; iter <= maxiterations; iter++) {
        for (int i = 0; i < N; i++)
            zn[i] = (apmask[i] != 1) ? llr[i] + (tov[i][0] + tov[i][1] + tov[i][2]) : llr[i];
        for (int i = 0; i < N; i++) zsum[i] += zn[i];
        if (iter > 0 && iter <= maxosd) memcpy(zsave[iter - 1], zsum, sizeof zsum);
        for (int i = 0; i < N; i++) cw[i] = zn[i] > 0.f;
        int ncheck = 0;
        for (int i = 0; i < M; i++) {
            int s = 0;
            for (int q = 0; q < WSX_NRW[i]; q++) s += cw[WSX_NM[i][q] - 1];
            if (s % 2) ncheck++;
        }
        if (ncheck == 0) {
            int nh = 0;
            for (int i = 0; i < N; i++) nh += ((2 * cw[i] - 1) * llr[i] < 0.f);
            *nharderror = nh;
            if (!crc_bad(cw)) {
                memcpy(message91, cw, KK);
                float d = 0;
                for (int i = 0; i < N; i++) d += ((llr[i] >= 0) ^ cw[i]) * fabsf(llr[i]);
                *dmin = d;
                *ntype = 1;
                return;
            }
        }
        if (iter > 0) {
            int nd = ncheck - nclast;
            if (nd < 0) ncnt = 0; else ncnt++;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) { *nharderror = -1; break; }
        }
        nclast = ncheck;
        for (int j = 0; j < M; j++)
            for (int i = 0; i < WSX_NRW[j]; i++) {
                int ibj = WSX_NM[j][i] - 1;
                toc[j][i] = zn[ibj];
                for (int kk = 0; kk < 3; kk++)
                    if (WSX_MN[ibj][kk] == j + 1) toc[j][i] -= tov[ibj][kk];
            }
        for (int i = 0; i < M; i++)
            for (int q = 0; q < 7; q++) tanhtoc[i][q] = tanhf(-toc[i][q] / 2);
        for (int j = 0; j < N; j++)
            for (int i = 0; i < 3; i++) {
                int ichk = WSX_MN[j][i] - 1;
                float Tmn = 1.f;
                for (int q = 0; q < WSX_NRW[ichk]; q++)
                    if (WSX_NM[ichk][q] != j + 1) Tmn *= tanhtoc[ichk][q];
                tov[j][i] = 2 * platanh(-Tmn);
            }
    }
    for (int i = 0; i < nosd; i++) {
        int nh;
        float dminosd;
        PROF_BEGIN(4);
        osd174_91(zsave[i], KK, apmask, norder, message91, cw, &nh, &dminosd);
        PROF_END(4);
        if (nh > 0) {
            float d = 0;
            for (int q = 0; q < N; q++) d += ((llr[q] >= 0) ^ cw[q]) * fabsf(llr[q]);
            *dmin = d;
            *nharderror = nh;
            *ntype = 2;
            return;
        }
    }
    *ntype = 0;
    *nharderror = -1;
}
#undef M

/* -------------------------------------------- get_spectrum_baseline etc. */

/* baseline.f90: s[1..NH1] (destroyed: converted to dB), sbase[1..NH1] */
static void baseline(float* s, int nfa, int nfb, float* sbase)
{
    static WSX_BSS double x[1001], y[1001];
    const int nseg = 10, npct = 10;
    double df = 12000.0 / 3840.0, a[5];
    int ia = (int)lround(nfa / df); if (ia < 1) ia = 1;
    int ib = (int)lround(nfb / df);
    for (int i = ia; i <= ib; i++) s[i] = 10.0f * log10f(s[i]);
    int nlen = (ib - ia + 1) / nseg;
    int i0 = (ib - ia + 1) / 2;
    int k = 0;
    for (int n = 1; n <= nseg; n++) {
        int ja = ia + (n - 1) * nlen, jb = ja + nlen - 1;
        float base = pctile(&s[ja], nlen, npct);
        for (int i = ja; i <= jb; i++)
            if (s[i] <= base) {
                if (k < 1000) k++;
                x[k] = i - i0;
                y[k] = s[i];
            }
    }
    polyfit(&x[1], &y[1], k, 5, a);
    for (int i = 0; i <= NH1; i++) sbase[i] = 0.f;
    for (int i = ia; i <= ib; i++) {
        double t = i - i0;
        sbase[i] = (float)(a[0] + t * (a[1] + t * (a[2] + t * (a[3] + t * (a[4])))) + 0.65);
    }
}

/* get_spectrum_baseline.f90. nfa/nfb are narrowed in place, as in Fortran. */
static void get_spectrum_baseline(const float* dd, int* nfa, int* nfb, float* sbase)
{
    enum { NST = NFFT1 / 2, NF = 93 };
    static WSX_BSS float window[NFFT1], x[NFFT1], savg[NH1 + 1], savsm[NH1 + 1];
    static WSX_BSS cf cx[NH1 + 1];
    static int first = 1;
    if (first) {
        first = 0;
        const float pi = 4.0f * atanf(1.f);
        const float a0 = 0.3635819f, a1 = -0.4891775f, a2 = 0.1365995f, a3 = -0.0106411f;
        float sum = 0;
        for (int i = 0; i < NFFT1; i++) {          /* nuttal_window.f90 */
            window[i] = a0 + a1 * cosf(2 * pi * i / NFFT1) + a2 * cosf(4 * pi * i / NFFT1) +
                        a3 * cosf(6 * pi * i / NFFT1);
            sum += window[i];
        }
        for (int i = 0; i < NFFT1; i++) window[i] = window[i] / sum * NSPS * 2 / 300.0f;
    }
    memset(savg, 0, sizeof savg);
    const float df = 12000.0f / NFFT1;
    for (int j = 1; j <= NF; j++) {
        int ia = (j - 1) * NST + 1, ib = ia + NFFT1 - 1;
        if (ib > NMAX) break;
        for (int i = 0; i < NFFT1; i++) x[i] = dd[ia - 1 + i] * window[i];
        r2c(x, cx, NFFT1);
        for (int i = 1; i <= NH1; i++) {
            float m = cabsf(cx[i]);
            savg[i] += m * m;
        }
    }
    int nwin = *nfb - *nfa;
    if (*nfa < 100) { *nfa = 100; if (nwin < 100) *nfb = *nfa + nwin; }
    if (*nfb > 4910) { *nfb = 4910; if (nwin < 100) *nfa = *nfb - nwin; }
    int ia = nint(*nfa / df); if (ia < 1) ia = 1;
    int ib = nint(*nfb / df);
    memset(savsm, 0, sizeof savsm);
    for (int i = 8; i <= NH1 - 7; i++) {
        float s = 0;
        for (int q = i - 7; q <= i + 7; q++) s += savg[q];
        savsm[i] = s / 15.f;
    }
    int npts_filt = ib - ia + 1;
    if (npts_filt > 40) {
        int nq1 = ia + npts_filt / 4, nq2 = ib - npts_filt / 4;
        float spassband = pctile(&savsm[nq1], nq2 - nq1 + 1, 30);
        if (spassband > 0.f) {
            float sthresh = spassband * 0.01f;
            int ia_det = ia, ib_det = ib;
            for (int i = ia; i <= ib; i++) if (savsm[i] >= sthresh) { ia_det = i; break; }
            for (int i = ib; i >= ia; i--) if (savsm[i] >= sthresh) { ib_det = i; break; }
            if (ia_det < ib_det) { *nfa = nint(ia_det * df); *nfb = nint(ib_det * df); }
        }
    }
    baseline(savg, *nfa, *nfb, sbase);
}

#ifndef WSX_PIECE_ONLY   /* not on the piece path - see wsx_piece_begin */
/* ------------------------------------------------------------------ sync8 */
#define JZ 62
#define MAXPRECAND 1000
static float s_sp[NHSYM + 1][NH1 + 1];   /* s(i,j) -> s_sp[j][i], 1-based */
#define S(i, m) s_sp[m][i]

static void sync8(const float* dd, int* nfa, int* nfb, float syncmin, int nfqso,
                  int maxcand, float candidate[][3], int* ncand, float* sbase)
{
    static float x[NFFT1], sync2d[NH1 + 1][2 * JZ + 1], red[NH1 + 1], red2[NH1 + 1];
    static cf cx[NH1 + 1];
    static int jpeak[NH1 + 1], jpeak2[NH1 + 1], indx[NH1 + 1], indx2[NH1 + 1];
    static float candidate0[MAXPRECAND + 1][3];
    const float tstep = NSTEP / 12000.0f, df = 12000.0f / NFFT1, fac = 1.0f / 300.0f;

    for (int j = 1; j <= NHSYM; j++) {
        int ia = (j - 1) * NSTEP;
        for (int i = 0; i < NSPS; i++) x[i] = fac * dd[ia + i];
        for (int i = NSPS; i < NFFT1; i++) x[i] = 0.f;
        r2c(x, cx, NFFT1);
        for (int i = 1; i <= NH1; i++) S(i, j) = crealf(cx[i]) * crealf(cx[i]) + cimagf(cx[i]) * cimagf(cx[i]);
    }
    get_spectrum_baseline(dd, nfa, nfb, sbase);

    int ia = nint(*nfa / df); if (ia < 1) ia = 1;
    int ib = nint(*nfb / df);
    const int nssy = NSPS / NSTEP, nfos = NFFT1 / NSPS;
    const int jstrt = (int)(0.5f / tstep);
    memset(candidate0, 0, sizeof candidate0);
    int k = 0;

    for (int i = ia; i <= ib; i++)
        for (int j = -JZ; j <= JZ; j++) {
            float ta = 0, tb = 0, tc = 0, t0a = 0, t0b = 0, t0c = 0;
            for (int n = 0; n <= 6; n++) {
                int m = j + jstrt + nssy * n;
                if (m >= 1 && m <= NHSYM) {
                    ta += S(i + nfos * icos7[n], m);
                    float s7 = 0; for (int q = 0; q <= 6; q++) s7 += S(i + nfos * q, m);
                    t0a += s7;
                }
                tb += S(i + nfos * icos7[n], m + nssy * 36);
                { float s7 = 0; for (int q = 0; q <= 6; q++) s7 += S(i + nfos * q, m + nssy * 36); t0b += s7; }
                if (m + nssy * 72 <= NHSYM) {
                    tc += S(i + nfos * icos7[n], m + nssy * 72);
                    float s7 = 0; for (int q = 0; q <= 6; q++) s7 += S(i + nfos * q, m + nssy * 72);
                    t0c += s7;
                }
            }
            float t = ta + tb + tc, t0 = t0a + t0b + t0c;
            t0 = (t0 - t) / 6.0f;
            float sync_abc = t / t0;
            t = tb + tc; t0 = t0b + t0c;
            t0 = (t0 - t) / 6.0f;
            float sync_bc = t / t0;
            sync2d[i][j + JZ] = fmaxf(sync_abc, sync_bc);
        }

    memset(red, 0, sizeof red); memset(red2, 0, sizeof red2);
    const int mlag = 13, mlag2 = JZ;
    for (int i = ia; i <= ib; i++) {
        int b = -mlag;
        for (int j = -mlag; j <= mlag; j++) if (sync2d[i][j + JZ] > sync2d[i][b + JZ]) b = j;
        jpeak[i] = b; red[i] = sync2d[i][b + JZ];
        b = -mlag2;
        for (int j = -mlag2; j <= mlag2; j++) if (sync2d[i][j + JZ] > sync2d[i][b + JZ]) b = j;
        jpeak2[i] = b; red2[i] = sync2d[i][b + JZ];
    }
    int iz = ib - ia + 1;
    indexx(&red[ia - 1], iz, indx);
    int npctile = nint(0.40f * iz);
    if (npctile < 1) { *ncand = 0; return; }
    int ibase = indx[npctile] - 1 + ia;
    if (ibase < 1) ibase = 1;
    if (ibase > NH1) ibase = NH1;
    float base = red[ibase];
    for (int i = 0; i <= NH1; i++) red[i] /= base;
    indexx(&red2[ia - 1], iz, indx2);
    int ibase2 = indx2[npctile] - 1 + ia;
    if (ibase2 < 1) ibase2 = 1;
    if (ibase2 > NH1) ibase2 = NH1;
    float base2 = red2[ibase2];
    for (int i = 0; i <= NH1; i++) red2[i] /= base2;

    int lim = iz < MAXPRECAND ? iz : MAXPRECAND;
    for (int i = 1; i <= lim; i++) {
        int n = ia + indx[iz + 1 - i] - 1;
        if (k >= MAXPRECAND) break;
        if (red[n] >= syncmin && !isnan(red[n])) {
            k++;
            candidate0[k][0] = n * df;
            candidate0[k][1] = (jpeak[n] - 0.5f) * tstep;
            candidate0[k][2] = red[n];
        }
        if (abs(jpeak2[n] - jpeak[n]) == 0) continue;
        if (k >= MAXPRECAND) break;
        if (red2[n] >= syncmin && !isnan(red2[n])) {
            k++;
            candidate0[k][0] = n * df;
            candidate0[k][1] = (jpeak2[n] - 0.5f) * tstep;
            candidate0[k][2] = red2[n];
        }
    }
    int nc = k;
    for (int i = 2; i <= nc; i++)
        for (int j = 1; j <= i - 1; j++) {
            float fdiff = fabsf(candidate0[i][0]) - fabsf(candidate0[j][0]);
            float tdiff = fabsf(candidate0[i][1] - candidate0[j][1]);
            if (fabsf(fdiff) < 4.0f && tdiff < 0.04f) {
                if (candidate0[i][2] >= candidate0[j][2]) candidate0[j][2] = 0.f;
                if (candidate0[i][2] < candidate0[j][2]) candidate0[i][2] = 0.f;
            }
        }
    static float c3[MAXPRECAND + 1];
    static int cidx[MAXPRECAND + 1];
    for (int i = 1; i <= nc; i++) c3[i] = candidate0[i][2];
    indexx(c3, nc, cidx);
    k = 1;
    for (int i = 1; i <= nc; i++)
        if (fabsf(candidate0[i][0] - nfqso) <= 10.0f && candidate0[i][2] >= syncmin) {
            memcpy(candidate[k - 1], candidate0[i], sizeof(float) * 3);
            candidate0[i][2] = 0.f;
            k++;
        }
    for (int i = nc; i >= 1; i--) {
        int j = cidx[i];
        if (candidate0[j][2] >= syncmin) {
            candidate[k - 1][1] = candidate0[j][1];
            candidate[k - 1][2] = candidate0[j][2];
            candidate[k - 1][0] = fabsf(candidate0[j][0]);
            k++;
            if (k > maxcand) break;
        }
    }
    *ncand = k - 1;
}

#endif /* !WSX_PIECE_ONLY */

/* ---------------------------------------------------------- ft8_downsample */
#define NFFTD 192000
#define NFFT2 3200
#ifdef WSX_PIECE_ONLY
/* The piece path's two big buffers live on the HEAP, not in .bss, so
 * wsx_piece_release() can hand them back when FT8 stops (the Tab5 needs that
 * PSRAM in its other modes). g_pdd is the slot audio, NFFTD long with a zero
 * tail, so ft8_downsample's FFT reads it directly - jt9's own x(1:NMAX)=dd,
 * x(NMAX+1:)=0 copy is that same array, and it saved 750 KB. */
static float* g_pdd;     /* NFFTD floats */
static cf*    g_pcx;     /* NFFTD/2+1    */
#endif
static void ft8_downsample_(const float* dd, int* newdat, float f0, cf* c1);
static void ft8_downsample(const float* dd, int* newdat, float f0, cf* c1)
{
    PROF_BEGIN(1);
    ft8_downsample_(dd, newdat, f0, c1);
    PROF_END(1);
}
static void ft8_downsample_(const float* dd, int* newdat, float f0, cf* c1)
{
#ifdef WSX_PIECE_ONLY
    cf* const cx = g_pcx;            /* dd is g_pdd: NFFTD long, zero tail */
    static WSX_BSS cf tmp[NFFT2];
#else
    static WSX_BSS float x[NFFTD];
    static WSX_BSS cf cx[NFFTD / 2 + 1], tmp[NFFT2];
#endif
    static float taper[101];
    static int first = 1;
    if (first) {
        const float pi = 4.0f * atanf(1.0f);
        for (int i = 0; i <= 100; i++) taper[i] = 0.5f * (1.0f + cosf(i * pi / 100));
        first = 0;
    }
    if (*newdat) {
#ifdef WSX_PIECE_ONLY
        r2c(dd, cx, NFFTD);
#else
        memcpy(x, dd, sizeof(float) * NMAX);
        memset(x + NMAX, 0, sizeof(float) * (NFFTD - NMAX));
        r2c(x, cx, NFFTD);
#endif
        *newdat = 0;
    }
    const float df = 12000.0f / NFFTD, baud = 12000.0f / NSPS;
    int i0 = nint(f0 / df);
    float ft = f0 + 8.5f * baud;
    int it = nint(ft / df); if (it > NFFTD / 2) it = NFFTD / 2;
    float fb = f0 - 1.5f * baud;
    int ib = nint(fb / df); if (ib < 1) ib = 1;
    int k = 0;
    memset(c1, 0, sizeof(cf) * NFFT2);
    for (int i = ib; i <= it; i++) c1[k++] = cx[i];
    for (int i = 0; i <= 100; i++) c1[i] *= taper[100 - i];
    for (int i = 0; i <= 100; i++) c1[k - 1 - 100 + i] *= taper[i];
    int sh = i0 - ib;
    for (int i = 0; i < NFFT2; i++) tmp[i] = c1[((i + sh) % NFFT2 + NFFT2) % NFFT2];
    memcpy(c1, tmp, sizeof(cf) * NFFT2);
    c2c(c1, NFFT2, +1);
    float fac = 1.0f / sqrtf((float)NFFTD * NFFT2);
    for (int i = 0; i < NFFT2; i++) c1[i] *= fac;
}

/* ------------------------------------------------------------------ sync8d */
static float sync8d(const cf* cd0, int i0, const cf* ctwk, int itwk)
{
    static WSX_BSS cf csync[7][32];
    static int first = 1;
    if (first) {
        const float twopi = 8.0f * atanf(1.0f);
        for (int i = 0; i < 7; i++) {
            float phi = 0, dphi = twopi * icos7[i] / 32.0f;
            for (int j = 0; j < 32; j++) {
                csync[i][j] = cosf(phi) + I * sinf(phi);
                phi = fmodf(phi + dphi, twopi);
            }
        }
        first = 0;
    }
    float sync = 0;
    for (int i = 0; i < 7; i++) {
        int i1 = i0 + i * 32, i2 = i1 + 36 * 32, i3 = i1 + 72 * 32;
        cf cs2[32];
        for (int j = 0; j < 32; j++) cs2[j] = itwk == 1 ? ctwk[j] * csync[i][j] : csync[i][j];
        cf z1 = 0, z2 = 0, z3 = 0;
        if (i1 >= 0 && i1 + 31 <= NP2 - 1) for (int j = 0; j < 32; j++) z1 += cd0[i1 + j] * conjf(cs2[j]);
        if (i2 >= 0 && i2 + 31 <= NP2 - 1) for (int j = 0; j < 32; j++) z2 += cd0[i2 + j] * conjf(cs2[j]);
        if (i3 >= 0 && i3 + 31 <= NP2 - 1) for (int j = 0; j < 32; j++) z3 += cd0[i3 + j] * conjf(cs2[j]);
        sync += crealf(z1) * crealf(z1) + cimagf(z1) * cimagf(z1) +
                crealf(z2) * crealf(z2) + cimagf(z2) * cimagf(z2) +
                crealf(z3) * crealf(z3) + cimagf(z3) * cimagf(z3);
    }
    return sync;
}

#ifndef WSX_PIECE_ONLY   /* not on the piece path - see wsx_piece_begin */
/* ------------------------------------------------------------ gen_ft8wave */
static float gfsk_pulse(float b, float t)
{
    const float pi = 4.f * atanf(1.0f);
    const float c = pi * sqrtf(2.0f / logf(2.0f));
    return 0.5f * (erff(c * b * (t + 0.5f)) - erff(c * b * (t - 0.5f)));
}
/* icmplx=1, nsym=79, nsps=1920, bt=2, fsample=12000, nwave=NZ */
/* gen_ft8wave.f90, icmplx=1, nsym=79, bt=2.0: any nsps and fsample. Used at
 * nsps 1920 / 12000 S/s by subtractft8 and at nsps 32 / 200 S/s by ft8_a8d.
 * The pulse is recomputed when nsps changes, as the Fortran's fchk test
 * does (it keys on nsym+nsps+bt+fsample). */
static void gen_ft8wave(const int* itone, int nsps, float fsample, float f0, cf* cwave, int nwave)
{
    enum { NTAB = 65536, nsym = NN };
    static float pulse[3 * NSPS];
    static cf ctab[NTAB];
    static float dphi[(NN + 2) * NSPS];
    static int pulse_nsps = 0, tab_done = 0;
    const float twopi = 8.0f * atanf(1.0f);
    const float dt = 1.0f / fsample;
    if (!tab_done) {
        for (int i = 0; i < NTAB; i++) {
            float phi = i * twopi / NTAB;
            ctab[i] = cosf(phi) + I * sinf(phi);
        }
        tab_done = 1;
    }
    if (pulse_nsps != nsps) {
        for (int i = 1; i <= 3 * nsps; i++) {
            float tt = (i - 1.5f * nsps) / (float)nsps;
            pulse[i - 1] = gfsk_pulse(2.0f, tt);
        }
        pulse_nsps = nsps;
    }
    const float dphi_peak = twopi * 1.0f / (float)nsps;
    memset(dphi, 0, sizeof(float) * (nsym + 2) * nsps);
    for (int j = 1; j <= nsym; j++) {
        int ib = (j - 1) * nsps;
        for (int q = 0; q < 3 * nsps; q++) dphi[ib + q] += dphi_peak * pulse[q] * itone[j - 1];
    }
    for (int q = 0; q < 2 * nsps; q++) dphi[q] += dphi_peak * itone[0] * pulse[nsps + q];
    for (int q = 0; q < 2 * nsps; q++) dphi[nsym * nsps + q] += dphi_peak * itone[nsym - 1] * pulse[q];
    float phi = 0;
    for (int q = 0; q < (nsym + 2) * nsps; q++) dphi[q] += twopi * f0 * dt;
    int k = 0;
    for (int j = nsps; j <= nsps + nwave - 1; j++) {
        int i = (int)(phi * (float)NTAB / twopi);
        if (i >= NTAB) i = NTAB - 1;
        cwave[k++] = ctab[i];
        phi = fmodf(phi + dphi[j], twopi);
    }
    const int nramp = nint(nsps / 8.0f);
    for (int i = 0; i < nramp; i++)
        cwave[i] *= (1.0f - cosf(twopi * i / (2.0f * nramp))) / 2.0f;
    int k1 = nsym * nsps - nramp + 1;
    for (int i = 0; i < nramp; i++)
        cwave[k1 - 1 + i] *= (1.0f + cosf(twopi * i / (2.0f * nramp))) / 2.0f;
}

/* ------------------------------------------------------------- subtractft8 */
#define NFILT 4000
static cf sub_cref[NZ], sub_camp[NMAX], sub_cfilt[NMAX], sub_cw[NMAX];
static float sub_dd[NMAX], sub_x[NMAX];
static cf sub_cx[NMAX / 2 + 1];
static float endcorrection[NFILT / 2 + 1];

static float sqf(float* dd0, float f0, float dt, int idt, int ldt)
{
    int nstart = (int)(dt * 12000 + 1 + idt);
    memset(sub_camp, 0, sizeof sub_camp);
    memcpy(sub_dd, dd0, sizeof sub_dd);
    for (int i = 1; i <= NZ; i++) {
        int j = nstart - 1 + i;
        if (j >= 1 && j <= NMAX) sub_camp[i - 1] = sub_dd[j - 1] * conjf(sub_cref[i - 1]);
    }
    memcpy(sub_cfilt, sub_camp, sizeof(cf) * NZ);
    memset(sub_cfilt + NZ, 0, sizeof(cf) * (NMAX - NZ));
    c2c(sub_cfilt, NMAX, -1);
    for (int i = 0; i < NMAX; i++) sub_cfilt[i] *= sub_cw[i];
    c2c(sub_cfilt, NMAX, +1);
    for (int i = 0; i <= NFILT / 2; i++) sub_cfilt[i] *= endcorrection[i];
    for (int q = 0; q <= NFILT / 2; q++) sub_cfilt[NZ - 1 - q] *= endcorrection[q];
    memset(sub_x, 0, sizeof sub_x);
    for (int i = 1; i <= NZ; i++) {
        int j = nstart + i - 1;
        if (j >= 1 && j <= NMAX) {
            cf z = sub_cfilt[i - 1] * sub_cref[i - 1];
            sub_dd[j - 1] = sub_dd[j - 1] - 2.0f * crealf(z);
            sub_x[i - 1] = sub_dd[j - 1];
        }
    }
    float sqq = 0;
    if (ldt) {
        r2c(sub_x, sub_cx, NMAX);
        const float df = 12000.0f / NMAX;
        int ia = (int)((f0 - 1.5f * 6.25f) / df), ib = (int)((f0 + 8.5f * 6.25f) / df);
        for (int i = ia; i <= ib; i++)
            sqq += crealf(sub_cx[i]) * crealf(sub_cx[i]) + cimagf(sub_cx[i]) * cimagf(sub_cx[i]);
    }
    return sqq;
}

static void subtractft8_(float* dd0, const int* itone, float f0, float dt, int lrefinedt);
static void subtractft8(float* dd0, const int* itone, float f0, float dt, int lrefinedt)
{
    PROF_BEGIN(5);
    subtractft8_(dd0, itone, f0, dt, lrefinedt);
    PROF_END(5);
}
static void subtractft8_(float* dd0, const int* itone, float f0, float dt, int lrefinedt)
{
    static int first = 1;
    if (first) {
        static float window[NFILT + 1];   /* window(-NFILT/2..NFILT/2) */
        const float pi = 4.0f * atanf(1.0f);
        const float fac = 1.0f / (float)NMAX;
        float sumw = 0;
        for (int j = -NFILT / 2; j <= NFILT / 2; j++) {
            float c = cosf(pi * j / NFILT);
            window[j + NFILT / 2] = c * c;
            sumw += window[j + NFILT / 2];
        }
        static cf t[NMAX];
        memset(t, 0, sizeof t);
        for (int q = 0; q <= NFILT; q++) t[q] = window[q] / sumw;
        const int sh = NFILT / 2 + 1;                 /* cshift(cw, NFILT/2+1) */
        for (int i = 0; i < NMAX; i++) sub_cw[i] = t[(i + sh) % NMAX];
        c2c(sub_cw, NMAX, -1);
        for (int i = 0; i < NMAX; i++) sub_cw[i] *= fac;
        for (int j = 1; j <= NFILT / 2 + 1; j++) {
            float s = 0;
            for (int q = j - 1; q <= NFILT / 2; q++) s += window[q + NFILT / 2];
            endcorrection[j - 1] = 1.0f / (1.0f - s / sumw);
        }
        first = 0;
    }
    gen_ft8wave(itone, NSPS, 12000.0f, f0, sub_cref, NZ);
    if (lrefinedt) {
        float sqa = sqf(dd0, f0, dt, -90, 1);
        float sqb = sqf(dd0, f0, dt, +90, 1);
        float sq0 = sqf(dd0, f0, dt, 0, 1);
        float b = sqb - sqa, c = sqb + sqa - 2.0f * sq0;   /* peakup.f90 */
        float dx = -b / (2.0f * c);
        if (fabsf(dx) > 1.0f) return;
        int i2 = nint(90.0f * dx);
        sqf(dd0, f0, dt, i2, 0);
    } else {
        sqf(dd0, f0, dt, 0, 0);
    }
    memcpy(dd0, sub_dd, sizeof sub_dd);
}

#endif /* !WSX_PIECE_ONLY */

/* ----------------------------------------------------------------- ft8b */

typedef struct {
    int apsym[58];       /* +-1, or apsym[0]=99 when mycall is unusable */
    const wsx_params_t* p;
} apctx_t;

static void bits_to_payload(const int8_t* b77, uint8_t* payload)
{
    memset(payload, 0, 10);
    for (int i = 0; i < 77; i++)
        if (b77[i]) payload[i / 8] |= (uint8_t)(0x80u >> (i % 8));
}

/* ---------------------------------------------- packjt77 hash bookkeeping
 * jt9 stores a callsign only where packjt77's unpack77 calls save_hash_call:
 * call_2 of an i3=1/2 message when it is a full call, and the full call of
 * an i3=4 message with iflip=0. ft8_lib's unpacker stores every call it
 * sees, which resolved hashes jt9 prints as <...> (5 lines in 150 slots,
 * 2026-11-21). So ft8_lib gets a LOOKUP-ONLY view of the caller's table and
 * the port stores what packjt77 would. */
static ftx_callsign_hash_interface_t* g_hash;
static bool lk_lookup(ftx_callsign_hash_type_t t, uint32_t h, char* c)
{
    if (g_hash && g_hash->lookup_hash) return g_hash->lookup_hash(t, h, c);
    c[0] = 0;
    return false;
}
static void lk_nosave(const char* c, uint32_t n22) { (void)c; (void)n22; }
static ftx_callsign_hash_interface_t lk_if = {.lookup_hash = lk_lookup, .save_hash = lk_nosave};

/* ihashcall(c,22) and save_hash_call (its checks: not '<...>', >= 3 chars) */
static void save_hash_call(const char* c13)
{
    static const char c[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
    char cw[14];
    if (!g_hash || !g_hash->save_hash || !c13 || c13[0] == ' ' || c13[0] == 0) return;
    if (strncmp(c13, "<...>", 5) == 0) return;
    snprintf(cw, sizeof cw, "%s", c13[0] == '<' ? c13 + 1 : c13);
    char* gt = strchr(cw, '>');
    if (gt) *gt = 0;
    if (strlen(cw) < 3) return;
    uint64_t n8 = 0;
    for (int i = 0; i < 11; i++) {
        char ch = i < (int)strlen(cw) ? cw[i] : ' ';
        const char* q = strchr(c, ch);
        int j = (q && ch) ? (int)(q - c) : 0;
        n8 = 38 * n8 + (uint64_t)j;
    }
    uint64_t prod = 47055833459ull * n8;               /* mulmod64: mod 2^64 */
    g_hash->save_hash(cw, (uint32_t)(prod >> (64 - 22)));
}

/* The save_hash_call calls inside unpack77 (nrx=1), for i3 1, 2 and 4. */
static void unpack77_save(const ftx_message_t* m, const int8_t* b77, int i3)
{
    char to[32], de[32], ex[32];
    ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
    if (i3 == 1 || i3 == 2) {
        if (ftx_message_decode_std(m, &lk_if, to, de, ex, ft) != FTX_MESSAGE_RC_OK) return;
        if (de[0] != '<') save_hash_call(de);
    } else if (i3 == 4) {
        if (b77[70] != 0) return;                     /* iflip=1: call_1 is the full call */
        if (ftx_message_decode_nonstd(m, &lk_if, to, de, ex, ft) != FTX_MESSAGE_RC_OK) return;
        save_hash_call(de);
    }
}

/* unpack77, i3=5 (EU VHF: h12 h22 r1 s3 S11 g25). ft8_lib has no unpacker
 * for it. The mycall special case (n12 == hashmy12 -> <mycall>) is left
 * out: jt9's mycall is the dummy K1ABC. */
static int unpack_i3_5(const int8_t* b, char* msg)
{
    uint32_t n12 = 0, n22 = 0, ir, irpt = 0, iserial = 0, n = 0;
    int q = 0;
    for (int i = 0; i < 12; i++) n12 = n12 << 1 | (uint32_t)b[q++];
    for (int i = 0; i < 22; i++) n22 = n22 << 1 | (uint32_t)b[q++];
    ir = (uint32_t)b[q++];
    for (int i = 0; i < 3; i++) irpt = irpt << 1 | (uint32_t)b[q++];
    for (int i = 0; i < 11; i++) iserial = iserial << 1 | (uint32_t)b[q++];
    for (int i = 0; i < 25; i++) n = n << 1 | (uint32_t)b[q++];
    if (n > 18662399u) return 0;
    char c1[16] = "<...>", c2[16] = "<...>", t[14];
    if (lk_lookup(FTX_CALLSIGN_HASH_12_BITS, n12, t)) snprintf(c1, sizeof c1, "<%s>", t);
    if (lk_lookup(FTX_CALLSIGN_HASH_22_BITS, n22, t)) snprintf(c2, sizeof c2, "<%s>", t);
    int j1 = n / (18 * 10 * 10 * 24 * 24); n -= j1 * 18 * 10 * 10 * 24 * 24;   /* to_grid6 */
    int j2 = n / (10 * 10 * 24 * 24); n -= j2 * 10 * 10 * 24 * 24;
    int j3 = n / (10 * 24 * 24); n -= j3 * 10 * 24 * 24;
    int j4 = n / (24 * 24); n -= j4 * 24 * 24;
    int j5 = n / 24, j6 = n - j5 * 24;
    if (j1 > 17 || j2 > 17 || j3 > 9 || j4 > 9 || j5 > 23 || j6 > 23) return 0;
    char cexch[8];
    snprintf(cexch, sizeof cexch, "%2u%04u", (unsigned)(52 + irpt), (unsigned)iserial);   /* uint32_t is long on RISC-V */
    snprintf(msg, 64, "%s %s %s%s %c%c%c%c%c%c", c1, c2, ir ? "R " : "", cexch,
            'A' + j1, 'A' + j2, '0' + j3, '0' + j4, 'A' + j5, 'A' + j6);
    return 1;
}

/* unpack77, i3=0 n3=1 (DXpedition: c28 c28 h10 r5). ft8_lib has no
 * unpacker for it. unpack28 is borrowed from ft8_lib by wrapping each c28 in
 * an i3=1 payload (ip=0, grid AA00) and reading call_to back. */
static int unpack28_via_std(uint32_t n28, char* out)
{
    ftx_message_t m;
    memset(&m, 0, sizeof m);
    int8_t b[77] = {0};
    for (int i = 0; i < 28; i++) b[i] = (n28 >> (27 - i)) & 1;      /* n29a = n28<<1 */
    /* n29b = 0 (DE), ir 0, igrid4 0 = AA00, i3 = 1 */
    b[76] = 1;
    bits_to_payload(b, m.payload);
    char to[32], de[32], ex[32];
    ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
    if (ftx_message_decode_std(&m, &lk_if, to, de, ex, ft) != FTX_MESSAGE_RC_OK) return 0;
    strcpy(out, to);
    return 1;
}
static int unpack_dxped(const int8_t* b, char* msg)
{
    uint32_t n28a = 0, n28b = 0, n10 = 0, n5 = 0;
    int q = 0;
    for (int i = 0; i < 28; i++) n28a = n28a << 1 | (uint32_t)b[q++];
    for (int i = 0; i < 28; i++) n28b = n28b << 1 | (uint32_t)b[q++];
    for (int i = 0; i < 10; i++) n10 = n10 << 1 | (uint32_t)b[q++];
    for (int i = 0; i < 5; i++) n5 = n5 << 1 | (uint32_t)b[q++];
    int irpt = 2 * (int)n5 - 30;
    char c1[32], c2[32], c3[16] = "<...>", t[14];
    if (n28a <= 2 || n28b <= 2) return 0;
    if (!unpack28_via_std(n28a, c1) || !unpack28_via_std(n28b, c2)) return 0;
    if (lk_lookup(FTX_CALLSIGN_HASH_10_BITS, n10, t)) snprintf(c3, sizeof c3, "<%s>", t);
    /* The return value is checked: -Wformat-truncation=1 then trusts the
     * caller, and a message that does not fit is not a message. */
    int w = snprintf(msg, 64, "%s RR73; %s %s %c%02d", c1, c2, c3, irpt >= 0 ? '+' : '-', abs(irpt));
    if (w < 0 || w >= 64) return 0;
    return 1;
}

/* packjt77's callok(): 3+ chars, not Q..., call area digit at 2 or 3, a
 * letter in the prefix, an all-letter suffix (sfx is CHARACTER*3). */
static int callok(const char* w)
{
    int n = (int)strlen(w);
    while (n > 0 && w[n - 1] == ' ') n--;
    if (n < 3 || w[0] == 'Q') return 0;
    int i0;
    for (i0 = n; i0 >= 1; i0--) if (w[i0 - 1] >= '0' && w[i0 - 1] <= '9') break;
    if (i0 != 2 && i0 != 3) return 0;
    int nlp = 0;
    for (int i = 0; i < i0 - 1; i++) nlp += isalpha((unsigned char)w[i]) != 0;
    if (nlp == 0) return 0;
    char sfx[4] = {0};
    strncpy(sfx, w + i0, 3);
    int ns = (int)strlen(sfx);
    while (ns > 0 && sfx[ns - 1] == ' ') ns--;
    for (int i = 0; i < ns; i++) if (!isalpha((unsigned char)sfx[i])) return 0;
    return 1;
}
/* unpack28's success flag for a standard call (n28 >= NTOKENS + MAX22);
 * tokens and hashes always succeed. */
static int unpack28_ok(uint32_t n28)
{
    static const char c1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", c2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                      c3[] = "0123456789", c4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const uint32_t NTOKENS = 2063592, MAX22 = 4194304;
    if (n28 < NTOKENS) return 1;
    n28 -= NTOKENS;
    if (n28 < MAX22) return 1;
    uint32_t n = n28 - MAX22;
    uint32_t i1 = n / (36 * 10 * 27 * 27 * 27); n -= 36 * 10 * 27 * 27 * 27 * i1;
    uint32_t i2 = n / (10 * 27 * 27 * 27); n -= 10 * 27 * 27 * 27 * i2;
    uint32_t i3 = n / (27 * 27 * 27); n -= 27 * 27 * 27 * i3;
    uint32_t i4 = n / (27 * 27); n -= 27 * 27 * i4;
    uint32_t i5 = n / 27, i6 = n - 27 * i5;
    if (i1 > 36 || i2 > 35 || i3 > 9 || i4 > 26 || i5 > 26 || i6 > 26) return 0;
    char c13[8] = {c1[i1], c2[i2], c3[i3], c4[i4], c4[i5], c4[i6], 0};
    const char* w = c13;
    while (*w == ' ') w++;                                   /* adjustl */
    char t[8];
    strcpy(t, w);
    int L = (int)strlen(t);
    while (L > 0 && t[L - 1] == ' ') t[--L] = 0;            /* trim */
    if (!callok(t)) return 0;
    for (int i = 0; i < L; i++) if (t[i] == ' ') return 0;  /* label 900 */
    return 1;
}
/* unpack77's i3=1/2 failure rules that ft8_lib's unpacker does not apply */
static int std_ok_like_jt9(const int8_t* b, const char* text)
{
    uint32_t n28a = 0, n28b = 0, igrid4 = 0;
    for (int i = 0; i < 28; i++) n28a = n28a << 1 | (uint32_t)b[i];
    for (int i = 29; i < 57; i++) n28b = n28b << 1 | (uint32_t)b[i];
    int ir = b[58];
    for (int i = 59; i < 74; i++) igrid4 = igrid4 << 1 | (uint32_t)b[i];
    if (!unpack28_ok(n28a) || !unpack28_ok(n28b)) return 0;
    int cq = strncmp(text, "CQ ", 3) == 0;
    if (igrid4 <= 32400) { if (cq && ir == 1) return 0; }
    else if (cq && (int)igrid4 - 32400 >= 2) return 0;
    return 1;
}

static void normalizebmet(float* bmet, int n)
{
    float s = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s += bmet[i]; s2 += bmet[i] * bmet[i]; }
    float bmetav = s / (float)n, bmet2av = s2 / (float)n;
    float var = bmet2av - bmetav * bmetav;
    float bmetsig = var > 0.f ? sqrtf(var) : sqrtf(bmet2av);
    for (int i = 0; i < n; i++) bmet[i] /= bmetsig;
}

/* Returns nbadcrc. f1 and xdt are refined in place, as in Fortran. */
/* Knobs for the piece API below; the defaults are ft8b's own behaviour. */
static int g_piece_ndeep = 2;       /* decode174_91 norder; ft8b passes 2 */
static int g_piece_npasses = 0;     /* 0 = ft8b's npasses; else at most this many */
static int g_piece_maxosd  = 2;     /* decode174_91's maxosd: -1 BP only, 2 = ft8b */

static int ft8b(float* dd0, int* newdat, const apctx_t* ap, int nzhsym, int lsubtract,
                int imetric, int* iaptype, float* f1, float* xdt, float xbase,
                int* nharderrors, float* dmin, char* msg37, float* xsnr_out, int* itone)
{
    static const int mcq[29] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0};
    static const int nappasses[6] = {2, 2, 2, 4, 4, 3};
    static const int naptypes[6][4] = {{1,2,0,0},{2,3,0,0},{2,3,0,0},{3,4,5,6},{3,4,5,6},{3,1,2,0}};
    const wsx_params_t* p = ap->p;
    static WSX_BSS cf cd0[NFFT2], ctwk[32], csymb[32], cs[NN + 1][8];
    static WSX_BSS float s8[NN + 1][8];
    float bmeta[174], bmetb[174], bmetc[174], bmetd[174], bmete[174];
    float llra[174], llrb[174], llrc[174], llrd[174], llre[174], llrz[174];
    float s2[512];
    const float fs2 = 12000.0f / NDOWN, dt2 = 1.0f / fs2, twopi = 8.0f * atanf(1.0f);

    *nharderrors = -1;
    float delfbest = 0;
    int ibest = 0;
    ft8_downsample(dd0, newdat, *f1, cd0);
    int i0 = nint((*xdt + 0.5f) * fs2);
    float smax = 0;
    for (int idt = i0 - 10; idt <= i0 + 10; idt++) {
        float sync = sync8d(cd0, idt, ctwk, 0);
        if (sync > smax) { smax = sync; ibest = idt; }
    }
    smax = 0;
    for (int ifr = -5; ifr <= 5; ifr++) {
        float delf = ifr * 0.5f, dphi = twopi * delf * dt2, phi = 0;
        for (int i = 0; i < 32; i++) {
            ctwk[i] = cosf(phi) + I * sinf(phi);
            phi = fmodf(phi + dphi, twopi);
        }
        float sync = sync8d(cd0, ibest, ctwk, 1);
        if (sync > smax) { smax = sync; delfbest = delf; }
    }
    /* twkfreq1(cd0,...) is skipped: its output is overwritten by the next
     * ft8_downsample call before anything reads it. */
    *f1 = *f1 + delfbest;
    int nd = 0;
    ft8_downsample(dd0, &nd, *f1, cd0);
    float ss[9];
    for (int idt = -4; idt <= 4; idt++) ss[idt + 4] = sync8d(cd0, ibest + idt, ctwk, 0);
    int il = 0;
    for (int q = 1; q < 9; q++) if (ss[q] > ss[il]) il = q;
    ibest = il - 4 + ibest;
    *xdt = (ibest - 1) * dt2;

    for (int k = 1; k <= NN; k++) {
        int i1 = ibest + (k - 1) * 32;
        if (i1 >= 0 && i1 + 31 <= NP2 - 1) memcpy(csymb, cd0 + i1, sizeof csymb);
        else memset(csymb, 0, sizeof csymb);
        c2c(csymb, 32, -1);
        for (int q = 0; q < 8; q++) { cs[k][q] = csymb[q] / 1e3f; s8[k][q] = cabsf(csymb[q]); }
    }
    int nsync = 0;
    for (int k = 1; k <= 7; k++) {
        int kk[3] = {k, k + 36, k + 72};
        for (int r = 0; r < 3; r++) {
            int ip = 0;
            for (int q = 1; q < 8; q++) if (s8[kk[r]][q] > s8[kk[r]][ip]) ip = q;
            if (icos7[k - 1] == ip) nsync++;
        }
    }
    int syncmin = 6;
    if (imetric == 2) syncmin = 7;
    if (p->ndepth <= 2) syncmin = 8;
    if (nsync <= syncmin) return 1;

    for (int nsym = 1; nsym <= 3; nsym++) {
        int nt = 1 << (3 * nsym);
        for (int ihalf = 1; ihalf <= 2; ihalf++)
            for (int k = 1; k <= 29; k += nsym) {
                int ks = ihalf == 1 ? k + 7 : k + 43;
                for (int i = 0; i < nt; i++) {
                    int i1 = i / 64, i2 = (i & 63) / 8, i3 = i & 7;
                    if (nsym == 1) s2[i] = cabsf(cs[ks][graymap[i3]]);
                    else if (nsym == 2) s2[i] = cabsf(cs[ks][graymap[i2]] + cs[ks + 1][graymap[i3]]);
                    else s2[i] = cabsf(cs[ks][graymap[i1]] + cs[ks + 1][graymap[i2]] + cs[ks + 2][graymap[i3]]);
                }
                if (imetric == 2) for (int i = 0; i < nt; i++) s2[i] = s2[i] * s2[i];
                int i32 = 1 + (k - 1) * 3 + (ihalf - 1) * 87;
                int ibmax = nsym == 1 ? 2 : nsym == 2 ? 5 : 8;
                for (int ib = 0; ib <= ibmax; ib++) {
                    int bit = ibmax - ib;
                    float m1 = -INFINITY, m0 = -INFINITY;
                    for (int i = 0; i < nt; i++) {
                        if ((i >> bit) & 1) { if (s2[i] > m1) m1 = s2[i]; }
                        else if (s2[i] > m0) m0 = s2[i];
                    }
                    float bm = m1 - m0;
                    if (i32 + ib > 174) continue;
                    if (nsym == 1) {
                        bmeta[i32 + ib - 1] = bm;
                        float den = fmaxf(m1, m0);
                        bmetd[i32 + ib - 1] = den > 0.f ? bm / den : 0.f;
                    } else if (nsym == 2) bmetb[i32 + ib - 1] = bm;
                    else bmetc[i32 + ib - 1] = bm;
                }
            }
    }
    for (int i = 0; i < 174; i++) {
        float t[3] = {bmeta[i], bmetb[i], bmetc[i]};
        int ip = 0;
        for (int q = 1; q < 3; q++) if (fabsf(t[q]) > fabsf(t[ip])) ip = q;
        bmete[i] = t[ip];
    }
    normalizebmet(bmeta, 174); normalizebmet(bmetb, 174); normalizebmet(bmetc, 174);
    normalizebmet(bmetd, 174); normalizebmet(bmete, 174);
    const float scalefac = 2.83f;
    for (int i = 0; i < 174; i++) {
        llra[i] = scalefac * bmeta[i]; llrb[i] = scalefac * bmetb[i]; llrc[i] = scalefac * bmetc[i];
        llrd[i] = scalefac * bmetd[i]; llre[i] = scalefac * bmete[i];
    }

    int npasses = p->lapon ? 5 + 2 * nappasses[p->nqso_progress] : 5;
    if (nzhsym < 50) npasses = 5;

    if (g_piece_npasses > 0 && npasses > g_piece_npasses) npasses = g_piece_npasses;
    for (int ipass = 1; ipass <= npasses; ipass++) {
        int8_t apmask[174];
        const float* src = llra;
        if (ipass == 2) src = llrb;
        if (ipass == 3) src = llrc;
        if (ipass == 4) src = llrd;
        if (ipass == 5) src = llre;
        memcpy(llrz, src, sizeof llrz);
        if (ipass <= 5) { memset(apmask, 0, sizeof apmask); *iaptype = 0; }
        if (ipass > 5) {
            memcpy(llrz, ((ipass - 5) % 2 == 0) ? llrc : llra, sizeof llrz);
            float apmag = 0;
            for (int i = 0; i < 174; i++) if (fabsf(llrz[i]) > apmag) apmag = fabsf(llrz[i]);
            apmag *= 1.1f;
            *iaptype = naptypes[p->nqso_progress][(ipass - 4) / 2 - 1];
            if (*iaptype >= 3 && fabsf(*f1 - p->nfqso) > p->napwid && fabsf(*f1 - p->nftx) > p->napwid) continue;
            if (*iaptype >= 2 && ap->apsym[0] > 1) continue;
            if (*iaptype >= 3 && ap->apsym[29] > 1) continue;
            memset(apmask, 0, sizeof apmask);
            if (*iaptype == 1) {
                for (int i = 0; i < 29; i++) { apmask[i] = 1; llrz[i] = apmag * (2 * mcq[i] - 1); }
                apmask[74] = apmask[75] = apmask[76] = 1;
                llrz[74] = llrz[75] = -apmag; llrz[76] = apmag;
            } else if (*iaptype == 2) {
                for (int i = 0; i < 29; i++) { apmask[i] = 1; llrz[i] = apmag * ap->apsym[i]; }
                apmask[74] = apmask[75] = apmask[76] = 1;
                llrz[74] = llrz[75] = -apmag; llrz[76] = apmag;
            } else if (*iaptype == 3) {                   /* MyCall DxCall ??? */
                for (int i = 0; i < 58; i++) { apmask[i] = 1; llrz[i] = apmag * ap->apsym[i]; }
                apmask[74] = apmask[75] = apmask[76] = 1;
                llrz[74] = llrz[75] = -apmag; llrz[76] = apmag;
            } else {                                      /* 4/5/6: ... RRR / 73 / RR73 */
                static const int mrrr[19]  = {0,1,1,1,1,1,1,0,1,0,0,1,0,0,1,0,0,0,1};
                static const int m73[19]   = {0,1,1,1,1,1,1,0,1,0,0,1,0,1,0,0,0,0,1};
                static const int mrr73[19] = {0,1,1,1,1,1,1,0,0,1,1,1,0,1,0,1,0,0,1};
                const int* m = *iaptype == 4 ? mrrr : *iaptype == 5 ? m73 : mrr73;
                for (int i = 0; i < 77; i++) apmask[i] = 1;
                for (int i = 0; i < 58; i++) llrz[i] = apmag * ap->apsym[i];
                for (int i = 0; i < 19; i++) llrz[58 + i] = apmag * (2 * m[i] - 1);
            }
        }
        int8_t message91[91], cw[174];
        int ntype;
        *dmin = 0;
        PROF_BEGIN(3);
        decode174_91(llrz, g_piece_maxosd, g_piece_ndeep, apmask, message91, cw, &ntype, nharderrors, dmin);
        PROF_END(3);
        if (*nharderrors < 0 || *nharderrors > 36) continue;
        int nz = 0;
        for (int i = 0; i < 174; i++) nz += cw[i] == 0;
        if (nz == 174) continue;
        int n3 = message91[71] * 4 + message91[72] * 2 + message91[73];
        int i3 = message91[74] * 4 + message91[75] * 2 + message91[76];
        if (i3 > 5 || (i3 == 0 && n3 > 6)) continue;
        if (i3 == 0 && n3 == 2) continue;
        ftx_message_t m;
        memset(&m, 0, sizeof m);
        bits_to_payload(message91, m.payload);
        ftx_message_offsets_t off;
        char text[64];
        g_hash = p->hash_if;
        int ok = (i3 == 5)              ? unpack_i3_5(message91, text)
               : (i3 == 0 && n3 == 1)   ? unpack_dxped(message91, text)
               : ftx_message_decode(&m, &lk_if, text, &off) == FTX_MESSAGE_RC_OK;
        if (ok && (i3 == 1 || i3 == 2) && !std_ok_like_jt9(message91, text)) ok = 0;
        if (ok && strncmp(text, "CQ <", 4) == 0) ok = 0;           /* unpack77's last rule */
        unpack77_save(&m, message91, i3);
        if (!ok || strstr(text, "/R") || strncmp(text, "TU; ", 4) == 0)
            if (i3 >= 1 && i3 <= 3) continue;
        if (!ok) continue;
        if (snprintf(msg37, 38, "%s", text) >= 38) continue;   /* longer than jt9's msg37 */
        uint8_t tones[NN];
        ft8_encode(m.payload, tones);
        for (int i = 0; i < NN; i++) itone[i] = tones[i];
#ifndef WSX_PIECE_ONLY
        if (lsubtract) subtractft8(dd0, itone, *f1, *xdt, 0);
#else
        (void)lsubtract;
#endif
        float xsig = 0, xnoi = 0;
        for (int i = 1; i <= NN; i++) {
            xsig += s8[i][itone[i - 1]] * s8[i][itone[i - 1]];
            int ios = (itone[i - 1] + 4) % 7;
            xnoi += s8[i][ios] * s8[i][ios];
        }
        float xsnr = 0.001f, xsnr2 = 0.001f;
        float arg = xsig / xnoi - 1.0f;
        if (arg > 0.1f) xsnr = arg;
        arg = xsig / xbase / 3.0e6f - 1.0f;
        if (arg > 0.1f) xsnr2 = arg;
        xsnr = 10.0f * log10f(xsnr) - 27.0f;
        xsnr2 = 10.0f * log10f(xsnr2) - 27.0f;
        xsnr = xsnr2;                                  /* .not.nagain */
        if (nsync <= 10 && xsnr < -25.0f) return 1;
        if (xsnr < -25.0f) xsnr = -25.0f;
        *xsnr_out = xsnr;
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------- ft8_decode (driver) */

static WSX_BSS struct {
#ifndef WSX_PIECE_ONLY
    float dd[NMAX], dd1[NMAX];   /* the piece path uses g_pdd instead */
#endif
    int ndec_early;
    int itone_save[MAX_EARLY][NN];
    float f1_save[MAX_EARLY], xdt_save[MAX_EARLY];
    int lsubtracted[MAX_EARLY];
    char allmessages[MAX_EARLY][38];
    int ndecodes;
    float sbase[NH1 + 1];
    float candidate[MAXCAND][3];
} st;

#ifndef WSX_PIECE_ONLY   /* not on the piece path - see wsx_piece_begin */
/* ================================================================== ft8_a7
 * lib/ft8/ft8_a7.f90: "a7" decodes. Every decode is remembered per even/odd
 * sequence; at nzhsym 50 each message heard in the SAME sequence one cycle
 * earlier is tried again as 206 candidate messages (the next steps of that
 * QSO) at its old frequency and DT. jt9 on one WAV never has a previous
 * cycle, so this is the part of WSJT-X's live decoder that a single-file
 * referee cannot see; jt9 given many WAVs in ONE process does run it.
 *
 * Not reproduced: candidates genft8 would pack as anything other than i3=1
 * or i3=4 (a nonstandard call with a report). They are skipped (distance
 * 1e30) instead of packed as free text; jt9's free-text codeword for such a
 * message cannot be the best match for a real a7 signal. */
#define MAXDEC 200
static float a7_dt0[MAXDEC][2][2], a7_f0[MAXDEC][2][2];   /* (i, j=seq, k=prev/cur) */
static char  a7_msg0[MAXDEC][2][2][38];
static int   a7_ndec[2][2];
static int   a7_jseq;
static int   a7_nutc0 = -1;

/* chkcall.f90 */
static int chkcall(const char* w)
{
    int n1 = (int)strlen(w);
    if (n1 > 11 || strpbrk(w, ".+-?")) return 0;
    const char* sl = strchr(w, '/');
    if (n1 > 6 && !sl) return 0;
    char bc[14];
    snprintf(bc, 7, "%s", w);                                  /* bc = w(1:6) */
    if (sl) {
        int i0 = (int)(sl - w) + 1;                            /* 1-based */
        if ((i0 - 1 > n1 - i0 ? i0 - 1 : n1 - i0) > 6) return 0;
        if (i0 >= 2 && i0 <= n1 - 1) {
            if (i0 - 1 <= n1 - i0) snprintf(bc, sizeof bc, "%s", w + i0);
            else { memcpy(bc, w, i0 - 1); bc[i0 - 1] = 0; }
        }
    }
    int nbc = (int)strlen(bc);
    while (nbc > 0 && bc[nbc - 1] == ' ') nbc--;
    if (nbc > 6) return 0;
    #define ISL(c) ((c) >= 'A' && (c) <= 'Z')
    #define ISD(c) ((c) >= '0' && (c) <= '9')
    if (!ISL(bc[0]) && !(nbc > 1 && ISL(bc[1]))) return 0;
    if (bc[0] == 'Q' && strncmp(bc, "QU1RK", 5) != 0) return 0;
    int i1 = 0;
    if (nbc > 1 && ISD(bc[1])) i1 = 2;
    if (nbc > 2 && ISD(bc[2])) i1 = 3;
    if (i1 == 0 || i1 == nbc) return 0;
    int n = 0;
    for (int i = i1; i < nbc; i++) { if (!ISL(bc[i])) return 0; n++; }
    return n >= 1 && n <= 3;
}

/* stdcall (qra/q65/q65_set_list.f90) */
static int stdcall(const char* c)
{
    int n = (int)strlen(c);
    while (n > 0 && c[n - 1] == ' ') n--;
    int i;
    for (i = n; i >= 2; i--) if (ISD(c[i - 1])) break;
    int iarea = i, npdig = 0, nplet = 0, nslet = 0;
    for (i = 1; i <= iarea - 1; i++) { if (ISD(c[i - 1])) npdig++; if (ISL(c[i - 1])) nplet++; }
    for (i = iarea + 1; i <= n; i++) if (ISL(c[i - 1])) nslet++;
    return !(iarea < 2 || iarea > 3 || nplet == 0 || npdig >= iarea - 1 || nslet > 3);
}

/* split77.f90: words (upper case, <= 13 chars); nw is computed BEFORE the
 * "CQ xx CALL" merge and not after it, exactly as in the Fortran. */
static int split77(const char* msg, char w[19][14], int nw[19])
{
    int k = 0, n = 0;
    char c0 = ' ';
    memset(w, 0, 19 * 14);
    memset(nw, 0, 19 * sizeof(int));
    for (const char* p = msg; *p; p++) {
        char c = *p;
        if (c == ' ' && c0 == ' ') continue;
        if (c != ' ' && c0 == ' ') { if (k == 19) break; k++; n = 0; }
        n++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != ' ' && n <= 13) w[k - 1][n - 1] = c;
        c0 = c;
    }
    for (int i = 0; i < k; i++) nw[i] = (int)strlen(w[i]);
    if (k >= 3 && chkcall(w[2]) && strcmp(w[0], "CQ") == 0) {
        char t[14];
        snprintf(t, sizeof t, "CQ_%.10s", w[1]);
        strcpy(w[0], t);
        for (int i = 1; i < 18; i++) strcpy(w[i], w[i + 1]);
        w[18][0] = 0;
        k--;
    }
    return k;
}

static int isgrid4(const char* g)
{
    return strlen(g) == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' &&
           ISD(g[2]) && ISD(g[3]);
}

static void rtrim(char* s) { int n = (int)strlen(s); while (n > 0 && s[n - 1] == ' ') s[--n] = 0; }

/* ft8_a7_save */
static void ft8_a7_save(int jseq, float dt, float f, const char* msg)
{
    char w[19][14];
    int nw[19];
    if (strchr(msg, '/') || strchr(msg, '<')) return;
    int nwords = split77(msg, w, nw);
    if (nwords < 1) return;
    if (strncmp(w[0], "CQ_", 3) == 0) return;
    int j = jseq;
    a7_ndec[j][1]++;
    int i = a7_ndec[j][1];
    if (i > MAXDEC) return;
    a7_dt0[i - 1][j][1] = dt;
    a7_f0[i - 1][j][1] = f;
    char* m0 = a7_msg0[i - 1][j][1];
    snprintf(m0, 38, "%s %s", w[0], w[1]);
    if (strcmp(w[0], "CQ") == 0 && nw[1] <= 2) snprintf(m0, 38, "CQ %s %s", w[1], w[2]);
    rtrim(m0);
    if (isgrid4(w[nwords - 1])) {
        char t[38];
        snprintf(t, sizeof t, "%s %s", m0, w[nwords - 1]);
        strcpy(m0, t);
    }
    nwords = split77(m0, w, nw);
    char key[16];
    snprintf(key, sizeof key, " %s", w[1]);
    int nprev = a7_ndec[j][0] < MAXDEC ? a7_ndec[j][0] : MAXDEC;
    for (int q = 0; q < nprev; q++) {
        if (a7_f0[q][j][0] <= -98.0f) continue;
        const char* hit = strstr(a7_msg0[q][j][0], key);
        int i2 = hit ? (int)(hit - a7_msg0[q][j][0]) + 1 : 0;   /* Fortran index() */
        if (fabsf(f - a7_f0[q][j][0]) <= 3.0f && i2 >= 3) a7_f0[q][j][0] = -98.0f;
    }
}

/* The rotation at the top of ft8_decode's decode(): a new UTC moves this
 * sequence's current table to "previous". */
static void a7_new_call(int nutc, int nzhsym)
{
    if (nzhsym == 41) a7_jseq = (nutc / 5) % 2;
    if (a7_nutc0 == -1) {
        memset(a7_msg0, 0, sizeof a7_msg0);
        memset(a7_dt0, 0, sizeof a7_dt0);
        memset(a7_f0, 0, sizeof a7_f0);
    }
    if (nzhsym == 41 || nutc != a7_nutc0) {
        int j = a7_jseq, iz = a7_ndec[j][1];
        int n = iz < MAXDEC ? iz : MAXDEC;
        for (int i = 0; i < n; i++) {
            a7_dt0[i][j][0] = a7_dt0[i][j][1];
            a7_f0[i][j][0] = a7_f0[i][j][1];
            memcpy(a7_msg0[i][j][0], a7_msg0[i][j][1], 38);
        }
        a7_ndec[j][0] = iz;
        a7_ndec[j][1] = 0;
        a7_nutc0 = nutc;
        for (int i = 0; i < MAXDEC; i++) { a7_dt0[i][j][1] = 0; a7_f0[i][j][1] = 0; }
    }
}

/* pack28 for what a7 packs: CQ, a <hash>, a nonstandard call (hashed) or a
 * standard call. The hash saves pack28 makes are left to the caller. */
static uint32_t ihashcall22(const char* cw)
{
    static const char c[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
    uint64_t n8 = 0;
    int L = (int)strlen(cw);
    for (int i = 0; i < 11; i++) {
        char ch = i < L ? cw[i] : ' ';
        const char* q = strchr(c, ch);
        n8 = 38 * n8 + (uint64_t)((q && ch) ? (q - c) : 0);
    }
    return (uint32_t)((47055833459ull * n8) >> (64 - 22));
}
static uint32_t pack28(const char* c13)
{
    const uint32_t NTOKENS = 2063592, MAX22 = 4194304;
    static const char a1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", a2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                      a3[] = "0123456789", a4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (strcmp(c13, "CQ") == 0) return 2;
    if (c13[0] == '<') {
        char t[14];
        snprintf(t, sizeof t, "%s", c13 + 1);
        char* gt = strchr(t, '>');
        if (gt) *gt = 0;
        return NTOKENS + ihashcall22(t);
    }
    if (!stdcall(c13)) return NTOKENS + ihashcall22(c13);
    int n = (int)strlen(c13), iarea;
    for (iarea = n; iarea >= 2; iarea--) if (ISD(c13[iarea - 1])) break;
    char cs[7] = "      ";
    if (iarea == 2) { cs[0] = ' '; memcpy(cs + 1, c13, n < 5 ? n : 5); }
    else memcpy(cs, c13, n < 6 ? n : 6);
    int i1 = (int)(strchr(a1, cs[0]) - a1), i2 = (int)(strchr(a2, cs[1]) - a2),
        i3 = (int)(strchr(a3, cs[2]) - a3), i4 = (int)(strchr(a4, cs[3]) - a4),
        i5 = (int)(strchr(a4, cs[4]) - a4), i6 = (int)(strchr(a4, cs[5]) - a4);
    uint32_t n28 = 36u * 10 * 27 * 27 * 27 * i1 + 10u * 27 * 27 * 27 * i2 + 27u * 27 * 27 * i3 +
                   27u * 27 * i4 + 27u * i5 + i6;
    return (n28 + NTOKENS + MAX22) & ((1u << 28) - 1);
}

/* A token is i3=1-packable as a call field: CQ, <hash>, or a standard call. */
static int c28_ok(const char* t) { return strcmp(t, "CQ") == 0 || t[0] == '<' || stdcall(t); }

/* genft8 + encode174_91 for one a7 candidate. Returns 0 when the message is
 * a form this port does not pack (see the note at the top). */
static int a7_pack(const char* msg, int8_t* cw, int* itone, char* msgsent)
{
    char w[19][14];
    int nw[19];
    int nwords = 0;
    {   /* plain split on blanks: these messages never start "CQ xx CALL" */
        char t[40];
        snprintf(t, sizeof t, "%s", msg);
        for (char* tok = strtok(t, " "); tok && nwords < 19; tok = strtok(NULL, " "))
            snprintf(w[nwords++], 14, "%s", tok);
    }
    (void)nw;
    int8_t b[77] = {0};
    uint8_t payload[10];
    if (nwords >= 2 && nwords <= 3 && c28_ok(w[0]) && c28_ok(w[1])) {
        const uint32_t MAXGRID4 = 32400;
        uint32_t igrid4 = MAXGRID4 + 1, ir = 0;
        if (nwords == 3) {
            const char* x = w[2];
            char g4[5];
            snprintf(g4, sizeof g4, "%s", x);
            /* ⛔ pack77_1 tests is_grid4(w(nwords)(1:4)) FIRST, so "RR73" goes
             * out as the GRID SQUARE RR73, never as irpt=3. Tested the other
             * way round, every a7 RR73 candidate scored as noise: 17 of the
             * port's 98 missing a7 decodes, 2026-11-21. */
            if (isgrid4(g4))
                igrid4 = (uint32_t)((((x[0] - 'A') * 18 + (x[1] - 'A')) * 10 + (x[2] - '0')) * 10 + (x[3] - '0'));
            else if (strcmp(x, "RRR") == 0) igrid4 = MAXGRID4 + 2;
            else if (strcmp(x, "RR73") == 0) igrid4 = MAXGRID4 + 3;
            else if (strcmp(x, "73") == 0) igrid4 = MAXGRID4 + 4;
            else {
                if (x[0] == 'R') { ir = 1; x++; }
                int irpt = atoi(x);
                if (irpt >= -50 && irpt <= -31) irpt += 101;
                igrid4 = MAXGRID4 + (uint32_t)(irpt + 35);
            }
        }
        uint32_t n28a = pack28(w[0]), n28b = pack28(w[1]);
        int q = 0;
        for (int i = 27; i >= 0; i--) b[q++] = (n28a >> i) & 1;
        b[q++] = 0;                                            /* ipa */
        for (int i = 27; i >= 0; i--) b[q++] = (n28b >> i) & 1;
        b[q++] = 0;                                            /* ipb */
        b[q++] = (int8_t)ir;
        for (int i = 14; i >= 0; i--) b[q++] = (igrid4 >> i) & 1;
        b[q++] = 0; b[q++] = 0; b[q++] = 1;                    /* i3 = 1 */
        bits_to_payload(b, payload);
        snprintf(msgsent, 38, "%s", msg);
        /* ⛔ genft8 UNPACKS what it packed and, if unpack77 fails, sends the
         * ALL-ZERO codeword as "*** bad message ***". Every "QU1RK ..."
         * candidate fails (callok: no call starts with Q), as does CQ with
         * R or a report. In jt9 those are all one zero codeword; packed as
         * real messages here they set a lower dmin2 and rejected true a7
         * decodes on the 1.3 ratio (CQ CT7AXN IM59, 2026-11-21). */
        if (!std_ok_like_jt9(b, msg)) {
            memset(cw, 0, 174);
            for (int i = 0; i < NN; i++) itone[i] = 0;
            snprintf(msgsent, 38, "*** bad message ***");
            return 1;
        }
    } else if (nwords == 2 || nwords == 3) {
        /* pack77_4 (i3=4: h12 c58 iflip nrpt icq). ft8_lib's encoder was
         * used here first and did not give jt9's codeword for "CQ <nonstd>":
         * 3 a7 CQ decodes of 995 missed, 2026-11-21. */
        char c1[14], c2[14];
        snprintf(c1, sizeof c1, "%s", w[0][0] == '<' ? w[0] + 1 : w[0]);
        snprintf(c2, sizeof c2, "%s", w[1][0] == '<' ? w[1] + 1 : w[1]);
        char* gt;
        if (w[0][0] == '<' && (gt = strchr(c1, '>'))) *gt = 0;
        if (w[1][0] == '<' && (gt = strchr(c2, '>'))) *gt = 0;
        int ok1 = chkcall(c1), ok2 = chkcall(c2);
        if (ok1 && ok2 && strlen(c1) <= 6 && strlen(c2) <= 6 && !strchr(c1, '/') && !strchr(c2, '/'))
            return 0;                                          /* call == bcall: not type 4 */
        int i3 = -1, icq = 0, iflip = 0, nrpt = 0;
        uint32_t n12 = 0;
        const char* c11src = NULL;
        if (strcmp(w[0], "CQ") == 0 || (ok1 && ok2)) {
            if (strcmp(w[0], "CQ") == 0 && strlen(w[1]) <= 4) return 0;
            i3 = 4;
            if (strcmp(w[0], "CQ") == 0) icq = 1;
        }
        /* ⛔ icq: pack77_4 sets n12=0 and then calls save_hash_call(w(2),
         * n10,n12,n22), which RETURNS the call's 12-bit hash in n12. So the
         * h12 field carries hash12(call_2), not 0. */
        if (icq) { c11src = c2; n12 = ihashcall22(c2) >> 10; save_hash_call(w[1]); }
        else if (w[0][0] == '<') { i3 = 4; n12 = ihashcall22(c1) >> 10; save_hash_call(w[0]); c11src = c2; }
        else if (w[1][0] == '<') { i3 = 4; iflip = 1; n12 = ihashcall22(c2) >> 10; save_hash_call(w[1]); c11src = c1; }
        if (i3 != 4 || !c11src) return 0;
        static const char cset[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
        char c11[12];
        snprintf(c11, sizeof c11, "%11.11s", c11src);              /* adjustr */
        uint64_t n58 = 0;
        for (int i = 0; i < 11; i++) {
            const char* q2 = strchr(cset, c11[i]);
            n58 = n58 * 38 + (uint64_t)((q2 && c11[i]) ? (q2 - cset) : 0);
        }
        if (nwords == 3) {
            if (strcmp(w[2], "RRR") == 0) nrpt = 1;
            if (strcmp(w[2], "RR73") == 0) nrpt = 2;
            if (strcmp(w[2], "73") == 0) nrpt = 3;
        }
        if (icq) { iflip = 0; nrpt = 0; }
        int q = 0;
        for (int i = 11; i >= 0; i--) b[q++] = (n12 >> i) & 1;
        for (int i = 57; i >= 0; i--) b[q++] = (n58 >> i) & 1;
        b[q++] = (int8_t)iflip;
        b[q++] = (nrpt >> 1) & 1; b[q++] = nrpt & 1;
        b[q++] = (int8_t)icq;
        b[q++] = 1; b[q++] = 0; b[q++] = 0;                    /* i3 = 4 */
        bits_to_payload(b, payload);
        ftx_message_t m;
        memset(&m, 0, sizeof m);
        memcpy(m.payload, payload, 10);
        ftx_message_offsets_t off;
        char t[64];
        if (ftx_message_decode(&m, &lk_if, t, &off) != FTX_MESSAGE_RC_OK) return 0;
        snprintf(msgsent, 38, "%s", t);
    } else {
        return 0;
    }
    /* encode174_91: 77 bits + CRC14 (computed over 77 bits and 5 zeros) */
    int8_t m91[91], m96[96] = {0};
    memcpy(m96, b, 77);
    int crc = get_crc14(m96, 96);
    memcpy(m91, b, 77);
    for (int i = 0; i < 14; i++) m91[77 + i] = (crc >> (13 - i)) & 1;
    encode174_91_nocrc(m91, cw);
    uint8_t tones[NN];
    ft8_encode(payload, tones);
    for (int i = 0; i < NN; i++) itone[i] = tones[i];
    return 1;
}

/* ft8_a7d. Returns nharderrors (-1 = no decode). */
static int ft8_a7d(float* dd0, int* newdat, const char* call_1, const char* call_2,
                   const char* grid4, float* xdt, float* f1, float xbase, float* dmin_out,
                   char* msg37, float* xsnr)
{
    static cf cd0[NFFT2], ctwk[32], csymb[32], cs[NN + 1][8];
    static float s8[NN + 1][8];
    float bmeta[174], bmetb[174], bmetc[174], bmetd[174];
    float llr[4][174], s2[512], dmm[206];
#ifdef A7_DEBUG
    float dm1[6] = {0};
#endif
    const float fs2 = 12000.0f / NDOWN, dt2 = 1.0f / fs2, twopi = 8.0f * atanf(1.0f);

    int std_1 = stdcall(call_1);
    if (strncmp(call_1, "CQ", 2) == 0 && (call_1[2] == 0 || call_1[2] == ' ')) std_1 = 1;
    int std_2 = stdcall(call_2);

    float delfbest = 0;
    int ibest = 0;
    ft8_downsample(dd0, newdat, *f1, cd0);
    int i0 = nint((*xdt + 0.5f) * fs2);
    float smax = 0;
    for (int idt = i0 - 10; idt <= i0 + 10; idt++) {
        float sync = sync8d(cd0, idt, ctwk, 0);
        if (sync > smax) { smax = sync; ibest = idt; }
    }
    smax = 0;
    for (int ifr = -5; ifr <= 5; ifr++) {
        float delf = ifr * 0.5f, dphi = twopi * delf * dt2, phi = 0;
        for (int i = 0; i < 32; i++) { ctwk[i] = cosf(phi) + I * sinf(phi); phi = fmodf(phi + dphi, twopi); }
        float sync = sync8d(cd0, ibest, ctwk, 1);
        if (sync > smax) { smax = sync; delfbest = delf; }
    }
    *f1 = *f1 + delfbest;
    int nd = 0;
    ft8_downsample(dd0, &nd, *f1, cd0);
    float ss[9];
    for (int idt = -4; idt <= 4; idt++) ss[idt + 4] = sync8d(cd0, ibest + idt, ctwk, 0);
    int il = 0;
    for (int q = 1; q < 9; q++) if (ss[q] > ss[il]) il = q;
    ibest = il - 4 + ibest;
    *xdt = (ibest - 1) * dt2 - 0.5f;
    for (int k = 1; k <= NN; k++) {
        int i1 = ibest + (k - 1) * 32;
        if (i1 >= 0 && i1 + 31 <= NP2 - 1) memcpy(csymb, cd0 + i1, sizeof csymb);
        else memset(csymb, 0, sizeof csymb);
        c2c(csymb, 32, -1);
        for (int q = 0; q < 8; q++) { cs[k][q] = csymb[q] / 1e3f; s8[k][q] = cabsf(csymb[q]); }
    }
    for (int nsym = 1; nsym <= 3; nsym++) {
        int nt = 1 << (3 * nsym);
        for (int ihalf = 1; ihalf <= 2; ihalf++)
            for (int k = 1; k <= 29; k += nsym) {
                int ks = ihalf == 1 ? k + 7 : k + 43;
                for (int i = 0; i < nt; i++) {
                    int i1 = i / 64, i2 = (i & 63) / 8, i3 = i & 7;
                    if (nsym == 1) s2[i] = cabsf(cs[ks][graymap[i3]]);
                    else if (nsym == 2) s2[i] = cabsf(cs[ks][graymap[i2]] + cs[ks + 1][graymap[i3]]);
                    else s2[i] = cabsf(cs[ks][graymap[i1]] + cs[ks + 1][graymap[i2]] + cs[ks + 2][graymap[i3]]);
                }
                int i32 = 1 + (k - 1) * 3 + (ihalf - 1) * 87;
                int ibmax = nsym == 1 ? 2 : nsym == 2 ? 5 : 8;
                for (int ib = 0; ib <= ibmax; ib++) {
                    int bit = ibmax - ib;
                    float m1 = -INFINITY, m0 = -INFINITY;
                    for (int i = 0; i < nt; i++) {
                        if ((i >> bit) & 1) { if (s2[i] > m1) m1 = s2[i]; }
                        else if (s2[i] > m0) m0 = s2[i];
                    }
                    float bm = m1 - m0;
                    if (i32 + ib > 174) continue;
                    if (nsym == 1) {
                        bmeta[i32 + ib - 1] = bm;
                        float den = fmaxf(m1, m0);
                        bmetd[i32 + ib - 1] = den > 0.f ? bm / den : 0.f;
                    } else if (nsym == 2) bmetb[i32 + ib - 1] = bm;
                    else bmetc[i32 + ib - 1] = bm;
                }
            }
    }
    normalizebmet(bmeta, 174); normalizebmet(bmetb, 174);
    normalizebmet(bmetc, 174); normalizebmet(bmetd, 174);
    for (int i = 0; i < 174; i++) {
        llr[0][i] = 2.83f * bmeta[i]; llr[1][i] = 2.83f * bmetb[i];
        llr[2][i] = 2.83f * bmetc[i]; llr[3][i] = 2.83f * bmetd[i];
    }

    /* pack28's hash saves: every candidate packs call_1 (or QU1RK) and call_2 */
    save_hash_call((strncmp(call_1, "CQ", 2) == 0) ? "QU1RK" : call_1);
    save_hash_call(call_2);

    float pbest = 0, dmin = 1.e30f;
    int nharderrors = -1;
    char msgbest[38] = {0};
    const int is_cq = strncmp(call_1, "CQ", 2) == 0 && (call_1[2] == 0 || call_1[2] == ' ');
    for (int imsg = 1; imsg <= 206; imsg++) {
        char msg[40];
        int i = imsg;
        snprintf(msg, sizeof msg, "%s %s", call_1, call_2);
        if (is_cq && i != 5) snprintf(msg, sizeof msg, "QU1RK %s", call_2);
        if (!std_1) {
            if (i == 1 || i >= 6) snprintf(msg, sizeof msg, "<%s> %s", call_1, call_2);
            if (i >= 2 && i <= 4) snprintf(msg, sizeof msg, "%s <%s>", call_1, call_2);
        } else if (!std_2) {
            if (i <= 4 || i == 6) snprintf(msg, sizeof msg, "<%s> %s", call_1, call_2);
            if (i >= 7) snprintf(msg, sizeof msg, "%s <%s>", call_1, call_2);
        }
        char base[40];
        snprintf(base, sizeof base, "%s", msg);
        rtrim(base);
        if (i == 2) snprintf(msg, sizeof msg, "%s RRR", base);
        if (i == 3) snprintf(msg, sizeof msg, "%s RR73", base);
        if (i == 4) snprintf(msg, sizeof msg, "%s 73", base);
        if (i == 5) {
            if (std_2) {
                snprintf(msg, sizeof msg, "CQ %s", call_2);
                if (call_1[2] == '_') snprintf(msg, sizeof msg, "%s %s", call_1, call_2);
                if (strcmp(grid4, "RR73") != 0) { char t[40]; snprintf(t, sizeof t, "%s %s", msg, grid4); strcpy(msg, t); }
            } else snprintf(msg, sizeof msg, "CQ %s", call_2);
        }
        if (i == 6 && std_2) snprintf(msg, sizeof msg, "%s %s", base, grid4);
        if (i >= 7) {
            int isnr = -50 + (i - 7) / 2;
            if (i & 1) snprintf(msg, sizeof msg, "%s %c%02d", base, isnr < 0 ? '-' : '+', abs(isnr));
            else snprintf(msg, sizeof msg, "%s R%c%02d", base, isnr < 0 ? '-' : '+', abs(isnr));
        }
        rtrim(msg);

        int8_t cw[174];
        int itone[NN];
        char msgsent[38];
        dmm[imsg - 1] = 1.e30f;
        if (!a7_pack(msg, cw, itone, msgsent)) continue;
        float pow_ = 0;
        for (int k = 1; k <= NN; k++) pow_ += s8[k][itone[k - 1]] * s8[k][itone[k - 1]];
        float d[4];
        for (int m = 0; m < 4; m++) {
            float s = 0;
            for (int q = 0; q < 174; q++) s += ((llr[m][q] >= 0.f) ^ cw[q]) * fabsf(llr[m][q]);
            d[m] = s;
        }
        float dm = fminf(fminf(d[0], d[1]), fminf(d[2], d[3]));
        dmm[imsg - 1] = dm;
#ifdef A7_DEBUG
        if (imsg <= 6) dm1[imsg - 1] = dm;
        if (getenv("A7_ALL") && strstr(call_2, getenv("A7_ALL"))) fprintf(stderr, "    %3d %7.1f %s\n", imsg, dm, msg);
#endif
        if (dm < dmin) {
            dmin = dm;
            snprintf(msgbest, sizeof msgbest, "%s", msgsent);
            pbest = pow_;
            int m = (dm == d[0]) ? 0 : (dm == d[1]) ? 1 : (dm == d[2]) ? 2 : 3;
            nharderrors = 0;
            for (int q = 0; q < 174; q++) nharderrors += ((2 * cw[q] - 1) * llr[m][q] < 0.f);
        }
    }
    int il1 = 0;
    for (int q = 1; q < 206; q++) if (dmm[q] < dmm[il1]) il1 = q;
    dmm[il1] = 1.e30f;
    int il2 = 0;
    for (int q = 1; q < 206; q++) if (dmm[q] < dmm[il2]) il2 = q;
    float dmin2 = dmm[il2];
    *xsnr = -25.f;
    float arg = pbest / xbase / 3.0e6f - 1.0f;
    if (arg > 0.f) *xsnr = fmaxf(-25.0f, 10.0f * log10f(arg) - 27.0f);
#ifdef A7_DEBUG
    fprintf(stderr, "  d[1..6]=%.1f %.1f %.1f %.1f %.1f %.1f\n", dm1[0], dm1[1], dm1[2], dm1[3], dm1[4], dm1[5]);
    fprintf(stderr, "a7d '%s' '%s' g='%s' f=%.1f dt=%.2f dmin=%.1f dmin2=%.1f nh=%d best='%s'\n",
            call_1, call_2, grid4, *f1, *xdt, dmin, dmin2, nharderrors, msgbest);
#endif
    if (dmin > 100.0f || dmin2 / dmin < 1.3f) nharderrors = -1;
    snprintf(msg37, 38, "%s", msgbest);
    if (strncmp(msg37, "CQ ", 3) == 0 && std_2 && strcmp(grid4, "    ") == 0) nharderrors = -1;
    if (strncmp(msg37, "QU1RK ", 6) == 0) nharderrors = -1;
    *dmin_out = dmin;
    return nharderrors;
}

/* Every decode is reported through here: decoder.f90's ft8_decoded callback
 * clears ltry_a8 for a decode within 3 Hz of the RX frequency. */
static int a8_ltry;
static void emit(wsx_callback_t cb, void* ctx, const wsx_params_t* p, float sync, int snr, float dt,
                 float freq, const char* msg, int nap, float qual)
{
    if (fabsf(freq - (float)p->nfqso) < 3.0f) a8_ltry = 0;
    if (cb) cb(ctx, sync, snr, dt, freq, msg, nap, qual);
}

/* The a7 block at the end of ft8_decode's decode(), nzhsym 50 only. */
#ifdef A7_DEBUG
static float a7_raw[NMAX];
#endif
static int a7_run(const wsx_params_t* p, wsx_callback_t cb, void* ctx)
{
    int la8 = 1;
    int j = a7_jseq;
    if (!p->lapon || a7_ndec[j][0] < 1) return la8;
    int newdat = 1;
    int n = a7_ndec[j][0] < MAXDEC ? a7_ndec[j][0] : MAXDEC;
    for (int i = 0; i < n; i++) {
        if (a7_f0[i][j][0] == -99.0f) break;
        if (a7_f0[i][j][0] == -98.0f) continue;
        const char* m0 = a7_msg0[i][j][0];
        if (strchr(m0, '<')) continue;
        char call_1[14] = {0}, call_2[14] = {0}, grid4[5] = "    ";
        const char* s1 = strchr(m0, ' ');
        if (!s1) continue;                       /* Fortran: i1 = 0, call_1 empty */
        snprintf(call_1, sizeof call_1, "%.*s", (int)(s1 - m0), m0);
        const char* s2 = strchr(s1 + 1, ' ');
        if (s2) {
            snprintf(call_2, sizeof call_2, "%.*s", (int)(s2 - s1 - 1), s1 + 1);
            for (int q = 0; q < 4; q++) grid4[q] = s2[1 + q] ? s2[1 + q] : ' ';
            for (int q = 0; q < 4; q++) if (!s2[1 + q]) { for (int r = q; r < 4; r++) grid4[r] = ' '; break; }
        } else {
            snprintf(call_2, sizeof call_2, "%s", s1 + 1);
        }
        if (strcmp(grid4, "RR73") == 0 || strchr(grid4, '+') || strchr(grid4, '-')) strcpy(grid4, "    ");
        char g4[5];
        snprintf(g4, sizeof g4, "%s", grid4);
        rtrim(g4);
        float xdt = a7_dt0[i][j][0], f1 = a7_f0[i][j][0];
        int si = nint(f1 / 3.125f);
        if (si < 1) si = 1;
        if (si > NH1) si = NH1;
        float xbase = powf(10.0f, 0.1f * (st.sbase[si] - 40.0f));
        char msg37[38];
        float dmin, xsnr;
        /* a7d tests grid4 against "    " (blank) and "RR73"; it builds the
         * messages with the trimmed grid, as trim() does in Fortran. */
#ifdef A7_DEBUG
        if (getenv("A7_RAW")) {   /* debug: the raw slot instead of the residual */
            float x1 = xdt, f2 = f1, dm2, xs2; char m2[38]; int nd2 = 1;
            ft8_a7d(a7_raw, &nd2, call_1, call_2, g4[0] ? g4 : "    ", &x1, &f2, xbase, &dm2, m2, &xs2);
            newdat = 1;
        }
#endif
        int nh = ft8_a7d(st.dd, &newdat, call_1, call_2, g4[0] ? g4 : "    ", &xdt, &f1, xbase,
                         &dmin, msg37, &xsnr);
        if (nh >= 0) {
            int nsnr = (int)xsnr;                  /* nsnr=xsnr: truncation */
            if (p->hiscall[0] && strstr(msg37, p->hiscall)) la8 = 0;
            emit(cb, ctx, p, 10.0f, nsnr, xdt, f1, msg37, 7, 1.0f);
            ft8_a7_save(j, xdt, f1, msg37);
        }
    }
    return la8;
}

/* ================================================================== ft8_a8
 * lib/ft8/ft8_a8d.f90: "a8" list decoding at the RX frequency, only when a
 * DX call and grid are set (mid-QSO). Each of getmsg's 206 messages from the
 * DX station to us is turned into a waveform and correlated against the
 * downsampled audio over +-1 s of lag; the best fit is accepted on a symbol
 * probability test. */

/* getmsg (ft8_a7.f90) */
static void getmsg(int i, const char* mycall, const char* hiscall, const char* hisgrid, char* msg)
{
    int my_std = stdcall(mycall), his_std = stdcall(hiscall);
    int isnr = 0;
    char base[40];
    snprintf(msg, 38, "%s %s", mycall, hiscall);
    if (!my_std) {
        if (i == 1 || i >= 6) snprintf(msg, 38, "<%s> %s", mycall, hiscall);
        if (i >= 2 && i <= 4) snprintf(msg, 38, "%s <%s>", mycall, hiscall);
    } else if (!his_std) {
        if (i <= 4 || i == 6) snprintf(msg, 38, "<%s> %s", mycall, hiscall);
        if (i >= 7) snprintf(msg, 38, "%s <%s>", mycall, hiscall);
    }
    snprintf(base, sizeof base, "%s", msg);
    if (i == 2) snprintf(msg, 38, "%s RRR", base);
    if (i == 3) snprintf(msg, 38, "%s RR73", base);
    if (i == 4) snprintf(msg, 38, "%s 73", base);
    if (i == 5) {
        if (his_std) snprintf(msg, 38, "CQ %s %.4s", hiscall, hisgrid);
        else snprintf(msg, 38, "CQ %s", hiscall);
    }
    if (i == 6 && his_std) snprintf(msg, 38, "%s %.4s", base, hisgrid);
    if (i >= 7 && i <= 206) {
        isnr = -50 + (i - 7) / 2;
        if (i & 1) snprintf(msg, 38, "%s %c%02d", base, isnr < 0 ? '-' : '+', abs(isnr));
        else snprintf(msg, 38, "%s R%c%02d", base, isnr < 0 ? '-' : '+', abs(isnr));
    }
    rtrim(msg);
    if (abs(isnr) > 30) msg[0] = 0;
}

/* genft8's itone for an a8 message. A blank message is what pack77 makes
 * telemetry of: split77 finds no words, the "3z6" read of a blank field
 * gives three zeros, so i3=0 n3=5 with 71 zero bits. */
static int a8_tones(const char* msg, int* itone)
{
    if (!msg[0]) {
        int8_t b[77] = {0};
        b[71] = 1; b[72] = 0; b[73] = 1;                       /* n3 = 5, i3 = 0 */
        uint8_t payload[10], tones[NN];
        bits_to_payload(b, payload);
        ft8_encode(payload, tones);
        for (int i = 0; i < NN; i++) itone[i] = tones[i];
        return 1;
    }
    int8_t cw[174];
    char sent[38];
    return a7_pack(msg, cw, itone, sent);
}

/* twkfreq1.f90 */
static void twkfreq1(cf* ca, int npts, float fsample, const float* a, cf* cb)
{
    const float twopi = 6.283185307f;
    cf w = 1.0f, wstep;
    float x0 = 0.5f * (npts + 1), s = 2.0f / npts;
    for (int i = 1; i <= npts; i++) {
        float x = s * (i - x0);
        float p2 = 1.5f * x * x - 0.5f;
        float p3 = 2.5f * (x * x * x) - 1.5f * x;
        float p4 = 4.375f * (x * x * x * x) - 3.75f * (x * x) + 0.375f;
        float dphi = (a[0] + x * a[1] + p2 * a[2] + p3 * a[3] + p4 * a[4]) * (twopi / fsample);
        wstep = cosf(dphi) + I * sinf(dphi);
        w = w * wstep;
        cb[i - 1] = w * ca[i - 1];
    }
}

static void smo121(float* x, int nz)
{
    float x0 = x[0];
    for (int i = 1; i < nz - 1; i++) {
        float x1 = x[i];
        x[i] = 0.5f * x[i] + 0.25f * (x0 + x[i + 1]);
        x0 = x1;
    }
}

/* ft8_a8d. msgbest[0] == 0 means no decode. */
static void ft8_a8d(float* dd, const char* mycall, const char* dxcall, const char* dxgrid, float f1a,
                    float* xdt, float* fbest_out, float* xsnr, float* plog_out, char* msgbest)
{
    enum { NWAVE = NN * NSPS / NDOWN, NZZ = 3200, NFFTA = NZZ, NHA = NFFTA / 2, NMSGS = 206 };
    static cf cwave[NZZ], cd[NZZ], cd0[NZZ], csymb[32];
    static float s[NFFTA + 1], s0[NFFTA + 1], s1[NFFTA + 1];     /* index j + NHA */
    int itone[NN], itone_best[NN];
    float f1 = f1a;
    int newdat = 1;
    ft8_downsample(dd, &newdat, f1, cd);
    const float fac = 1.e-6f, fsd = 200.0f, dt = 1.0f / fsd, df = 200.0f / NFFTA;
    memset(s, 0, sizeof s);
    float sbest = 0, tbest = 0, fbest = 0;
    msgbest[0] = 0;
    memset(itone_best, 0, sizeof itone_best);
    for (int imsg = 1; imsg <= NMSGS; imsg++) {
        char msg[40];
        getmsg(imsg, mycall, dxcall, dxgrid, msg);
        if (!a8_tones(msg, itone)) continue;      /* a form this port does not pack */
        gen_ft8wave(itone, 32, fsd, 0.0f, cwave, NWAVE);
        for (int i = NWAVE; i < NZZ; i++) cwave[i] = 0;
        float spk = 0, fpk = 0, tpk = 0;
        int lagpk = 0, lag1 = -200, lag2 = 200, lagstep = 4;
        for (int iter = 1; iter <= 2; iter++) {
            if (iter == 2) { lag1 = lagpk - 8; lag2 = lagpk + 8; lagstep = 1; }
            for (int lag = lag1; lag <= lag2; lag += lagstep) {
                for (int i = 0; i < NWAVE; i++) {
                    int j = i + lag + 100;
                    cd0[i] = (j >= 0 && j <= NWAVE - 1) ? cd[j] * conjf(cwave[i]) : 0;
                }
                for (int i = NWAVE; i < NZZ; i++) cd0[i] = 0;
                c2c(cd0, NFFTA, -1);
                for (int i = 0; i < NFFTA; i++) {
                    int j = (i > NHA) ? i - NFFTA : i;
                    s[j + NHA] = fac * (crealf(cd0[i]) * crealf(cd0[i]) + cimagf(cd0[i]) * cimagf(cd0[i]));
                }
                smo121(s, NFFTA + 1);
                for (int j = -NHA; j <= NHA; j++)
                    if (s[j + NHA] > spk) {
                        spk = s[j + NHA];
                        fpk = j * df + f1;
                        lagpk = lag;
                        tpk = lag * dt;
                        memcpy(s0, s, sizeof s);
                    }
            }
        }
        if (spk > sbest) {
            sbest = spk; fbest = fpk; tbest = tpk;
            snprintf(msgbest, 38, "%s", msg);
            memcpy(itone_best, itone, sizeof itone);
            memcpy(s1, s0, sizeof s0);
        }
    }
    float a[5] = {0};
    a[0] = f1 - fbest;
    *fbest_out = fbest;
    if (fabsf(a[0]) > 5.0f) { msgbest[0] = 0; return; }
    twkfreq1(cd, NZZ, 200.0f, a, cd);
    *xdt = tbest;
    float ave = 0;
    for (int i = -200; i <= -100; i++) ave += s1[i + NHA];
    for (int i = 100; i <= 200; i++) ave += s1[i + NHA];
    ave /= 202.0f;
    for (int i = 0; i <= NFFTA; i++) s1[i] = s1[i] / ave - 1.0f;
    float s1pk = -INFINITY;
    for (int i = -32; i <= 32; i++) if (s1[i + NHA] > s1pk) s1pk = s1[i + NHA];
    float sig = 0;
    int nsig = 0;
    for (int i = -32; i <= 32; i++) {
        if (s1[i + NHA] < 0.5f * s1pk) continue;
        sig += s1[i + NHA];
        nsig++;
    }
    sig /= nsig;
    *xsnr = 10.0f * log10f(sig) - 35.0f;
    if (*xsnr < -30) *xsnr = -30;
    if (msgbest[0]) {
        float plog = 0, sum_sync = 0, sum_sig = 0, sum_big = 0, s8[8];
        int nhard = 0, nsum = 0;
        for (int k = 1; k <= NN; k++) {
            int i0 = 32 * (k - 1) + nint((tbest + 0.5f) / 0.005f);
            for (int i = 0; i < 32; i++) csymb[i] = (i0 + i >= 0 && i0 + i <= NZZ - 1) ? cd[i0 + i] : 0;
            c2c(csymb, 32, -1);
            float s8sum = 0;
            for (int i = 0; i < 8; i++) {
                s8[i] = crealf(csymb[i]) * crealf(csymb[i]) + cimagf(csymb[i]) * cimagf(csymb[i]);
                s8sum += s8[i];
            }
            if (s8sum > 0.0f) {
                float p = s8[itone_best[k - 1]] / s8sum;
                plog += logf(p);
                nsum++;
            }
            int ipk = 0;
            for (int i = 1; i < 8; i++) if (s8[i] > s8[ipk]) ipk = i;
            if (ipk != itone_best[k - 1]) nhard++;
            if (k <= 7 || (k >= 37 && k <= 43) || k >= 73) sum_sync += s8[itone_best[k - 1]];
            else sum_sig += s8[itone_best[k - 1]];
            sum_big += s8[ipk];
        }
        if (nsum < NN) plog += (NN - nsum) * logf(0.125f);
        float sigobig = (sum_sync + sum_sig) / sum_big;
        *plog_out = plog;
        if (nhard > 54 || plog < -159.0f || sigobig < 0.71f) msgbest[0] = 0;
    }
}

/* The a8 block at the end of ft8_decode's decode(), after a7. */
static void a8_run(const wsx_params_t* p, int la8, wsx_callback_t cb, void* ctx)
{
    if (!p->lapon || !la8 || strlen(p->hiscall) < 3 || strlen(p->hisgrid) < 4 || !a8_ltry) return;
    float f1 = (float)p->nfqso, xdt = 0, fbest = 0, xsnr = 0, plog = 0;
    char msg37[38];
    ft8_a8d(st.dd, p->mycall, p->hiscall, p->hisgrid, f1, &xdt, &fbest, &xsnr, &plog, msg37);
    if (msg37[0]) {
        float qual = plog < -147.0f ? 0.16f : 1.0f;
        if (cb) cb(ctx, 10.0f, nint(xsnr), xdt, fbest, msg37, 8, qual);
        ft8_a7_save(a7_jseq, xdt, f1, msg37);          /* f1 = nfqso, as the Fortran */
    }
}


static void ft8apset(const wsx_params_t* p, int* apsym)
{
    memset(apsym, 0, sizeof(int) * 58);
    apsym[0] = 99; apsym[29] = 99;
    if (strlen(p->mycall) < 3) return;
    const char* hc = strlen(p->hiscall) >= 3 ? p->hiscall : "KA1ABC";
    char msg[64];
    snprintf(msg, sizeof msg, "%s %s RRR", p->mycall, hc);
    ftx_message_t m;
    if (ftx_message_encode(&m, NULL, msg) != FTX_MESSAGE_RC_OK) return;
    if (ftx_message_get_i3(&m) != 1) return;
    char chk[64];
    ftx_message_offsets_t off;
    if (ftx_message_decode(&m, NULL, chk, &off) != FTX_MESSAGE_RC_OK || strcmp(chk, msg) != 0) return;
    for (int i = 0; i < 58; i++) apsym[i] = 2 * ((m.payload[i / 8] >> (7 - i % 8)) & 1) - 1;
    if (strlen(p->hiscall) < 3) apsym[29] = 99;
}

static void decode_call(const float* iwave, int nzhsym, const wsx_params_t* p,
                        wsx_callback_t cb, void* ctx)
{
    apctx_t ap;
    ap.p = p;
    g_hash = p->hash_if;
    a7_new_call(p->nutc, nzhsym);
    if (nzhsym == 41) a8_ltry = 1;                 /* decoder.f90: "FT8 defaults at start to enable a8" */
#ifdef A7_DEBUG
    if (nzhsym == 50) memcpy(a7_raw, iwave, sizeof a7_raw);
#endif
    ft8apset(p, ap.apsym);
    /* ft8apset's save_hash_call(hiscall), and pack77's pack28 saves of both
     * calls in "MYCALL HISCALL RRR" */
    if (strlen(p->hiscall) >= 3) save_hash_call(p->hiscall);
    if (strlen(p->mycall) >= 3) save_hash_call(p->mycall);
    if (nzhsym <= 47) {
        memcpy(st.dd, iwave, sizeof st.dd);
        memcpy(st.dd1, st.dd, sizeof st.dd);
    }
    if (nzhsym == 41) {
        st.ndecodes = 0;
        memset(st.allmessages, 0, sizeof st.allmessages);
    } else {
        st.ndecodes = st.ndec_early;
    }
    if (nzhsym == 47 && st.ndec_early == 0) {
        memcpy(st.dd1, st.dd, sizeof st.dd);
        goto l800;
    }
    if (nzhsym == 47 && st.ndec_early >= 1) {
        int lrefinedt = p->ndepth > 2;
        for (int i = 0; i < st.ndec_early; i++) {
            st.lsubtracted[i] = 0;
            if (st.xdt_save[i] - 0.5f < 0.396f) {
                subtractft8(st.dd, st.itone_save[i], st.f1_save[i], st.xdt_save[i], lrefinedt);
                st.lsubtracted[i] = 1;
            }
        }
        memcpy(st.dd1, st.dd, sizeof st.dd);
        return;                                       /* go to 900 */
    }
    if (nzhsym == 50 && st.ndec_early >= 1) {
        int n = 47 * 3456;
        memcpy(st.dd, st.dd1, sizeof(float) * n);
        memcpy(st.dd + n, iwave + n, sizeof(float) * (NMAX - n));
        for (int i = 0; i < st.ndec_early; i++) {
            if (st.lsubtracted[i]) continue;
            subtractft8(st.dd, st.itone_save[i], st.f1_save[i], st.xdt_save[i], 1);
        }
    }
    int ifa = p->nfa, ifb = p->nfb;
    int npass = p->ndepth == 1 ? 2 : 3;
    for (int ipass = 1; ipass <= npass; ipass++) {
        int newdat = 1;
        float syncmin = p->ndepth <= 2 ? 2.1f : 1.3f;
        int imetric = ipass == 1 ? 1 : 2;
        if (ipass == 3 && st.ndecodes == 0) continue;
        int ncand;
        PROF_BEGIN(0);
        sync8(st.dd, &ifa, &ifb, syncmin, p->nfqso, MAXCAND, st.candidate, &ncand, st.sbase);
        PROF_END(0);
        for (int icand = 0; icand < ncand; icand++) {
            float sync = st.candidate[icand][2];
            float f1 = st.candidate[icand][0];
            float xdt = st.candidate[icand][1];
            int si = nint(f1 / 3.125f);
            if (si < 0) si = 0;
            if (si > NH1) si = NH1;
            float xbase = powf(10.0f, 0.1f * (st.sbase[si] - 40.0f));
            char msg37[38] = {0};
            int iaptype = 0, nharderrors, itone[NN];
            float dmin = 0, xsnr = 0;
            int nbadcrc = ft8b(st.dd, &newdat, &ap, nzhsym, 1, imetric, &iaptype, &f1, &xdt,
                               xbase, &nharderrors, &dmin, msg37, &xsnr, itone);
            int nsnr = nint(xsnr);
            xdt = xdt - 0.5f;
            if (nbadcrc == 0) {
                int ldupe = 0;
                for (int id = 0; id < st.ndecodes; id++)
                    if (strcmp(msg37, st.allmessages[id]) == 0) ldupe = 1;
                if (!ldupe) {
                    if (st.ndecodes >= MAX_EARLY) continue;
                    int d = st.ndecodes++;
                    snprintf(st.allmessages[d], 38, "%s", msg37);
                    st.f1_save[d] = f1;
                    st.xdt_save[d] = xdt + 0.5f;
                    memcpy(st.itone_save[d], itone, sizeof itone);
                    float qual = 1.0f - (nharderrors + dmin) / 60.0f;
                    emit(cb, ctx, p, sync, nsnr, xdt, f1, msg37, iaptype, qual);
                    ft8_a7_save(a7_jseq, xdt, f1, msg37);
                }
            }
        }
    }
l800:
    st.ndec_early = 0;
    if (nzhsym < 50) st.ndec_early = st.ndecodes;
    if (nzhsym == 50) {                             /* label 900 */
        int la8 = a7_run(p, cb, ctx);
        a8_run(p, la8, cb, ctx);
    }
}

#endif /* !WSX_PIECE_ONLY */

void wsx_default_params(wsx_params_t* p)
{
    memset(p, 0, sizeof *p);
    p->nfa = 200; p->nfb = 4000;
    p->nfqso = 1500;
    p->nftx = 0;       /* jt9.f90 never sets nftx; only AP types >= 3 read it */
    p->ndepth = 3;
    p->lapon = 1;
    p->napwid = 75;
    p->nqso_progress = 0;
    snprintf(p->mycall, sizeof p->mycall, "K1ABC");
    snprintf(p->hiscall, sizeof p->hiscall, "W9XYZ");
    snprintf(p->hisgrid, sizeof p->hisgrid, "EN37");
}

#ifndef WSX_PIECE_ONLY   /* not on the piece path - see wsx_piece_begin */
/* One jt9 call: id2a = iwave(1:nzhsym*3456), zero after - jt9.f90's id2a. */
static void run_call(const int16_t* iwave, int nzhsym, const wsx_params_t* p,
                     wsx_callback_t cb, void* ctx)
{
    static float id2a[NMAX];
    int n = nzhsym * 3456;
    if (n > NMAX) n = NMAX;
    for (int i = 0; i < NMAX; i++) id2a[i] = i < n ? (float)iwave[i] : 0.f;
#ifdef WSX_PROF
    double t0 = prof_now();
    int d0 = st.ndecodes;
#endif
    decode_call(id2a, nzhsym, p, cb, ctx);
#ifdef WSX_PROF
    fprintf(stderr, "  call %d: %6.0f ms, ndecodes %d -> %d\n", nzhsym, 1e3 * (prof_now() - t0), d0, st.ndecodes);
#endif
}

/* The 41 call reads only iwave[0 .. 41*3456), so it can run while the rest of
 * the slot is still arriving - what WSJT-X does at 11.8 s into the slot. */
void wsx_decode_early(const int16_t* iwave, const wsx_params_t* p, wsx_callback_t cb, void* ctx)
{
    memset(&st, 0, sizeof st);
    run_call(iwave, 41, p, cb, ctx);
}

/* Calls 47 and 50, continuing the state wsx_decode_early() left. */
void wsx_decode_late(const int16_t* iwave, const wsx_params_t* p, wsx_callback_t cb, void* ctx)
{
    run_call(iwave, 47, p, cb, ctx);
    run_call(iwave, 50, p, cb, ctx);
}

void wsx_decode_slot(const int16_t* iwave, const wsx_params_t* p, wsx_callback_t cb, void* ctx)
{
    wsx_decode_early(iwave, p, cb, ctx);
    wsx_decode_late(iwave, p, cb, ctx);
}

#endif /* !WSX_PIECE_ONLY */

/* ---------------------------------------------------------------- pieces
 * For measuring single WSJT-X pieces on ANOTHER decoder's candidates: load a
 * slot once (dd, the 192000-point downsample FFT, the spectrum baseline), then
 * run ft8b's demodulator on one candidate at a time. No subtraction, no AP,
 * no early passes - just the per-candidate front end and decoder. */
static apctx_t g_piece_ap;
static wsx_params_t g_piece_p;
static int g_piece_newdat;

#ifdef WSX_PIECE_ONLY
#define PDD g_pdd
#define PDD_LEN NFFTD
#else
#define PDD st.dd
#define PDD_LEN NMAX
#endif

float* wsx_piece_audio(void)
{
#ifdef WSX_PIECE_ONLY
    if (!g_pdd) g_pdd = malloc(sizeof(float) * NFFTD);
    if (!g_pcx) g_pcx = malloc(sizeof(cf) * (NFFTD / 2 + 1));
    if (!g_pdd || !g_pcx) { wsx_piece_release(); return NULL; }
#endif
    return PDD;
}

static void piece_setup(int npasses, int maxosd)
{
    PROF_BEGIN(7);
    wsx_default_params(&g_piece_p);
    g_piece_p.lapon = 0;
    g_piece_ap.p = &g_piece_p;
    g_piece_npasses = npasses;
    g_piece_maxosd = maxosd;
    int ifa = g_piece_p.nfa, ifb = g_piece_p.nfb;
    get_spectrum_baseline(PDD, &ifa, &ifb, st.sbase);
    g_piece_newdat = 1;
    PROF_END(7);
}

void wsx_piece_begin(const int16_t* iwave, int npasses, int maxosd)
{
    float* dd = wsx_piece_audio();
    if (!dd) return;
    for (int i = 0; i < NMAX; i++) dd[i] = (float)iwave[i];
    for (int i = NMAX; i < PDD_LEN; i++) dd[i] = 0.0f;
    piece_setup(npasses, maxosd);
}

int wsx_piece_begin_audio(int n, int npasses, int maxosd)
{
    float* dd = wsx_piece_audio();
    if (!dd) return 0;
    if (n > NMAX) n = NMAX;
    /* Slot peak -> int16 full scale, then back to float: bit for bit what
     * the corpus WAVs (and so the parity proof) hold. */
    float fpeak = 0.0f;
    for (int i = 0; i < n; i++) { float a = fabsf(dd[i]); if (a > fpeak) fpeak = a; }
    const float g = (fpeak > 1e-6f) ? (32767.0f / fpeak) : 0.0f;
    for (int i = 0; i < n; i++) dd[i] = (float)(int16_t)lroundf(dd[i] * g);
    for (int i = n; i < PDD_LEN; i++) dd[i] = 0.0f;
    piece_setup(npasses, maxosd);
    return 1;
}

void wsx_piece_release(void)
{
#ifdef WSX_PIECE_ONLY
    free(g_pdd); g_pdd = NULL;
    free(g_pcx); g_pcx = NULL;
#endif
    for (int i = 0; i < (int)(sizeof plans / sizeof plans[0]); i++) {
        free(plans[i].cfg);              /* kiss_*_alloc(..., NULL, NULL) is one malloc */
        plans[i].cfg = NULL;
    }
}

int wsx_piece_try(float f1, float xdt, int imetric, char* msg, float* f_out, float* dt_out)
{
    return wsx_piece_try_ex(f1, xdt, imetric, msg, f_out, dt_out, NULL, NULL);
}

/* As wsx_piece_try, plus jt9's SNR (ft8b's xsnr, -25 floor) and the 79
 * channel tones. The tones let a caller test the decode against another
 * decoder's payloads exactly (ft8_encode -> compare), with no text or hash
 * involved. */
int wsx_piece_try_ex(float f1, float xdt, int imetric, char* msg, float* f_out, float* dt_out,
                     float* snr_out, int* itone_out)
{
    int si = nint(f1 / 3.125f);
    if (si < 1) si = 1;
    if (si > NH1) si = NH1;
    float xbase = powf(10.0f, 0.1f * (st.sbase[si] - 40.0f));
    int iaptype = 0, nh, itone[NN];
    float dmin = 0, xsnr = 0;
    char m37[38] = {0};
#ifdef WSX_PIECE_ONLY
    if (!g_pdd || !g_pcx) return 0;
#endif
    PROF_BEGIN(6);
    int bad = ft8b(PDD, &g_piece_newdat, &g_piece_ap, 50, 0, imetric, &iaptype, &f1, &xdt, xbase,
                   &nh, &dmin, m37, &xsnr, itone);
    PROF_END(6);
    if (bad) return 0;
    snprintf(msg, 38, "%s", m37);
    if (f_out) *f_out = f1;
    if (dt_out) *dt_out = xdt - 0.5f;
    if (snr_out) *snr_out = xsnr;
    if (itone_out) memcpy(itone_out, itone, sizeof(int) * NN);
    return 1;
}

void wsx_piece_end(void) { g_piece_npasses = 0; g_piece_maxosd = 2; g_piece_ndeep = 2; }
void wsx_piece_set_ndeep(int ndeep) { g_piece_ndeep = ndeep; }
/* AP type 1 only ("CQ ??? ???", ft8b passes 6-7): the MyCall/DxCall types
 * are disabled (apsym[0] = 99, exactly ft8b's "mycall unusable" mark). */
void wsx_piece_set_ap_cq(int on)
{
    g_piece_p.lapon = on ? 1 : 0;
    g_piece_p.nqso_progress = 0;
    g_piece_ap.apsym[0] = 99;
    g_piece_npasses = on ? 0 : 5;
}

#endif /* FT8_DECODER_WSJTX */
