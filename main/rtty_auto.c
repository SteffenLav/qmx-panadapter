/* RTTY auto speed + shift (R028) - see rtty_auto.h. */
#include "rtty_auto.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef RTTY_AUTO_DEBUG
int rtty_auto_dbg_hist[2][48];
#endif
const float rtty_auto_shifts[RTTY_AUTO_NSHIFT] = { 170.0f, 200.0f, 425.0f, 450.0f, 850.0f };
const float rtty_auto_bauds[RTTY_AUTO_NBAUD]   = { 45.45f, 50.0f, 75.0f, 100.0f };

/* Speed verdict: the best score must reach this (noise scores ~0). */
#define AUTO_MIN_SCORE   0.20f   /* noise over 80 runs: sigma ~0.08; 100 Bd/170 at -3 dB scored 0.23-0.26 */
/* Shift verdict: the weaker tone over the median bin, dB. */
#define AUTO_PAIR_DB     6.0f
#define AUTO_SWITCH_DB   3.0f    /* a new shift must beat the held one by this */
#define AUTO_MIN_BLOCKS  6          /* ~1 s of spectrum (0.17 s blocks) */
#define AUTO_SPEC_KEEP   0.97f       /* per 0.17 s block: ~5.7 s. 0.9 (1.7 s) cost 422 -> 381 harness cases, 0.95 -> 401 */
#define AUTO_HYST        0.2f       /* decision hysteresis, fraction of total */

static void set_tones(rtty_auto_t *a)
{
    float fm = a->centre + a->pair_centre + a->shift / 2.0f;    /* once per pair: float is enough */
    float fs = a->centre + a->pair_centre - a->shift / 2.0f;
    a->m_c = 1; a->m_s = 0; a->m_sc = cosf(-2.0f * (float)M_PI * fm / a->fs); a->m_ss = sinf(-2.0f * (float)M_PI * fm / a->fs);
    a->s_c = 1; a->s_s = 0; a->s_sc = cosf(-2.0f * (float)M_PI * fs / a->fs); a->s_ss = sinf(-2.0f * (float)M_PI * fs / a->fs);
    for (int b = 0; b < RTTY_AUTO_NBAUD; b++) {
        float T = a->fs / rtty_auto_bauds[b];
        int W = (int)lroundf(T / 2.0f);
        memset(&a->det[b], 0, sizeof a->det[b]);
        a->det[b].W = W < 4 ? 4 : W > RTTY_AUTO_MAXWIN ? RTTY_AUTO_MAXWIN : W;
        a->det[b].T = T;
        a->det[b].deb = (uint32_t)lroundf(RTTY_AUTO_DEBOUNCE * T);
    }
    a->max_dt = (uint32_t)lroundf(12.0f * a->fs / rtty_auto_bauds[0]);
    a->ntrans = 0; a->baud_ok = false;
}

float rtty_auto_quality(uint32_t dchars, float dconf_sum)
{
    if (dchars < RTTY_AUTO_Q_MIN_CHARS) return 0;
    return dconf_sum / (float)dchars;
}

bool rtty_auto_trial_keep(float q_before, float q_after)
{
    if (q_after >= q_before + 0.08f) return true;              /* clearly better (gap >= 0.19 measured) */
    if (q_before < 0.20f && q_after > 0.50f) return true;     /* from nothing to a decode */
    return false;
}

void rtty_auto_init(rtty_auto_t *a, float fs, float centre_hz)
{
    memset(a, 0, sizeof *a);
    a->fs = fs;
    a->centre = centre_hz;
    a->D = (int)lroundf(fs / RTTY_AUTO_RATE_HZ);
    if (a->D < 1) a->D = 1;
    a->mc = 1; a->ms = 0;
    a->mstep_c = cosf(-2.0f * (float)M_PI * centre_hz / fs);
    a->mstep_s = sinf(-2.0f * (float)M_PI * centre_hz / fs);
}

static int cmpf(const void *x, const void *y)
{
    float p = *(const float *)x, q = *(const float *)y;
    return p < q ? -1 : p > q;
}

#include <stdlib.h>

/* Peak power within +/-1 bin of f (Hz rel. centre). */
static float peak_at(const rtty_auto_t *a, float f)
{
    int i = (int)lroundf(f / RTTY_AUTO_BIN_HZ) + (RTTY_AUTO_NBINS - 1) / 2;
    float m = 0;
    for (int k = i - 1; k <= i + 1; k++)
        if (k >= 0 && k < RTTY_AUTO_NBINS && a->pow[k] > m) m = a->pow[k];
    return m;
}

static void shift_verdict(rtty_auto_t *a)
{
    float *tmp = a->sorted;            /* not on the stack - see rtty_auto.h */
    memcpy(tmp, a->pow, sizeof a->sorted);
    qsort(tmp, RTTY_AUTO_NBINS, sizeof(float), cmpf);
    float med = tmp[RTTY_AUTO_NBINS / 2];
    if (med <= 0) return;
    float best = 0, bs = 0, bc = 0, held = 0, hc = a->pair_centre;
    for (int s = 0; s < RTTY_AUTO_NSHIFT; s++) {
        float h = rtty_auto_shifts[s] / 2.0f;
        for (float c = -RTTY_AUTO_CSEARCH_HZ; c <= RTTY_AUTO_CSEARCH_HZ; c += RTTY_AUTO_BIN_HZ) {
            float lo = peak_at(a, c - h), hi = peak_at(a, c + h);
            float w = lo < hi ? lo : hi;
            if (w > best) { best = w; bs = rtty_auto_shifts[s]; bc = c; }
            /* the held shift's own best, near where it was */
            if (a->shift_ok && rtty_auto_shifts[s] == a->shift && fabsf(c - a->pair_centre) <= 20.0f && w > held)
                { held = w; hc = c; }
        }
    }
    /* 170 over 200 unless 200 is AUTO_SWITCH_DB stronger. At a low
     * modulation index the two are hard to tell apart in the spectrum, and
     * the cost is real: R120RB (weak, 170) decoded with 200 copied "599" once
     * instead of three times (measured 2026-11-14). 170 is the amateur norm. */
    if (bs == 200.0f) {
        float b170 = 0, c170 = bc;
        for (float c = bc - 20.0f; c <= bc + 20.0f; c += RTTY_AUTO_BIN_HZ) {
            float lo = peak_at(a, c - 85.0f), hi = peak_at(a, c + 85.0f);
            float w = lo < hi ? lo : hi;
            if (w > b170) { b170 = w; c170 = c; }
        }
        if (b170 > 0 && 10.0f * log10f(best / b170) < AUTO_SWITCH_DB) { bs = 170.0f; bc = c170; }
    }
    float db = 10.0f * log10f(best / med);
    a->pair_db = db;
    if (db < AUTO_PAIR_DB) return;
    if (!a->shift_ok) {
        a->shift = bs; a->pair_centre = bc; a->shift_ok = true;
        set_tones(a);
        return;
    }
    /* Hysteresis: at a low modulation index (shift/baud < ~2.5) the CPFSK
     * spectrum's peaks are not at the tones, and 170/200 alternated at 75-100
     * Bd - each switch restarted the speed count (measured 2026-11-14). */
    if (bs != a->shift && held > 0 && 10.0f * log10f(best / held) < AUTO_SWITCH_DB) bs = a->shift, bc = hc;
    if (bs != a->shift || fabsf(bc - a->pair_centre) > 20.0f) {
        a->shift = bs; a->pair_centre = bc;
        set_tones(a);                  /* a different pair: speed starts over */
    }
}

/* In-place radix-2 FFT, n a power of two. */
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int u = i + k, v = i + k + len / 2;
                float xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
                re[v] = re[u] - xr; im[v] = im[u] - xi;
                re[u] += xr; im[u] += xi;
                float nc = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nc;
            }
        }
    }
}

static void block_done(rtty_auto_t *a)
{
    float *xr = a->fx, *xi = a->fy;    /* not on the stack - see rtty_auto.h */
    for (int i = 0; i < RTTY_AUTO_NFFT; i++) {
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / RTTY_AUTO_NFFT);
        xr[i] = a->br[i] * w; xi[i] = a->bi[i] * w;
    }
    fft(xr, xi, RTTY_AUTO_NFFT);
    for (int b = 0; b < RTTY_AUTO_NBINS; b++) {
        int k = b - (RTTY_AUTO_NBINS - 1) / 2;          /* -102..102 */
        int i = k < 0 ? k + RTTY_AUTO_NFFT : k;
        /* Decaying, not a sum: summed from start, minutes of noise before an
         * over made the pair take 30 s to reach 6 dB (bench2, 75 Bd test). */
        a->pow[b] = a->pow[b] * AUTO_SPEC_KEEP + xr[i] * xr[i] + xi[i] * xi[i];
    }
    a->blocks++;
    if (a->blocks >= AUTO_MIN_BLOCKS) shift_verdict(a);
}

static void speed_verdict(rtty_auto_t *a);

/* One polarity's verdict: q = whose runs are taken as space (0 low tone,
 * 1 high tone). Returns the speed index, or -1; *sc = its score. */
static int verdict_for(const rtty_auto_t *a, int q, float *sc)
{
    float s[RTTY_AUTO_NBAUD], best = -2;
    int bi = -1;
    for (int b = 0; b < RTTY_AUTO_NBAUD; b++) {
        const __typeof__(a->det[0]) *e = &a->det[b];
        s[b] = e->cnt[q] > 0 ? e->acc[q] / e->cnt[q] : 0;
        if (s[b] > best) { best = s[b]; bi = b; }
    }
    *sc = best;
    if (best < AUTO_MIN_SCORE) return -1;
    /* Lowest-wins ONLY for an exact multiple: a b Bd signal scores as high at
     * 2b. Applied to every candidate it picked 45.45 for a -6 dB 50 Bd
     * signal (0.31 vs 0.43) - those are not multiples (measured 2026-11-14). */
    for (int b = 0; b < bi; b++)
        if (fabsf(2.0f * rtty_auto_bauds[b] - rtty_auto_bauds[bi]) < 0.01f && s[b] >= RTTY_AUTO_NEAR * best)
            return b;
    return bi;
}

static void speed_verdict(rtty_auto_t *a)
{
    /* Each polarity gets its own verdict. ⛔ They can BOTH fit: at 50 Bd with
     * a 1.5-bit stop the mark runs (stop = 30 ms = 3 bits of 100 Bd) fit 100
     * as well as the space runs fit 50 (0.99 each, measured 2026-11-14), so
     * the higher score alone picked 100 half the time. When the two are close
     * the LOWER speed is right: no 100 Bd frame has mark runs that fit 50. */
    float s0, s1;
    int v0 = verdict_for(a, 0, &s0), v1 = verdict_for(a, 1, &s1);
    int p;
    if (v0 < 0 && v1 < 0) {            /* no verdict; scores still shown */
        p = s1 > s0 ? 1 : 0;
        for (int b = 0; b < RTTY_AUTO_NBAUD; b++) {
            const __typeof__(a->det[0]) *e = &a->det[b];
            a->score[b] = e->cnt[p] > 0 ? e->acc[p] / e->cnt[p] : 0;
        }
        a->baud_ok = false;
        return;
    }
    if (v0 < 0) p = 1;
    else if (v1 < 0) p = 0;
    else if (s0 >= RTTY_AUTO_NEAR * s1 && s1 >= RTTY_AUTO_NEAR * s0) p = v0 <= v1 ? 0 : 1;
    else p = s1 > s0 ? 1 : 0;
    int bi = p ? v1 : v0;
    for (int b = 0; b < RTTY_AUTO_NBAUD; b++) {
        const __typeof__(a->det[0]) *e = &a->det[b];
        a->score[b] = e->cnt[p] > 0 ? e->acc[p] / e->cnt[p] : 0;
        a->short_frac[b] = e->ntrans ? (float)e->nshort / e->ntrans : 1;
    }
    a->baud = rtty_auto_bauds[bi]; a->baud_ok = true;
    a->mark_low = (p == 1);            /* the high tone's runs are whole bits: it is space */
}

void rtty_auto_feed(rtty_auto_t *a, const float *re, const float *im, int n)
{
    for (int i = 0; i < n; i++) {
        float xr = re[i], xi = im ? im[i] : 0.0f;
        a->n++;

        /* spectrum path: mix to 0, boxcar-decimate, block */
        float yr = xr * a->mc - xi * a->ms, yi = xr * a->ms + xi * a->mc;
        float nc = a->mc * a->mstep_c - a->ms * a->mstep_s;
        a->ms = a->mc * a->mstep_s + a->ms * a->mstep_c; a->mc = nc;
        a->ar += yr; a->ai += yi;
        if (++a->k >= a->D) {
            a->br[a->nb] = a->ar / a->D; a->bi[a->nb] = a->ai / a->D;
            a->ar = a->ai = 0; a->k = 0;
            if (++a->nb >= RTTY_AUTO_NFFT) { a->nb = 0; block_done(a); }
        }
        if ((a->n & 1023u) == 0) {             /* keep the phasors on the circle */
            float g = 1.0f / sqrtf(a->mc * a->mc + a->ms * a->ms); a->mc *= g; a->ms *= g;
            g = 1.0f / sqrtf(a->m_c * a->m_c + a->m_s * a->m_s); a->m_c *= g; a->m_s *= g;
            g = 1.0f / sqrtf(a->s_c * a->s_c + a->s_s * a->s_s); a->s_c *= g; a->s_s *= g;
        }
        if (!a->shift_ok) continue;

        /* speed path: shared tone mixers, one boxcar pair per candidate */
        float mr = xr * a->m_c - xi * a->m_s, mi = xr * a->m_s + xi * a->m_c;
        float sr = xr * a->s_c - xi * a->s_s, si = xr * a->s_s + xi * a->s_c;
        nc = a->m_c * a->m_sc - a->m_s * a->m_ss; a->m_s = a->m_c * a->m_ss + a->m_s * a->m_sc; a->m_c = nc;
        nc = a->s_c * a->s_sc - a->s_s * a->s_ss; a->s_s = a->s_c * a->s_ss + a->s_s * a->s_sc; a->s_c = nc;
        bool any = false;
        for (int b = 0; b < RTTY_AUTO_NBAUD; b++) {
            __typeof__(a->det[0]) *e = &a->det[b];
            e->smr += mr - e->mr[e->pos]; e->smi += mi - e->mi[e->pos];
            e->ssr += sr - e->sr[e->pos]; e->ssi += si - e->si[e->pos];
            e->mr[e->pos] = mr; e->mi[e->pos] = mi; e->sr[e->pos] = sr; e->si[e->pos] = si;
            if (++e->pos >= e->W) e->pos = 0;
            if (e->filled < e->W) { e->filled++; continue; }
            float pm = e->smr * e->smr + e->smi * e->smi;
            float ps = e->ssr * e->ssr + e->ssi * e->ssi;
            float d = pm - ps, tot = pm + ps;
            int st = e->state;
            if (d > AUTO_HYST * tot) st = 1;
            else if (d < -AUTO_HYST * tot) st = -1;
            if (st == e->state) { e->cand = st; continue; }     /* a glitch ended */
            /* debounce: see rtty_auto.h */
            if (st != e->cand) { e->cand = st; e->t_cand = a->n; continue; }
            if (e->state != 0 && a->n - e->t_cand < e->deb) continue;
            /* Both tones' runs are scored; the whole-bit one is space - see
             * rtty_auto.h. state +1 = the HIGH tone. */
            if (e->state != 0 && e->have_last) {
                uint32_t du = e->t_cand - e->t_last;
                float dt = (float)du, T = e->T;
                if (dt >= 0.25f * T && du < a->max_dt) {
                    int p = e->state == 1 ? 1 : 0;     /* the run that just ended */
                    if (b == RTTY_AUTO_HIST_DET) {
                        int ms = (int)(dt * 1000.0f / a->fs + 0.5f);
                        if (ms < RTTY_AUTO_HIST_N && a->hist[p][ms] < 65535) a->hist[p][ms]++;
                    }
#ifdef RTTY_AUTO_DEBUG
                    if (b == 0) { int hb = (int)(dt / T * 4.0f + 0.5f); if (hb < 48) rtty_auto_dbg_hist[p][hb]++; }
#endif
                    const float k = 1.0f - 1.0f / RTTY_AUTO_KEEP;
                    e->acc[p] = e->acc[p] * k + cosf(2.0f * (float)M_PI * dt / T);
                    e->cnt[p] = e->cnt[p] * k + 1.0f;
                    if (dt < 0.75f * T) e->nshort++;
                    e->ntrans++;
                    any = true;
                }
            }
            e->t_last = e->t_cand; e->have_last = true;
            e->state = st;
        }
        a->ntrans = a->det[0].ntrans;
        if (any && a->ntrans >= RTTY_AUTO_MIN_TRANS) speed_verdict(a);
    }
}
