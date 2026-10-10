/* Host test: JS8 free-text ENCODING (J8).
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_jsc_encode_harness test/js8_jsc_encode_harness.c \
 *       components/ft8_lib/ft8/js8_jsc.c \
 *       components/ft8_lib/ft8/js8_jsc_encode.c \
 *       components/ft8_lib/ft8/js8_jsc_tables.c
 *
 * WHY THIS IS A REAL TEST AND NOT SELF-AGREEMENT. The decoder it round-trips
 * through was validated against REAL JS8Call transmissions off air (J7, the
 * `QSL` decode on 40 m, 2026-10-09) - it is not a mirror written alongside the
 * encoder. So "encode, then decode, and get the same text back" exercises the
 * encoder against a reference that an outside station already agreed with.
 *
 * ⛔ WHAT IT STILL CANNOT PROVE: that JS8Call decodes OUR frames. Only a real
 * station can say that, and until one has, the encoder is UNVERIFIED on air no
 * matter how green this file is.
 */
#include <stdio.h>
#include <string.h>

#include "js8_jsc.h"

static int g_fail;

static void check(int cond, const char* what)
{
    if (!cond) { printf("  FAIL: %s\n", what); g_fail++; }
}

/* Encode `text`, decode every frame back, and compare with the normalised
 * input. Returns the number of frames used. */
static int roundtrip(const char* text, int expect_ok)
{
    char norm[512];
    js8_jsc_normalize(text, norm, sizeof(norm));

    uint8_t frames[8 * 9];
    int n = js8_jsc_text_to_frames(text, frames, 8);

    char got[1024];
    got[0] = '\0';
    for (int i = 0; i < n; i++) {
        char part[JS8_JSC_TEXT_MAX + 1];
        if (!js8_jsc_frame_to_text(frames + (size_t)i * 9, part, sizeof(part))) {
            printf("  FAIL: frame %d of '%s' did not decode\n", i, norm);
            g_fail++;
            return n;
        }
        strncat(got, part, sizeof(got) - strlen(got) - 1);
    }

    if (expect_ok && strcmp(got, norm) != 0) {
        printf("  FAIL: '%s'\n        sent '%s'\n        got  '%s'  (%d frames)\n",
               text, norm, got, n);
        g_fail++;
    }
    return n;
}

int main(void)
{
    printf("js8_jsc_encode_harness\n");

    /* ---- normalisation, because the frame count belongs to the SENT text */
    {
        char out[64];
        js8_jsc_normalize("  hello   world  ", out, sizeof(out));
        check(!strcmp(out, "HELLO WORLD"), "normalize collapses and trims");

        js8_jsc_normalize("a\tb\nc", out, sizeof(out));
        check(!strcmp(out, "A B C"), "normalize folds tabs and newlines");

        /* A character with no Huffman code has no representation in EITHER
         * coding and is dropped - the operator sees that in the preview. */
        js8_jsc_normalize("OK~~GOOD", out, sizeof(out));
        check(strstr(out, "~") == NULL, "normalize drops unsendable characters");
    }

    /* ---- round trips ------------------------------------------------- */
    printf("  round trips:\n");
    const char* cases[] = {
        "HELLO",
        "QSL",
        "HELLO WORLD",
        "THANKS FOR THE CALL",
        "RIG IS A QMX RUNNING 5 WATTS INTO A DIPOLE",
        "73 AND GOOD DX",
        "TEMP IS 12C AND RAINING HERE IN DENMARK",
        /* Mixed: common words that should word-code, plus a token that will
           not be in the truncated table and must fall back to Huffman. */
        "THE WEATHER IS ZZQX TODAY",
        /* Digits and punctuation that the alphabet does carry. */
        "QRV 14078 AT 1200Z",
        /* Long enough to need several frames. */
        "THIS IS A LONGER MESSAGE THAT WILL NOT FIT INTO ONE SINGLE FRAME "
        "AND THEREFORE HAS TO BE SPLIT ACROSS SEVERAL OF THEM IN SEQUENCE",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int n = roundtrip(cases[i], 1);
        char norm[512];
        js8_jsc_normalize(cases[i], norm, sizeof(norm));
        printf("    %2d frame(s)  %2d s  '%s'\n", n, n * 15, norm);
    }

    /* ---- the pad invariant, which is what makes decoding safe --------- */
    {
        /* Every frame we emit must contain a zero somewhere after the flags,
         * or js8_jsc_unpad() cannot find the end of the content. Checked on a
         * text long enough to push a frame right up to the limit. */
        uint8_t frames[8 * 9];
        int n = js8_jsc_text_to_frames(
            "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE",
            frames, 8);
        check(n > 0, "a long single-character run encodes");
        for (int i = 0; i < n; i++) {
            char part[JS8_JSC_TEXT_MAX + 1];
            check(js8_jsc_frame_to_text(frames + (size_t)i * 9, part, sizeof(part)),
                  "a brim-full frame still decodes (the pad zero survived)");
        }
    }

    /* ---- nothing encodable --------------------------------------------- */
    {
        uint8_t frames[9];
        int n = js8_jsc_text_to_frames("~~~~", frames, 1);
        check(n == 0, "text with nothing sendable produces no frames");
        n = js8_jsc_text_to_frames("", frames, 1);
        check(n == 0, "empty text produces no frames");
    }

    /* ---- fuzz, because ten hand-picked strings prove very little -------
     *
     * Ten cases chose themselves to be reasonable. The boundary bug this file
     * found only appeared in the ONE case long enough to straddle frames, and
     * only at a particular word length - so the interesting inputs are the
     * ones nobody would think to write down. Fixed seed, so a failure is
     * reproducible rather than a story about a run that once went wrong. */
    {
        printf("  fuzz:\n");
        unsigned seed = 20261010u;
        int worst_frames = 0, n_run = 0;
        for (int iter = 0; iter < 3000; iter++) {
            char msg[200];
            size_t n = 0;
            int nw = 1 + (int)(seed % 9);
            for (int w = 0; w < nw && n + 24 < sizeof(msg); w++) {
                seed = seed * 1103515245u + 12345u;
                /* Mix table words with junk tokens, so both codings and the
                 * fallback between them are exercised. */
                const char* word = js8_jsc_word((seed >> 8) % 4000u);
                if ((seed >> 4) & 3u) {
                    if (!word) continue;
                    size_t wl = strlen(word);
                    if (n + wl + 2 >= sizeof(msg)) break;
                    memcpy(msg + n, word, wl);
                    n += wl;
                } else {
                    int jl = 1 + (int)((seed >> 16) % 7u);
                    for (int c = 0; c < jl && n + 2 < sizeof(msg); c++) {
                        seed = seed * 1103515245u + 12345u;
                        msg[n++] = (char)('A' + (seed >> 12) % 26u);
                    }
                }
                if (w + 1 < nw) msg[n++] = ' ';
            }
            msg[n] = '\0';
            if (!n) continue;

            char norm[256];
            if (js8_jsc_normalize(msg, norm, sizeof(norm)) <= 0) continue;

            uint8_t frames[12 * 9];
            int nf = js8_jsc_text_to_frames(msg, frames, 12);
            if (nf <= 0) { printf("  FAIL: fuzz produced no frames for '%s'\n", norm); g_fail++; break; }

            char got[512];
            got[0] = '\0';
            int bad = 0;
            for (int i = 0; i < nf; i++) {
                char part[JS8_JSC_TEXT_MAX + 1];
                if (!js8_jsc_frame_to_text(frames + (size_t)i * 9, part, sizeof(part))) { bad = 1; break; }
                strncat(got, part, sizeof(got) - strlen(got) - 1);
            }
            if (bad || strcmp(got, norm) != 0) {
                printf("  FAIL: fuzz iter %d\n        sent '%s'\n        got  '%s'\n",
                       iter, norm, got);
                g_fail++;
                break;
            }
            if (nf > worst_frames) worst_frames = nf;
            n_run++;
        }
        printf("    %d messages round-tripped, worst %d frames\n", n_run, worst_frames);
    }

    if (g_fail) { printf("js8_jsc_encode_harness: %d FAILED\n", g_fail); return 1; }
    printf("js8_jsc_encode_harness: pass\n");
    return 0;
}
