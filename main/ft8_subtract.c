/* Subtract a decoded FT8 signal from the slot audio. See ft8_subtract.h for
 * what this is for, what it measured, and why it is not yet wired into
 * decode_slot().
 *
 * No allocation: the caller owns the scratch. This runs on the decode task,
 * where a failed malloc has to be handled rather than returned as a smaller
 * number of decodes, and ft8_test.c already carries enough allocation paths.
 */
#include "ft8_subtract.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* GFSK shaping, the same constants the device's own synthesiser uses
 * (ft8_test.c: FD_E2E_GFSK_BT, FD_E2E_GFSK_K), which is in turn ft8_lib's
 * gen_ft8.c demo. If these two ever disagree the subtraction stops matching
 * the signal it is trying to remove, and the symptom is a second pass that
 * quietly finds nothing. */
#define GFSK_BT 2.0f
#define GFSK_K  5.336446f    /* pi * sqrt(2 / log(2)) */

int ft8_subtract_scratch_floats(int n_sym)
{
    /* dphi (n_wave + 2 spsym) + pulse (3 spsym) + phase (n_wave) */
    const int nw = n_sym * FT8_SUB_SPSYM;
    return (nw + 2 * FT8_SUB_SPSYM) + (3 * FT8_SUB_SPSYM) + nw;
}

int ft8_subtract_start_sample(float time_sec, int rate_hz)
{
    const int spsym = (int)(0.5f + rate_hz * 0.160f);
    return (int)lrintf(time_sec * (float)rate_hz) - FT8_SUB_START_LEAD_SYM * spsym;
}

double ft8_subtract_signal(float *sig, int n_sig, const uint8_t *tones,
                           int n_sym, float f0_hz, int start, int rate_hz,
                           float *scratch)
{
    if (!sig || !tones || !scratch || n_sym <= 0 || n_sig <= 0) return 0.0;

    const int spsym = (int)(0.5f + rate_hz * 0.160f);
    const int n_wave = n_sym * spsym;
    const float dphi_peak = 2.0f * (float)M_PI / (float)spsym;   /* hmod = 1.0 */

    float *dphi  = scratch;
    float *pulse = dphi + (n_wave + 2 * spsym);
    float *ph    = pulse + (3 * spsym);

    for (int i = 0; i < n_wave + 2 * spsym; i++)
        dphi[i] = 2.0f * (float)M_PI * f0_hz / (float)rate_hz;

    for (int i = 0; i < 3 * spsym; i++) {
        float t = (float)i / (float)spsym - 1.5f;
        float a1 = GFSK_K * GFSK_BT * (t + 0.5f);
        float a2 = GFSK_K * GFSK_BT * (t - 0.5f);
        pulse[i] = (erff(a1) - erff(a2)) / 2.0f;
    }
    for (int i = 0; i < n_sym; i++) {
        const int ib = i * spsym;
        for (int j = 0; j < 3 * spsym; j++)
            dphi[j + ib] += dphi_peak * (float)tones[i] * pulse[j];
    }

    /* Integrate to phase, skipping the one-symbol run-in the pulse adds. */
    float phi = 0.0f;
    for (int k = 0; k < spsym; k++) phi += dphi[k];
    for (int k = 0; k < n_wave; k++) {
        ph[k] = phi;
        phi += dphi[k + spsym];
        if (phi > (float)M_PI) phi -= 2.0f * (float)M_PI;
    }

    /* Per-symbol-block least squares against sin/cos of that phase.
     * Two unknowns, so a 2x2 normal-equation solve per block. */
    double window_energy = 0.0, fitted_energy = 0.0;
    for (int b = 0; b < n_sym; b++) {
        const int off = b * spsym;
        double sii = 0, sqq = 0, siq = 0, ri = 0, rq = 0;
        for (int k = 0; k < spsym; k++) {
            const int idx = start + off + k;
            if (idx < 0 || idx >= n_sig) continue;
            const double si = sin(ph[off + k]), sq = cos(ph[off + k]);
            const double r = sig[idx];
            sii += si * si; sqq += sq * sq; siq += si * sq;
            ri  += r * si;  rq  += r * sq;
        }
        const double det = sii * sqq - siq * siq;
        if (fabs(det) < 1e-12) continue;          /* block entirely outside sig */
        const double a = (ri * sqq - rq * siq) / det;
        const double bb = (rq * sii - ri * siq) / det;
        for (int k = 0; k < spsym; k++) {
            const int idx = start + off + k;
            if (idx < 0 || idx >= n_sig) continue;
            const double m = a * sin(ph[off + k]) + bb * cos(ph[off + k]);
            window_energy += (double)sig[idx] * (double)sig[idx];
            fitted_energy += m * m;
            sig[idx] = (float)((double)sig[idx] - m);
        }
    }
    return window_energy > 0.0 ? fitted_energy / window_energy : 0.0;
}
