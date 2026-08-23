/* Host test for the WSPR audio-domain decoder (main/wspr_decode.c) against
 * a REAL captured WSPR WAV file - the actual Phase 1 deliverable per
 * docs/wspr-scope.md: "Get this decoding real, known signals correctly
 * before it ever touches the device."
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I components/ft8_lib \
 *       -o wspr_decode_harness test/wspr_decode_harness.c \
 *       main/wspr_proto.c main/wspr_fano.c main/wspr_decode.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -lm && ./wspr_decode_harness
 *
 * WAV: test/wav_reference/wspr/150426_0918.wav - WSJT's own official WSPR
 * sample recording (sourceforge.net/projects/wsjt/files/samples/WSPR/),
 * 120 s / 12000 Hz / 16-bit mono, the standard WSPR RX capture format.
 * Tracked in git like every other file under test/wav_reference/ (not a
 * copyrighted vendor manual - a small, freely-distributed WSJT project
 * sample file, same status as the existing FT8 reference WAVs).
 *
 * GROUND TRUTH. This file's actual transmitted content isn't documented
 * anywhere findable - there's no "here's what 150426_0918.wav decodes to"
 * reference page. What stands in for ground truth here: five of the eight
 * detected candidate frequencies decode to standard-format US amateur
 * callsigns (W3HH, WD4LHT, ND6P, W5BIT, KI7CI), each with a legal WSPR
 * power value and a clean Fano convergence (82-102 cycles, vs 49400-78894
 * for the three rejected candidates) - three independent signals that all
 * agree. That is real corroboration, not proof against an authoritative
 * source; if this file's true content is ever found published, replace
 * this comment and tighten the assertions.
 *
 * The file's own STRONGEST signal (by a 3x margin) does NOT decode
 * plausibly - RESOLVED (test/wspr_diag_candidate0.c, docs/wspr-phase1-
 * status.md): real ionospheric fading (QSB) within the transmission, not
 * a bug - confirmed frequency drift was NOT the cause, then found the
 * signal's sync-bit match rate climbs from ~52-63% to 81-89% and its
 * total power rises ~20x over the 110s transmission, both textbook QSB,
 * with no second overlapping signal (a +/-3Hz scan shows one clean peak).
 * A uniform-confidence metric can't recover ~46/162 symbol errors
 * concentrated in the noisy first half even though the signal is real -
 * fixing this needs PER-SYMBOL (not per-capture) confidence weighting, a
 * genuinely different idea from the soft-metric attempts tried so far.
 * The regression test below asserts the 5 that DO work, and separately
 * checks that exactly 3 are rejected, so a future fix for this candidate
 * shows up as an easy "6th decode appeared" rather than a silent behavior
 * change
 * nobody notices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wspr_proto.h"
#include "wspr_fano.h"
#include "wspr_decode.h"

static int g_fail = 0;

static int16_t *load_wav_mono16(const char *path, long *n_out, long *rate_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        fclose(f);
        return NULL;
    }
    int channels = 1, bits = 16;
    long rate = 12000;
    int16_t *samples = NULL;
    long nsamples = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t cksize = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            fread(fmt, 1, cksize < 16 ? cksize : 16, f);
            channels = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            if (cksize > 16) fseek(f, (long)(cksize - 16), SEEK_CUR);
        } else if (memcmp(ck, "data", 4) == 0) {
            int bytes_per_frame = (bits / 8) * channels;
            nsamples = (bytes_per_frame > 0) ? (long)(cksize / (uint32_t)bytes_per_frame) : 0;
            samples = (int16_t *)malloc((size_t)nsamples * sizeof(int16_t));
            for (long i = 0; i < nsamples; i++) {
                uint8_t s[8];
                if (fread(s, 1, (size_t)bytes_per_frame, f) != (size_t)bytes_per_frame) {
                    nsamples = i;
                    break;
                }
                samples[i] = (int16_t)(s[0] | (s[1] << 8)); /* channel 0, 16-bit */
            }
            break;
        } else {
            fseek(f, (long)(cksize + (cksize & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (!samples || channels != 1 || bits != 16) {
        fprintf(stderr, "%s: expected mono 16-bit PCM, got %d ch %d bit\n", path, channels, bits);
        free(samples);
        return NULL;
    }
    *n_out = nsamples;
    *rate_out = rate;
    return samples;
}

typedef struct {
    const char *call;
    const char *grid;
    int dbm;
} expected_t;

static const expected_t kExpected[] = {
    { "W3HH",   "EL89", 30 },
    { "WD4LHT", "EL89", 30 },
    { "ND6P",   "DM04", 30 },
    { "W5BIT",  "EL09", 17 },
    { "KI7CI",  "DM09", 37 },
};
#define N_EXPECTED (int)(sizeof(kExpected) / sizeof(kExpected[0]))

int main(void)
{
    const char *path = "test/wav_reference/wspr/150426_0918.wav";
    long n = 0, rate = 0;
    int16_t *samples = load_wav_mono16(path, &n, &rate);
    if (!samples) {
        printf("FAIL  could not load %s\n", path);
        return 1;
    }
    if (rate != (long)WSPR_SAMPLE_RATE_HZ) {
        printf("FAIL  %s is %ld Hz, expected %.0f\n", path, rate, WSPR_SAMPLE_RATE_HZ);
        free(samples);
        return 1;
    }
    printf("loaded %s: %ld samples (%.1f s)\n\n", path, n, n / (double)rate);

    wspr_freq_candidate_t cands[8];
    int ncand = wspr_find_candidates(samples, n, 1350.0, 1650.0, cands, 8);
    printf("found %d frequency candidates\n\n", ncand);

    int matched[N_EXPECTED] = { 0 };
    int n_ok = 0, n_rejected = 0;

    for (int c = 0; c < ncand; c++) {
        wspr_decode_result_t r;
        wspr_decode_candidate(samples, n, cands[c].freq_hz, &r);
        if (r.ok) {
            n_ok++;
            printf("  #%d f0=%.3f Hz  DECODED call='%s' grid='%s' power=%d dBm  cycles=%u\n",
                   c, cands[c].freq_hz, r.callsign, r.grid, r.power_dbm, r.cycles);
            for (int e = 0; e < N_EXPECTED; e++) {
                if (strcmp(r.callsign, kExpected[e].call) == 0
                    && strcmp(r.grid, kExpected[e].grid) == 0
                    && r.power_dbm == kExpected[e].dbm) {
                    matched[e] = 1;
                }
            }
        } else {
            n_rejected++;
            printf("  #%d f0=%.3f Hz  rejected (cycles=%u)\n", c, cands[c].freq_hz, r.cycles);
        }
    }

    printf("\n-- checking the 5 known-good decodes --\n");
    for (int e = 0; e < N_EXPECTED; e++) {
        printf("  %s  %s/%s/%d\n", matched[e] ? "PASS" : "FAIL",
               kExpected[e].call, kExpected[e].grid, kExpected[e].dbm);
        if (!matched[e]) g_fail++;
    }

    printf("\n-- checking the reject count --\n");
    /* Documents the file's own strongest (cause-unconfirmed) candidate
     * plus 2 noise candidates as an explicit, visible assertion - see the
     * file header. If that candidate is ever explained/fixed, this count
     * should drop and this assertion should be updated deliberately, not
     * silently pass either way. */
    int reject_ok = (n_rejected == 3);
    printf("  %s  %d rejected (expected 3)\n", reject_ok ? "PASS" : "FAIL", n_rejected);
    if (!reject_ok) g_fail++;

    printf("\n%s (%d failure%s) - %d/%d candidates decoded plausibly\n",
           g_fail == 0 ? "ALL PASS" : "FAILED", g_fail, g_fail == 1 ? "" : "s",
           n_ok, ncand);

    free(samples);
    return g_fail == 0 ? 0 : 1;
}
