/* Does SUCCESSIVE SUBTRACTION recover the stations hiding under stronger ones?
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I components/ft8_lib -o wspr_subtract_sweep \
 *       test/wspr_subtract_sweep.c main/wspr_proto.c main/wspr_fano.c \
 *       main/wspr_decode.c main/wspr_subtract.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c -lm
 *   ./wspr_subtract_sweep test/wav_reference/wspr/260824_1910.wav [cands] [passes]
 *
 * Scored against the .txt sidecars, which hold what wsprd got from the same
 * audio - so this reports a number that can be compared, not just a number.
 *
 * Links the REAL decoder and the REAL subtractor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wspr_decode.h"
#include "wspr_subtract.h"

#define MAXC 64
#define MAXFOUND 64

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "test/wav_reference/wspr/260824_1910.wav";
    int max_c  = argc > 2 ? atoi(argv[2]) : 24;
    int passes = argc > 3 ? atoi(argv[3]) : 3;
    if (max_c < 1 || max_c > MAXC) max_c = 24;
    if (passes < 1) passes = 1;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f) - 44; fseek(f, 44, SEEK_SET);
    long n = bytes / 2;
    int16_t *s = malloc((size_t)n * sizeof(int16_t));
    if (!s || fread(s, sizeof(int16_t), (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);

    char found[MAXFOUND][8];
    int nfound = 0;

    for (int p = 0; p < passes; p++) {
        wspr_freq_candidate_t c[MAXC];
        int nc = wspr_find_candidates(s, n, 1350.0, 1650.0, c, max_c);
        int newly = 0, subbed = 0;

        for (int i = 0; i < nc; i++) {
            wspr_decode_result_t r;
            wspr_decode_candidate(s, n, c[i].freq_hz, &r);
            if (!r.ok) continue;

            int dup = 0;
            for (int k = 0; k < nfound; k++)
                if (!strcmp(found[k], r.callsign)) { dup = 1; break; }
            if (!dup && nfound < MAXFOUND) {
                snprintf(found[nfound++], 8, "%s", r.callsign);
                newly++;
                printf("  pass %d: '%s' '%s' %d dBm  f=%.2f cyc=%u\n",
                       p, r.callsign, r.grid, r.power_dbm, r.freq_hz, r.cycles);
            }

            /* Subtract EVERY decode, including duplicates: a station already
             * logged in an earlier pass is still sitting in the waveform
             * masking its neighbour, and leaving it there defeats the purpose. */
            uint8_t tones[WSPR_NSYM];
            if (wspr_tones_from_message(r.callsign, r.grid, r.power_dbm, tones))
                subbed += (wspr_subtract(s, n, r.freq_hz, r.best_dt_samples, tones) > 0);
        }
        printf("  pass %d: %d candidate(s), %d new, %d signal(s) subtracted\n",
               p, nc, newly, subbed);
        if (newly == 0 && p > 0) break;      /* nothing more to uncover */
    }

    printf("=> %s: %d unique station(s) over %d pass(es)\n", path, nfound, passes);
    free(s);
    return 0;
}
