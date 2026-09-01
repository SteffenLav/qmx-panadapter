// Signal-class generators for anything that filters the panadapter display
// per bin, plus the record of one approach that was tried and rejected.
//
// Build (from the repo root):
//   gcc -O2 -o spur_floor_harness test/spur_floor_harness.c -lm
//   ./spur_floor_harness
//
// WHY THIS EXISTS. The operator's requirement on any display filter is blunt:
// "I won't accept any medium to bigger real signal rejection." That cannot be
// checked on the bench - there is no antenna to produce a real signal with - so
// it is stated here instead, as synthetic classes with explicit bounds.
//
// WHAT WAS REJECTED. The first attempt at hiding the QMX's synthesizer spurs
// classified bins by how CONSTANT they were: track a per-bin mean and a measure
// of variation, and let the display floor climb only into bins whose variation
// had collapsed. The reasoning was that a spur holds its level to a fraction of
// a dB indefinitely while nothing on the air does.
//
// This harness killed it. Run against these classes it scored slow QSB as
// "steady" and ate 30 dB of a fading carrier, plus 13 dB of an FT8 burst and
// 5.7 dB of keyed CW - precisely the failure the operator had ruled out. The
// mistake was measuring frame-to-frame deviation, which is near zero for a
// signal fading over tens of seconds. Widening the window to minutes fixed that
// case and broke others; the approach was abandoned.
//
// What shipped instead needs no inference at all: main/dsp/spur_map.c nudges the
// dial 25 Hz and watches which bins move. A spur's offset changes at 16-50x the
// dial, a real signal at 1x - a physical discriminator with an order of
// magnitude of margin rather than a statistical guess. See spur_map.h.
//
// KEEP THESE GENERATORS. Any future per-bin filter should be run against them
// before it goes near the hardware, because the hardware cannot test the one
// requirement that matters.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FPS          10.0f     // ui_push_spectrum rate
#define RUN_SECONDS  600
#define N_FRAMES     ((int)(RUN_SECONDS * FPS))

// Deterministic uniform (0,1). Own generator so results do not depend on libc,
// and 64-bit because unsigned long is 32 bits on mingw and the shift was UB.
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
// spread - the figure any constancy test has to stay clear of.
static float bin_db(float sig_lin)
{
    float e = -logf(urand());
    return 10.0f * log10f(sig_lin + e);
}

static float db2lin(float db) { return powf(10.0f, db / 10.0f); }

typedef float (*sig_fn)(int frame);

static float sig_noise(int f)   { (void)f; return 0.0f; }

// The measured QMX spur at 14.074: +38.6 dB, unvarying.
static float sig_spur(int f)    { (void)f; return db2lin(38.6f); }

// An unmodulated steady carrier - indistinguishable from a spur by any
// constancy test, which is why constancy was the wrong discriminator.
static float sig_carrier(int f) { (void)f; return db2lin(38.0f); }

// CW at ~15 wpm: 80 ms elements, on/off.
static float sig_cw(int f)
{
    int on = ((int)(f / (0.08f * FPS)) % 2) == 0;
    return on ? db2lin(30.0f) : 0.0f;
}

// FT8: 15 s on, 15 s off.
static float sig_ft8(int f)
{
    int slot = (int)(f / (15.0f * FPS));
    return (slot % 2) ? db2lin(20.0f) : 0.0f;
}

// SSB: speech-like amplitude movement, several dB, a few Hz.
static float sig_ssb(int f)
{
    float env = 0.5f + 0.5f * sinf((float)f * 0.7f) * sinf((float)f * 0.13f);
    return db2lin(25.0f) * (0.1f + 0.9f * env * env);
}

// A carrier with ordinary QSB: +/-4 dB, ~40 s period. This is the class that
// falsified the constancy approach - slow fading is not frame-to-frame variation.
static float sig_qsb(int f)
{
    float d = 4.0f * sinf((float)f * (2.0f * 3.14159265f / (40.0f * FPS)));
    return db2lin(30.0f + d);
}

typedef struct {
    const char *name;
    sig_fn      fn;
    float       nominal_db;
    int         is_artifact;   // 1 = a filter SHOULD remove it
} case_t;

// Peak-to-valley excursion over the run, and the mean level. Any candidate
// filter can be judged on these: an artifact shows a tiny excursion, everything
// real shows a large one. Reported rather than asserted, because the numbers are
// the point - a future filter asserts against them, this prints them.
static void profile(const case_t *c)
{
    float lo = 1e9f, hi = -1e9f, sum = 0.0f;
    for (int f = 0; f < N_FRAMES; f++) {
        float v = bin_db(c->fn(f));
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }
    printf("%-22s %8.1f %9.1f %9.1f %10.1f   %s\n",
           c->name, c->nominal_db, sum / (float)N_FRAMES, hi - lo, lo,
           c->is_artifact ? "artifact - remove" : "REAL - must survive");
}

int main(void)
{
    const case_t cases[] = {
        { "noise floor",         sig_noise,    0.0f, 0 },
        { "QMX spur (38.6 dB)",  sig_spur,    38.6f, 1 },
        { "steady carrier",      sig_carrier, 38.0f, 0 },
        { "CW keyed (30 dB)",    sig_cw,      30.0f, 0 },
        { "FT8 burst (20 dB)",   sig_ft8,     20.0f, 0 },
        { "SSB voice (25 dB)",   sig_ssb,     25.0f, 0 },
        { "carrier +QSB (30 dB)",sig_qsb,     30.0f, 0 },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    printf("signal classes for panadapter display filters"
           "  (%d s at %.0f fps)\n\n", RUN_SECONDS, FPS);
    printf("%-22s %8s %9s %9s %10s   %s\n",
           "class", "nominal", "mean dB", "excursion", "min dB", "verdict");
    for (int i = 0; i < n; i++) profile(&cases[i]);

    printf("\nNote the two rows that look alike: the QMX spur and an\n"
           "unmodulated steady carrier. No amount of watching separates them,\n"
           "which is why spur_map.c moves the LO instead of watching.\n");
    return 0;
}
