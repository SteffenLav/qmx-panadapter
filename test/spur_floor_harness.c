// Host test for main/dsp/spur_floor.c - the constancy-gated display floor.
//
// Build (from the repo root):
//   gcc -O2 -I main/dsp -o spur_floor_harness test/spur_floor_harness.c main/dsp/spur_floor.c -lm
//   ./spur_floor_harness
//   ./spur_floor_harness --mutate     <- must FAIL, see below
//
// This exists because the operator's requirement cannot be checked on the
// bench: "I won't accept any medium to bigger real signal rejection", and there
// is no antenna available to produce a real signal with. So the requirement is
// stated here as a set of synthetic signal classes with explicit pass bounds,
// and the shipped code is linked directly rather than mirrored.
//
// --mutate opens the constancy gate (steady_mad_db huge), which is exactly the
// old ungated behaviour. The real-signal cases MUST fail in that mode; if they
// pass, this harness is not actually measuring anything and must be fixed
// before it is trusted.

#include "spur_floor.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPS          10.0f     // ui_push_spectrum rate
#define RUN_SECONDS  600       // 10 minutes - far beyond the ~50 s absorb time
#define N_FRAMES     ((int)(RUN_SECONDS * FPS))

// Deterministic uniform (0,1). Own generator so results do not depend on libc.
// Must be 64-bit: unsigned long is 32 bits on mingw and the shift was UB.
static unsigned long long s_rng = 20260813ULL;
static float urand(void)
{
    s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    unsigned int x = (unsigned int)(s_rng >> 33);
    return ((float)x + 1.0f) / 2147483650.0f;
}

// One FFT bin's power in dB: signal power (linear, 1.0 == noise mean) plus
// non-coherent noise. Exponentially distributed noise power is what a complex
// Gaussian bin actually produces, and it is what gives an empty bin its ~5.6 dB
// spread - the number the constancy gate has to stay clear of.
static float bin_db(float sig_lin)
{
    float e = -logf(urand());
    return 10.0f * log10f(sig_lin + e);
}

typedef float (*sig_fn)(int frame);

static float sig_noise(int f)   { (void)f; return 0.0f; }

// The measured QMX spur: +38.7 dB, utterly unvarying.
static float sig_spur(int f)    { (void)f; return powf(10.0f, 38.7f / 10.0f); }

// A steady unmodulated carrier - the acknowledged hard case, reported not asserted.
static float sig_carrier(int f) { (void)f; return powf(10.0f, 38.0f / 10.0f); }

// CW at ~15 wpm: 80 ms elements, on/off.
static float sig_cw(int f)
{
    int on = ((int)(f / (0.08f * FPS)) % 2) == 0;
    return on ? powf(10.0f, 30.0f / 10.0f) : 0.0f;
}

// FT8: 15 s on, 15 s off.
static float sig_ft8(int f)
{
    int slot = (int)(f / (15.0f * FPS));
    return (slot % 2) ? powf(10.0f, 20.0f / 10.0f) : 0.0f;
}

// SSB: speech-like amplitude movement, several dB, a few Hz.
static float sig_ssb(int f)
{
    float env = 0.5f + 0.5f * sinf((float)f * 0.7f) * sinf((float)f * 0.13f);
    return powf(10.0f, 25.0f / 10.0f) * (0.1f + 0.9f * env * env);
}

// A carrier with ordinary QSB: +-4 dB, ~40 s period.
static float sig_qsb(int f)
{
    float d = 4.0f * sinf((float)f * (2.0f * 3.14159265f / (40.0f * FPS)));
    return powf(10.0f, (30.0f + d) / 10.0f);
}

typedef struct {
    const char *name;
    sig_fn      fn;
    float       true_db;    // level over the noise floor, for reference
    bool        expect_suppressed;
    float       max_loss_db; // pass bound when expect_suppressed is false
} case_t;

// Returns dB of suppression: how much of the signal's true prominence the
// tracked floor has eaten by the end of the run.
static float run_case(const case_t *c, const spur_floor_cfg_t *cfg, float *out_final)
{
    float smooth, floor, mad;
    spur_floor_seed(bin_db(c->fn(0)), 0.0f, &smooth, &floor, &mad);

    // Peak displayed value over the last 10 s, so a keyed or bursty signal is
    // judged on its ON periods rather than on whatever phase the run ends in.
    float peak_late = -1e9f;
    int late_from = N_FRAMES - (int)(10 * FPS);

    for (int f = 0; f < N_FRAMES; f++) {
        float disp = spur_floor_step(cfg, bin_db(c->fn(f)), &smooth, &floor, &mad);
        if (f >= late_from && disp > peak_late) peak_late = disp;
    }
    *out_final = peak_late;
    return c->true_db - peak_late;
}

int main(int argc, char **argv)
{
    bool mutate = (argc > 1 && strcmp(argv[1], "--mutate") == 0);

    spur_floor_cfg_t cfg = SPUR_FLOOR_CFG_DEFAULT;
    if (mutate) cfg.steady_mad_db = 1e9f;   // gate always open == old behaviour

    const case_t cases[] = {
        { "noise floor",        sig_noise,    0.0f, false, 3.0f  },
        { "QMX spur (38.7 dB)", sig_spur,    38.7f, true,  0.0f  },
        { "CW keyed (30 dB)",   sig_cw,      30.0f, false, 1.0f  },
        { "FT8 burst (20 dB)",  sig_ft8,     20.0f, false, 1.0f  },
        { "SSB voice (25 dB)",  sig_ssb,     25.0f, false, 1.0f  },
        { "carrier +QSB (30dB)",sig_qsb,     34.0f, false, 1.0f  },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    printf("spur_floor harness - %d s at %.0f fps%s\n",
           RUN_SECONDS, FPS, mutate ? "   [MUTATED: gate disabled]" : "");
    printf("%-22s %10s %10s %10s   %s\n",
           "case", "true dB", "shown dB", "lost dB", "verdict");

    int failures = 0;
    for (int i = 0; i < n; i++) {
        float shown = 0.0f;
        float lost = run_case(&cases[i], &cfg, &shown);
        const char *verdict;
        bool ok;
        if (cases[i].expect_suppressed) {
            ok = (lost >= 20.0f);
            verdict = ok ? "ok (suppressed)" : "FAIL - spur still visible";
        } else {
            ok = (lost <= cases[i].max_loss_db);
            verdict = ok ? "ok (preserved)" : "FAIL - real signal eaten";
        }
        if (!ok) failures++;
        printf("%-22s %10.1f %10.1f %10.1f   %s\n",
               cases[i].name, cases[i].true_db, shown, lost, verdict);
    }

    // Reported, never asserted: a genuinely unmodulated carrier is
    // indistinguishable from a spur by this or any other constancy test, and
    // pretending otherwise would be dishonest. In practice nothing on the air
    // holds still like this - which is the assumption the whole design rests on.
    {
        case_t steady = { "steady carrier", sig_carrier, 38.0f, false, 0.0f };
        float shown = 0.0f;
        float lost = run_case(&steady, &cfg, &shown);
        printf("\nnote: unmodulated steady carrier loses %.1f dB "
               "(known limit, not asserted)\n", lost);
    }

    if (mutate) {
        if (failures == 0) {
            printf("\nMUTATION TEST FAILED: real signals survived with the gate "
                   "disabled, so this harness proves nothing.\n");
            return 2;
        }
        printf("\nmutation test ok: %d case(s) failed with the gate disabled, "
               "so the assertions do bite.\n", failures);
        return 0;
    }

    printf("\n%s\n", failures ? "FAILURES PRESENT" : "all cases pass");
    return failures ? 1 : 0;
}
