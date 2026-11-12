/* RTTY FSK demodulator - see rtty_demod.h for the method. T196. */
#include "rtty_demod.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef RTTY_DEBUG
#include <stdio.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void set_step(float f_hz, float fs, float *c, float *s)
{
    double w = 2.0 * M_PI * (double)f_hz / (double)fs;
    *c = (float)cos(w);
    *s = (float)sin(w);
}

#ifndef AFC_PRESENT   /* harness override for measuring the gate */
#define AFC_PRESENT(d) ((d)->sig_lvl >= RTTY_AFC_MIN_LVL)
#endif

/* Mark and space steps from centre + AFC offset. */
static void retune(rtty_demod_t *d)
{
    float half = d->cfg.shift_hz * 0.5f;
    float c = d->cfg.centre_hz + d->afc_off;
    float fm  = c + (d->cfg.reverse ? -half : half);
    float fsp = c + (d->cfg.reverse ? half : -half);
    set_step(fm,  d->cfg.fs_hz, &d->mstep_c, &d->mstep_s);
    set_step(fsp, d->cfg.fs_hz, &d->sstep_c, &d->sstep_s);
}

/* Once per bit: apply the discriminator. Frozen while unlocked. */
static void afc_update(rtty_demod_t *d)
{
    float r = d->afc_r, i = d->afc_i;
    d->afc_r = d->afc_i = 0.0f;
    if (d->cfg.afc_hz <= 0.0f || !d->g_lock || !d->afc_ok || !AFC_PRESENT(d) || (r == 0.0f && i == 0.0f)) return;
    float e = atan2f(i, r) * d->cfg.fs_hz / (float)(2.0 * M_PI);
    float o = d->afc_off + RTTY_AFC_MU * e;
    if (o >  d->cfg.afc_hz) o =  d->cfg.afc_hz;
    if (o < -d->cfg.afc_hz) o = -d->cfg.afc_hz;
    d->afc_off = o;
    retune(d);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* One 0.5 s block is in acq_br/bi: DFT it, smooth, and jump AFC to the best
 * tone pair if no signal is being decoded. */
static void acq_block(rtty_demod_t *d)
{
    const float rate = d->cfg.fs_hz / (float)d->acq_D;
    for (int b = 0; b < d->acq_nb; b++) {
        float c, s, pc = 1.0f, ps = 0.0f, xr = 0.0f, xi = 0.0f;
        set_step(d->acq_f0 + RTTY_ACQ_STEP_HZ * (float)b, rate, &c, &s);
        for (int n = 0; n < d->acq_N; n++) {
            /* x * conj(p) */
            xr += d->acq_br[n] * pc + d->acq_bi[n] * ps;
            xi += d->acq_bi[n] * pc - d->acq_br[n] * ps;
            float t = pc * c - ps * s; ps = pc * s + ps * c; pc = t;
        }
        float p = xr * xr + xi * xi;
        d->acq_pow[b] = d->acq_blocks ? 0.5f * d->acq_pow[b] + 0.5f * p : p;
    }
    d->acq_blocks++;
    if (d->acq_blocks < 2 || d->cfg.afc_hz <= 0.0f || AFC_PRESENT(d)) return;

    float sorted[RTTY_ACQ_BINS];
    memcpy(sorted, d->acq_pow, (size_t)d->acq_nb * sizeof(float));
    qsort(sorted, (size_t)d->acq_nb, sizeof(float), cmp_float);
    float thr = RTTY_ACQ_SNR * sorted[d->acq_nb / 2];

    int h = (int)lroundf(d->cfg.shift_hz * 0.5f / RTTY_ACQ_STEP_HZ);
    float best = 0.0f, best_off = 0.0f;
    for (int b = h; b < d->acq_nb - h; b++) {
        float off = d->acq_f0 + RTTY_ACQ_STEP_HZ * (float)b;
        if (fabsf(off) > d->cfg.afc_hz) continue;
        float lo = d->acq_pow[b - h], hi = d->acq_pow[b + h];
        float m = lo < hi ? lo : hi;
        if (m > thr && m > best) { best = m; best_off = off; }
    }
    if (best > 0.0f && fabsf(best_off - d->afc_off) >= RTTY_ACQ_MIN_MOVE) {
        d->afc_off = best_off;
        retune(d);
        d->n_acq++;
    }
}

static void acq_feed(rtty_demod_t *d, float xr, float xi)
{
    float ar = xr * d->acq_c + xi * d->acq_s, ai = xi * d->acq_c - xr * d->acq_s;
    float t = d->acq_c * d->acq_step_c - d->acq_s * d->acq_step_s;
    d->acq_s = d->acq_c * d->acq_step_s + d->acq_s * d->acq_step_c; d->acq_c = t;
    d->acq_ar += ar; d->acq_ai += ai;
    if (++d->acq_k < d->acq_D) return;
    d->acq_k = 0;
    float g = 1.0f / sqrtf(d->acq_c * d->acq_c + d->acq_s * d->acq_s);
    d->acq_c *= g; d->acq_s *= g;
    d->acq_br[d->acq_n] = d->acq_ar; d->acq_bi[d->acq_n] = d->acq_ai;
    d->acq_ar = d->acq_ai = 0.0f;
    if (++d->acq_n >= d->acq_N) { d->acq_n = 0; acq_block(d); }
}

bool rtty_demod_init(rtty_demod_t *d, const rtty_demod_cfg_t *cfg)
{
    memset(d, 0, sizeof(*d));
    if (!cfg || cfg->fs_hz <= 0 || cfg->baud <= 0) return false;
    d->cfg = *cfg;
    d->T = cfg->fs_hz / cfg->baud;
    d->L = (int)lroundf(d->T);
    if (d->L < 4 || d->L > RTTY_MAX_BIT_SAMPLES) return false;

    retune(d);
    d->mc = 1.0f; d->ms = 0.0f;
    d->sc = 1.0f; d->ss = 0.0f;
    d->g_bits = 7.5f;
    d->g_P = 7.5 * d->T;
    d->cfg_alpha = RTTY_GRID_ALPHA;
    d->cfg_beta  = RTTY_GRID_BETA;

    d->acq_D = (int)lroundf(cfg->fs_hz / RTTY_ACQ_RATE_HZ);
    if (d->acq_D < 1) d->acq_D = 1;
    d->acq_N = (int)(cfg->fs_hz / (float)d->acq_D * 0.5f);
    if (d->acq_N > RTTY_ACQ_N) d->acq_N = RTTY_ACQ_N;
    float span = cfg->afc_hz + cfg->shift_hz * 0.5f;
    d->acq_nb = 2 * (int)ceilf(span / RTTY_ACQ_STEP_HZ) + 1;
    if (d->acq_nb > RTTY_ACQ_BINS - 1) d->acq_nb = RTTY_ACQ_BINS - 1;   /* odd */
    d->acq_f0 = -RTTY_ACQ_STEP_HZ * (float)(d->acq_nb / 2);
    set_step(cfg->centre_hz, cfg->fs_hz, &d->acq_step_c, &d->acq_step_s);
    d->acq_c = 1.0f;
    return true;
}

/* Re-sum the rings exactly and renormalise the phasors. Called once per bit
 * length, so float rounding in the running sums cannot accumulate. */
static void refresh(rtty_demod_t *d)
{
    float a = 0, b = 0, c = 0, e = 0;
    for (int i = 0; i < d->L; i++) { a += d->mr[i]; b += d->mi[i]; c += d->sr[i]; e += d->si[i]; }
    d->smr = a; d->smi = b; d->ssr = c; d->ssi = e;
    float g = 1.0f / sqrtf(d->mc * d->mc + d->ms * d->ms);
    d->mc *= g; d->ms *= g;
    g = 1.0f / sqrtf(d->sc * d->sc + d->ss * d->ss);
    d->sc *= g; d->ss *= g;
}

/* d at absolute (fractional) time t, linear interpolation. Caller keeps t
 * inside the last RTTY_HIST-1 samples. */
static float hist_at(const rtty_demod_t *d, double t)
{
    uint32_t i = (uint32_t)t;
    float f = (float)(t - (double)i);
    float a = d->hist[i % RTTY_HIST], b = d->hist[(i + 1) % RTTY_HIST];
    return a + f * (b - a);
}

/* Best start sample point in [c - w, c + w], in T/20 steps: start must read
 * space and stop mark; among those, the largest sum |d| over the 7 sample
 * points. Returns false when no time qualifies. */
static bool search(const rtty_demod_t *d, double c, double w, double Tb,
                   double *best_t, float *best_s)
{
    bool found = false;
    int K = (int)ceil(w / (Tb / 20.0));
    for (int k = -K; k <= K; k++) {
        double t0 = c + (double)k * Tb / 20.0;
        float vs = hist_at(d, t0), ve = hist_at(d, t0 + 6.0 * Tb);
        if (vs >= 0.0f || ve <= 0.0f) continue;
        float s = -vs + ve;
        for (int b = 1; b <= 5; b++) s += fabsf(hist_at(d, t0 + (double)b * Tb));
        if (!found || s > *best_s) { *best_s = s; *best_t = t0; found = true; }
    }
    return found;
}

static void emit(rtty_demod_t *d, double t0, double Tb, float score,
                 rtty_char_cb cb, void *user)
{
    uint8_t code = 0;
    for (int b = 1; b <= 5; b++)
        if (hist_at(d, t0 + (double)b * Tb) > 0.0f) code |= (uint8_t)(1u << (b - 1));
    rtty_char_t ch = { code, score / 7.0f, t0 + (double)d->L - d->T };
    d->afc_ok = ch.conf >= RTTY_AFC_MIN_CONF;

    /* Signal presence: conf smoothed over ~5 characters, with hysteresis.
     * Always computed - AFC uses it even when the squelch is off. */
    d->sig_lvl += RTTY_SQ_ALPHA * (ch.conf - d->sig_lvl);
    if (!d->present && d->sig_lvl >= RTTY_SQ_OPEN) d->present = true;
    else if (d->present && d->sig_lvl < RTTY_SQ_OPEN - RTTY_SQ_HYST) d->present = false;

    if (d->cfg.squelch <= 0.0f) { d->n_chars++; if (cb) cb(&ch, user); return; }

    /* Fast close: RTTY_SQ_LOW_RUN characters in a row below RTTY_SQ_LOW.
     * The smoothed level alone left 6-11 noise characters after every
     * transmission (once 124, measured 2026-11-12). Per character, noise is
     * below 0.5 96 % of the time, a -6 dB signal 1 %. */
    d->low_run = ch.conf < RTTY_SQ_LOW ? d->low_run + 1 : 0;
    if (!d->sq_open && d->sig_lvl >= d->cfg.squelch && d->low_run == 0) d->sq_open = true;
    else if (d->sq_open && (d->sig_lvl < d->cfg.squelch - RTTY_SQ_HYST ||
                            d->low_run >= RTTY_SQ_LOW_RUN)) {
        d->sq_open = false;
        d->sig_lvl = d->cfg.squelch - RTTY_SQ_HYST;   /* re-open needs fresh evidence */
    }
    if (!d->sq_open) {
        /* Hold the last few: they are the characters the average was still
         * rising through, and they are printed if the gate opens next. */
        if (d->n_pend == RTTY_SQ_PENDING) {
            memmove(&d->pend[0], &d->pend[1], (RTTY_SQ_PENDING - 1) * sizeof(d->pend[0]));
            d->n_pend--;
            d->n_squelched++;
        }
        d->pend[d->n_pend++] = ch;
        return;
    }
    for (int i = 0; i < d->n_pend; i++) { d->n_chars++; if (cb) cb(&d->pend[i], user); }
    d->n_pend = 0;
    d->n_chars++;
    if (cb) cb(&ch, user);
}

static void steer(rtty_demod_t *d, double err)
{
    d->g_P += d->cfg_beta * err;
    double nom = d->g_bits * d->T;
    if (d->g_P < nom * (1.0 - RTTY_GRID_MAX_RATE)) d->g_P = nom * (1.0 - RTTY_GRID_MAX_RATE);
    if (d->g_P > nom * (1.0 + RTTY_GRID_MAX_RATE)) d->g_P = nom * (1.0 + RTTY_GRID_MAX_RATE);
}

/* UNLOCKED: a crossing candidate is due. Search +/-0.4 bit around it. Lock
 * the character clock when this start follows the previous one by one
 * period (+/-0.3 bit). */
static void eval_candidate(rtty_demod_t *d, rtty_char_cb cb, void *user)
{
    const double T = d->T;
    double bt; float bs;
    if (!search(d, d->t_cross + 0.5 * T, 0.4 * T, T, &bt, &bs)) {
        /* Count by what the nominal timing saw, so the counters still say
         * which end failed. */
        if (hist_at(d, d->t_cross + 0.5 * T) >= 0.0f) d->n_false_start++; else d->n_framing++;
        return;
    }
    if (d->g_valid) {
        double dt = bt - d->g_ref;
        double err = dt - d->g_P;
        if (fabs(err) < 0.3 * T) {
            d->g_lock = true;
            d->g_miss = 0;
            d->g_votes = 0;
            steer(d, err);
        } else if (dt > 6.75 * T && dt < 8.25 * T) {
            /* Back to back but off this period: the sender may use another
             * stop length. Switch after 3 votes in a row for the same one. */
            float v = (float)(floor(dt / (0.5 * T) + 0.5) * 0.5);
            if (v != d->g_bits) {
                if (v == d->g_vote) d->g_votes++; else { d->g_vote = v; d->g_votes = 1; }
                if (d->g_votes >= 3) { d->g_bits = v; d->g_P = v * T; d->g_votes = 0; }
            }
        }
    }
    d->g_ref = bt;
    d->g_valid = true;
    emit(d, bt, T, bs, cb, user);
}

/* LOCKED: the next slot is due one period after the last. Search +/-0.3 bit
 * around the prediction, decide at the prediction pulled alpha of the way to
 * the found time, steer the period. A slot with no character in it (idle
 * mark, or lost in noise) advances the clock; RTTY_GRID_MAX_MISS of them in a
 * row drop the lock and the crossing hunter takes over again. */
static void eval_slot(rtty_demod_t *d, rtty_char_cb cb, void *user)
{
    double Tg = d->g_P / d->g_bits;
    double pred = d->g_ref + d->g_P;
    double bt; float bs;
    if (search(d, pred, 0.3 * Tg, Tg, &bt, &bs)) {
        double err = bt - pred;
        double t = pred + d->cfg_alpha * err;
        if (!(hist_at(d, t) < 0.0f && hist_at(d, t + 6.0 * Tg) > 0.0f)) t = bt;
        steer(d, err);
        d->g_ref = t;
        d->g_miss = 0;
        d->n_grid++;
#ifdef RTTY_DEBUG
        printf("SLOT err=%+.3f P=%.4f\n", err / d->T, d->g_P / d->T);
#endif
        emit(d, t, Tg, bs, cb, user);
        return;
    }
    d->g_ref = pred;
    d->afc_ok = false;
    if (++d->g_miss >= RTTY_GRID_MAX_MISS) {
        d->g_lock = false;
        d->in_char = false;
        d->n_unlock++;
    }
}

static void framer(rtty_demod_t *d, float dv, rtty_char_cb cb, void *user)
{
    float prev = d->prev_d;
    d->prev_d = dv;
    d->hist[d->ns % RTTY_HIST] = dv;
    uint32_t now = d->ns++;

    if (d->g_lock) {
        /* Latest sample the slot search reads, plus one for interpolation. */
        double Tg = d->g_P / d->g_bits;
        if ((double)now >= d->g_ref + d->g_P + 6.3 * Tg + 1.0) eval_slot(d, cb, user);
        return;
    }
    if (!d->in_char) {
        if (prev > 0.0f && dv <= 0.0f && now > 0) {
            float frac = prev / (prev - dv);   /* 0..1 from the previous sample */
            d->t_cross = (double)(now - 1) + frac;
            d->in_char = true;
        }
        return;
    }
    /* Latest sample the search reads: t_cross + 0.5T + 0.4T + 6T, plus one
     * for interpolation. Evaluate once it exists, then hunt again - the next
     * start's crossing is >= 7 bits after this one (1 stop bit) and normally
     * 7.5 (1.5 stop bits). */
    if ((double)now >= d->t_cross + 6.9 * d->T + 1.0) {
        d->in_char = false;
        eval_candidate(d, cb, user);
    }
}

void rtty_demod_feed(rtty_demod_t *d, const float *re, const float *im, int n,
                     rtty_char_cb cb, void *user)
{
    const float L = (float)d->L;
    for (int k = 0; k < n; k++) {
        float xr = re[k], xi = im ? im[k] : 0.0f;
        if (d->cfg.afc_hz > 0.0f) acq_feed(d, xr, xi);

        /* x * conj(phasor): mixes the tone at the phasor frequency to DC */
        float a_r = xr * d->mc + xi * d->ms, a_i = xi * d->mc - xr * d->ms;
        float b_r = xr * d->sc + xi * d->ss, b_i = xi * d->sc - xr * d->ss;

        /* advance phasors */
        float t;
        t = d->mc * d->mstep_c - d->ms * d->mstep_s; d->ms = d->mc * d->mstep_s + d->ms * d->mstep_c; d->mc = t;
        t = d->sc * d->sstep_c - d->ss * d->sstep_s; d->ss = d->sc * d->sstep_s + d->ss * d->sstep_c; d->sc = t;

        int p = d->pos;
        d->smr += a_r - d->mr[p]; d->smi += a_i - d->mi[p];
        d->ssr += b_r - d->sr[p]; d->ssi += b_i - d->si[p];
        d->mr[p] = a_r; d->mi[p] = a_i; d->sr[p] = b_r; d->si[p] = b_i;
        float zmr = d->smr, zmi = d->smi, zsr = d->ssr, zsi = d->ssi;
        if (++d->pos >= d->L) { d->pos = 0; refresh(d); afc_update(d); }
        if (d->filled < d->L) { d->filled++; d->pmr = zmr; d->pmi = zmi; d->psr = zsr; d->psi = zsi; continue; }

        float am = sqrtf(zmr * zmr + zmi * zmi) / L;
        float as = sqrtf(zsr * zsr + zsi * zsi) / L;
        d->mark_lvl  += 0.002f * (am - d->mark_lvl);
        d->space_lvl += 0.002f * (as - d->space_lvl);
        float dv = (am - as) / (am + as + 1e-20f);
        /* AFC discriminator: z[n] * conj(z[n-1]) of the tone that is on */
        if (dv > RTTY_AFC_GATE) {
            d->afc_r += zmr * d->pmr + zmi * d->pmi;
            d->afc_i += zmi * d->pmr - zmr * d->pmi;
        } else if (dv < -RTTY_AFC_GATE) {
            d->afc_r += zsr * d->psr + zsi * d->psi;
            d->afc_i += zsi * d->psr - zsr * d->psi;
        }
        d->pmr = zmr; d->pmi = zmi; d->psr = zsr; d->psi = zsi;
        framer(d, dv, cb, user);
    }
}
