/* WSPR simulation mode. See wspr_sim.h for the design and why it synthesizes
 * audio instead of injecting finished spots. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "storage/settings.h"
#include "wspr_proto.h"
#include "wspr_fano.h"
#include "wspr_decode.h"
#include "wspr_sim.h"

static const char *TAG = "wspr_sim";

#define TONE_SPACING_HZ (WSPR_SAMPLE_RATE_HZ / (double)WSPR_SYM_LEN_SAMPLES)

bool wspr_sim_enabled(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    return s.sim_mode_en;
}

/* FLOAT throughout, not double: the P4's FPU is single-precision only, so a
 * double sin() is a software library call. The self-test's first version spent
 * 80 SECONDS synthesizing one window in double and 2.8 s in float. */
static uint32_t s_rng = 0x5EED1234u;
static float rng_gaussian(void)
{
    uint32_t s = s_rng;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    float u1 = (s % 1000000 + 1) / 1000001.0f;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    float u2 = (s % 1000000 + 1) / 1000001.0f;
    s_rng = s;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

static inline void add_sample(int16_t *pcm, long i, float v)
{
    float x = pcm[i] + v;
    if (x >  32767.0f) x =  32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    pcm[i] = (int16_t)x;
}

void wspr_sim_noise(int16_t *pcm, long n, float sigma)
{
    for (long i = 0; i < n; i++) {
        float v = sigma * rng_gaussian() * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
}

bool wspr_sim_add_station(int16_t *pcm, long n, const char *call, const char *grid,
                          int dbm, double f0_hz, double start_s, float amplitude)
{
    wspr_msg_bytes_t msg;
    if (!wspr_pack_message(call, grid, dbm, &msg)) {
        ESP_LOGW(TAG, "'%s' '%s' %d dBm will not pack - skipped", call, grid, dbm);
        return false;
    }
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);

    long start = (long)(start_s * WSPR_SAMPLE_RATE_HZ);
    float phase = 0.0f;
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        float freq = (float)f0_hz + tones[sym] * (float)TONE_SPACING_HZ;
        float dphi = 2.0f * (float)M_PI * freq / (float)WSPR_SAMPLE_RATE_HZ;
        for (int k = 0; k < WSPR_SYM_LEN_SAMPLES; k++) {
            long idx = start + (long)sym * WSPR_SYM_LEN_SAMPLES + k;
            if (idx >= 0 && idx < n) add_sample(pcm, idx, amplitude * sinf(phase) * 32767.0f);
            phase += dphi;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
    }
    return true;
}

void wspr_sim_add_burst(int16_t *pcm, long n, double t_s, double dur_s, float amplitude)
{
    long a = (long)(t_s * WSPR_SAMPLE_RATE_HZ);
    long b = a + (long)(dur_s * WSPR_SAMPLE_RATE_HZ);
    if (a < 0) a = 0;
    if (b > n) b = n;
    /* White noise, i.e. flat across the whole band - which is what makes it
     * read as a HORIZONTAL streak on the waterfall (broadband at one instant)
     * rather than the vertical trace a WSPR signal draws. */
    for (long i = a; i < b; i++)
        add_sample(pcm, i, amplitude * rng_gaussian() * 32767.0f);
}

/* The phantom population.
 *
 * Spread around the globe on purpose: distance and bearing are computed from
 * the operator's own grid, so a table clustered in one country would leave the
 * KM and BRG columns untested and the list looking nothing like a real band.
 *
 * `duty` is one-in-N cycles, which is how WSPR is actually operated - most
 * stations transmit in a fraction of slots. `amp` spans loud to marginal so the
 * display has to cope with both, and so the waterfall's contrast can be judged
 * against a signal whose true strength is KNOWN.
 */
static const struct {
    const char *call, *grid;
    int    dbm;
    double f0;        /* audio Hz within the window */
    float  amp;       /* linear, before per-cycle fading */
    int    duty;      /* transmits one cycle in `duty` */
} s_phantoms[] = {
    { "VK7SIM", "QE38", 37, 1502.0, 0.045f, 2 },  /* Tasmania, ~16,500 km */
    { "W1SIM",  "FN42", 30, 1447.0, 0.030f, 2 },  /* US east coast */
    { "JA1SIM", "PM95", 33, 1561.0, 0.022f, 3 },  /* Japan */
    { "G4SIM",  "IO91", 23, 1478.0, 0.055f, 2 },  /* England, close and loud */
    { "DL0SIM", "JO62", 27, 1523.0, 0.060f, 3 },  /* Germany, closest */
    { "ZS6SIM", "KG44", 30, 1590.0, 0.016f, 4 },  /* South Africa, marginal */
};
#define N_PHANTOMS (int)(sizeof(s_phantoms) / sizeof(s_phantoms[0]))

void wspr_sim_build_window(int16_t *pcm, long n, int64_t cycle_utc)
{
    /* Seeded from the cycle so a given window is reproducible - the same cycle
     * always builds the same audio - while consecutive cycles differ. That is
     * what makes a display bug reproducible instead of a story about something
     * that happened once. */
    s_rng = (uint32_t)(cycle_utc * 2654435761u) | 1u;

    wspr_sim_noise(pcm, n, 0.020f);

    int cyc = (int)(cycle_utc / 120);
    int added = 0;
    for (int i = 0; i < N_PHANTOMS; i++) {
        if ((cyc + i) % s_phantoms[i].duty != 0) continue;

        /* Per-cycle fading, +-6 dB. Real WSPR levels move between cycles by far
         * more than this, and a station whose SNR never changes would make the
         * spot list look synthetic at a glance. */
        float fade = powf(10.0f, (rng_gaussian() * 3.0f) / 20.0f);
        if (fade < 0.25f) fade = 0.25f;
        if (fade > 3.0f)  fade = 3.0f;

        /* Their clock error: real stations start 1.1-2.1 s in (measured from
         * WSJT's reference recording - see wspr_tx.c's start-offset note). */
        double start_s = 1.1 + 0.9 * ((rng_gaussian() + 3.0) / 6.0);
        if (start_s < 0.8) start_s = 0.8;
        if (start_s > 2.3) start_s = 2.3;

        /* A few Hz of dial error, as every real station has. */
        double f0 = s_phantoms[i].f0 + rng_gaussian() * 1.5;

        if (wspr_sim_add_station(pcm, n, s_phantoms[i].call, s_phantoms[i].grid,
                                 s_phantoms[i].dbm, f0, start_s,
                                 s_phantoms[i].amp * fade))
            added++;
    }

    /* Local QRM, one cycle in three. This is not decoration: a broadband burst
     * is what took the top of the waterfall's colour scale on the real band and
     * pushed genuine traces down to near-black. Keeping one in the simulation
     * makes that a case that can be reproduced and fixed rather than waited for. */
    if ((cyc % 3) == 0) {
        wspr_sim_add_burst(pcm, n, 20.0 + (cyc % 7) * 9.0, 1.2, 0.15f);
        wspr_sim_add_burst(pcm, n, 71.0, 0.6, 0.11f);
    }

    ESP_LOGI(TAG, "cycle %lld: %d phantom station(s) transmitting%s",
             (long long)cycle_utc, added, (cyc % 3) == 0 ? " + QRM burst" : "");
}
