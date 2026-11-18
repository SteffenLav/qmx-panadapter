/* Decode a conformance vector and dump the PCM, so the pristine and hoisted
 * headers can be compared byte-for-byte.
 *
 * ⭐ THE DECISIVE COMPARISON IS PRISTINE vs HOISTED, AND IT MUST BE
 * BIT-IDENTICAL. Only the storage class of one local changed - same code,
 * same arithmetic, same order - so any difference at all means the hoist
 * altered behaviour, which is precisely the scratch-reuse fault the silent
 * frame could not detect.
 *
 * The reference .pcm is a weaker check: minimp3's own test suite compares by
 * PSNR (README: "PSNR > 96db"), not bytes, because a float decoder need not
 * match the ISO reference exactly. So a mismatch against the reference is not
 * by itself a defect; a mismatch between the two builds is.
 *
 *   gcc -O2 -I<header-dir> -o ab_<name> minimp3_ab.c
 *   ./ab_<name> <vector.bit> <out.pcm>
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <in.bit> <out.pcm>\n", argv[0]); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(f);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 2; }

    /* One decoder across the whole stream - that is what makes the scratch
     * reusable across calls and therefore what the test is about. */
    static mp3dec_t dec;
    mp3dec_init(&dec);
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;

    long off = 0, frames = 0, samples_total = 0;
    while (off < n) {
        int samples = mp3dec_decode_frame(&dec, buf + off, (int)(n - off), pcm, &info);
        if (info.frame_bytes <= 0) break;              /* out of data */
        off += info.frame_bytes;
        if (samples > 0) {
            fwrite(pcm, sizeof(mp3d_sample_t), (size_t)samples * info.channels, o);
            frames++;
            samples_total += samples;
        }
    }
    fclose(o);
    free(buf);
    printf("%-34s %5ld frames %8ld samples\n", argv[1], frames, samples_total);
    return 0;
}
