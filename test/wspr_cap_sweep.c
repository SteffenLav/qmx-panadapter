/* How many WSPR stations does our decoder find if the CANDIDATE CAP is lifted?
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I components/ft8_lib -o wspr_cap_sweep \
 *       test/wspr_cap_sweep.c main/wspr_proto.c main/wspr_fano.c \
 *       main/wspr_decode.c components/ft8_lib/fft/kiss_fft.c \
 *       components/ft8_lib/fft/kiss_fftr.c -lm
 *   ./wspr_cap_sweep test/wav_reference/wspr/260824_1906.wav [max_cands]
 *
 * WHY THIS EXISTS
 *   The device ran with WSPR_MAX_CANDS = 8 and every one of its first 127
 *   cycles reported exactly 8 - the cap was saturated 100 % of the time, so
 *   "8 candidates" read like a measurement while being a ceiling. wsprd finds
 *   32 stations across the three 2026-08-24 reference windows where the device
 *   found 10.
 *
 *   That leaves two candidate explanations which the device CANNOT separate,
 *   because on hardware the decode has to fit inside a 120 s cycle:
 *     (a) the cap - the stations were never TRIED, or
 *     (b) the ranking - they were tried and our decoder cannot decode them.
 *   Here there is no time limit, so trying every candidate answers it directly.
 *
 * Links the REAL decoder. The algorithm is untouched by the cap change; the
 * only variable is how many candidates get a chance.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wspr_decode.h"

/* Guards are applied through the REAL wspr_guard_check(), never a copy: a
 * harness that mirrors the code under test only ever proves the mirror. */

#define MAX_SWEEP 64

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "test/wav_reference/wspr/260824_1906.wav";
    int max_c = argc > 2 ? atoi(argv[2]) : 20;
    if (max_c < 1 || max_c > MAX_SWEEP) max_c = 20;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);          /* canonical 44-byte header, verified by
                                      * test/wspr_wav_harness.c */
    long n = bytes / 2;
    int16_t *samples = malloc((size_t)n * sizeof(int16_t));
    if (!samples) { fclose(f); fprintf(stderr, "oom\n"); return 1; }
    if (fread(samples, sizeof(int16_t), (size_t)n, f) != (size_t)n) {
        fclose(f); free(samples); fprintf(stderr, "short read\n"); return 1;
    }
    fclose(f);

    printf("%s: %ld samples (%.1f s), trying up to %d candidates\n",
           path, n, n / 12000.0, max_c);

    /* --guards applies the SHIPPED guard set, so this can measure what the
     * device would actually PUBLISH rather than what the raw decoder emitted.
     * Without it a noise-born fabrication counts as a decode, and any
     * sensitivity number built on that is wrong - measured 2026-08-25, where
     * two "decodes" at +8 and +10 dB of added noise were both fabrications
     * that the SLOW guard catches. */
    int use_guards = 0;
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a], "--guards")) use_guards = 1;

    wspr_guards_t guards;
    wspr_guards_defaults(&guards);
    wspr_accepted_t accepted = { 0 };

    wspr_freq_candidate_t cands[MAX_SWEEP];
    int ncand = wspr_find_candidates(samples, n, 1350.0, 1650.0, cands, max_c);
    printf("  found %d candidate(s)%s\n", ncand,
           use_guards ? " (guards ENFORCED)" : " (raw decoder, no guards)");

    int decoded = 0, beyond8 = 0, guarded = 0;
    for (int i = 0; i < ncand; i++) {
        wspr_decode_result_t r;
        wspr_decode_candidate(samples, n, cands[i].freq_hz, &r);

        int rej = 0;
        if (r.ok && use_guards) {
            double dnear; int wn, ws;
            if (wspr_guard_check(&guards, &accepted, &r, &dnear, &wn, &ws) != WSPR_GUARD_PASS)
                rej = 1;
        }

        printf("  cand %2d f=%7.2f score=%9.3g cycles=%6u  %s",
               i, cands[i].freq_hz, (double)cands[i].comb_score, r.cycles,
               !r.ok ? "rejected" : (rej ? "GUARDED " : "DECODED "));
        if (r.ok) {
            printf(" '%s' '%s' %d dBm", r.callsign, r.grid, r.power_dbm);
            if (rej) guarded++;
            else {
                decoded++;
                if (i >= 8) beyond8++;
                if (use_guards) wspr_accepted_add(&accepted, r.freq_hz);
            }
        }
        printf("\n");
    }
    if (use_guards) printf("  %d guarded off\n", guarded);
    /* The number that matters: decodes that the old cap of 8 could never have
     * reached, no matter how good the decoder was. */
    printf("  => %d decode(s) of %d tried; %d of them beyond the old cap of 8\n",
           decoded, ncand, beyond8);
    free(samples);
    return 0;
}
