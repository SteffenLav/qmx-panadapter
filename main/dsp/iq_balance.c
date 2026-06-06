#include "iq_balance.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>

// ---- Tuning ------------------------------------------------------------
// Steady-state time constants (alpha = 1 / (tau * fs), fs = 48 kHz):
//   tau_dc  = 1.0 s  -> alpha_dc  = 2.08e-5
//   tau_pwr = 0.2 s  -> alpha_pwr = 1.04e-4
//   tau_xy  = 1.0 s  -> alpha_xy  = 2.08e-5  (Phase C: 2s->1s for faster
//                                             steady-state phase tracking)
//
// Phase C two-speed startup: for FAST_RAMP_SAMPLES samples of active
// signal after each reset, all alphas are multiplied by ALPHA_FAST_MULT.
// This drives initial convergence in ~tau/8 before settling to slow
// tracking.  FAST_RAMP_SAMPLES = 2 s worth so the burst outlasts any
// band-change transient but is short enough to not bias on a single tone.
#define ALPHA_DC          (2.08e-5f)
#define ALPHA_PWR         (1.04e-4f)
#define ALPHA_XY          (2.08e-5f)

#define FAST_RAMP_SAMPLES  96000u      // 2 s @ 48 kHz
#define ALPHA_FAST_MULT    8.0f

// Freeze updates when total power is below this threshold (avoid
// integrating pure quantisation noise on dummy load).
#define POWER_FREEZE_THRESHOLD  (4.0f)   // ~ |sample| < 2 LSB RMS

// ---- State -------------------------------------------------------------
static bool     s_enabled       = false;
static uint32_t s_startup_count = 0;    // samples with signal seen since reset
static float    s_dc_i          = 0.0f;
static float    s_dc_q          = 0.0f;
static float    s_p_i           = 1.0f; // mean square of I (>0 to avoid div-by-zero)
static float    s_p_q           = 1.0f; // mean square of Q
static float    s_xy            = 0.0f; // mean of i*q

// Derived correction coefficients, updated once per sample.
//   q_out = (q - K_phi * i) * K_amp
// Initial values are pass-through.
static float    s_K_phi         = 0.0f;
static float    s_K_amp         = 1.0f;

// ---- Public API --------------------------------------------------------
void iq_balance_reset(void)
{
    s_startup_count = 0;
    s_dc_i  = 0.0f;
    s_dc_q  = 0.0f;
    s_p_i   = 1.0f;
    s_p_q   = 1.0f;
    s_xy    = 0.0f;
    s_K_phi = 0.0f;
    s_K_amp = 1.0f;
}

void iq_balance_init(bool enabled)
{
    s_enabled = enabled;
    iq_balance_reset();
}
void iq_balance_set_enabled(bool on)
{
    s_enabled = on;
}

bool iq_balance_is_enabled(void)
{
    return s_enabled;
}

void iq_balance_apply(int16_t *i_inout, int16_t *q_inout)
{
    if (!s_enabled) return;

    // 1. Promote to float and remove running DC.
    float i = (float)(*i_inout) - s_dc_i;
    float q = (float)(*q_inout) - s_dc_q;

    // 2. Apply current correction coefficients (computed last sample).
    //    Classical Gram-Schmidt I/Q correction:
    //      i_out = i
    //      q_out = (q - sin(phi) * i / sqrt(p_i)) * sqrt(p_i / p_q) / cos(phi)
    //    We fold sqrt(p_i) into K_phi and sqrt(p_i/p_q)/cos(phi) into K_amp
    //    so the per-sample work is just one MAC plus one multiply.
    float i_out = i;
    float q_out = (q - s_K_phi * i) * s_K_amp;

    // 3. Update running estimates (only when there is some signal energy).
    float total_p = i * i + q * q;
    if (total_p > POWER_FREEZE_THRESHOLD) {
        // Two-speed: 8× faster alpha for first 2 s of real signal after reset.
        float fast = (s_startup_count < FAST_RAMP_SAMPLES) ? ALPHA_FAST_MULT : 1.0f;
        if (s_startup_count < FAST_RAMP_SAMPLES) s_startup_count++;

        // DC trackers (against the raw, pre-correction sample).
        s_dc_i += (ALPHA_DC  * fast) * ((float)(*i_inout) - s_dc_i);
        s_dc_q += (ALPHA_DC  * fast) * ((float)(*q_inout) - s_dc_q);

        // Power and cross-product (after DC removal, pre-correction).
        s_p_i  += (ALPHA_PWR * fast) * (i * i - s_p_i);
        s_p_q  += (ALPHA_PWR * fast) * (q * q - s_p_q);
        s_xy   += (ALPHA_XY  * fast) * (i * q - s_xy);

        // Recompute correction coefficients.
        //   sin_phi = xy / sqrt(p_i * p_q)
        //   cos_phi = sqrt(1 - sin_phi^2)
        //   K_phi   = sin_phi * sqrt(p_q / p_i)
        //   K_amp   = sqrt(p_i / p_q) / cos_phi
        float denom = sqrtf(s_p_i * s_p_q);
        if (denom > 1e-6f) {
            float sin_phi = s_xy / denom;
            // Clamp to keep cos_phi real.
            if (sin_phi >  0.5f) sin_phi =  0.5f;
            if (sin_phi < -0.5f) sin_phi = -0.5f;
            float cos_phi = sqrtf(1.0f - sin_phi * sin_phi);

            float ratio_qi = sqrtf(s_p_q / s_p_i);
            float ratio_iq = 1.0f / ratio_qi;

            s_K_phi = sin_phi * ratio_qi;
            s_K_amp = ratio_iq / cos_phi;
        }
    }

    // 4. Clamp back to int16.
    if (i_out >  32767.0f) i_out =  32767.0f;
    if (i_out < -32768.0f) i_out = -32768.0f;
    if (q_out >  32767.0f) q_out =  32767.0f;
    if (q_out < -32768.0f) q_out = -32768.0f;

    *i_inout = (int16_t)i_out;
    *q_inout = (int16_t)q_out;
}
