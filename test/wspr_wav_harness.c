/* Host test for wspr_wav_header() / wspr_wav_filename().
 *
 * Build (from the repo root):
 *   gcc -I main -o wspr_wav_harness test/wspr_wav_harness.c main/wspr_wav.c \
 *       && ./wspr_wav_harness
 *
 * Why this exists: the WSPR capture dump writes a 2.88 MB WAV to the SD card so
 * the same window can be run through real wsprd on a PC - the only way to tell
 * "our sensitivity is short" from "those traces are not WSPR". If the header is
 * wrong the file is silently useless: wsprd either refuses it or reads the
 * wrong length, and either way the answer we went to the trouble of collecting
 * is garbage. None of that is worth discovering after an hour of captures.
 *
 * ⭐ THE STRONGEST TEST HERE IS AN EXTERNAL VECTOR. test/wav_reference/ holds
 * WSJT-X's own recordings, whose headers were written by somebody else's code.
 * Byte-comparing against those checks our layout against something other than
 * our own reasoning - the same reason the LoTW harness uses CardSat's worked
 * example rather than only self-consistent cases.
 *
 * It links the REAL functions, not copies.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "wspr_wav.h"

static int fails = 0;

static void ok(const char *what, int cond)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "*** FAIL ***");
    if (!cond) fails++;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
}

int main(void)
{
    uint8_t h[WSPR_WAV_HDR_BYTES];

    /* ---- 1. structure of a real 120 s WSPR window at 12 kHz ---- */
    const uint32_t n = 120u * 12000u;               /* 1,440,000 samples */
    size_t got = wspr_wav_header(h, n, 12000);
    ok("returns 44 for a 120 s window", got == WSPR_WAV_HDR_BYTES);
    ok("starts RIFF",                   memcmp(h + 0,  "RIFF", 4) == 0);
    ok("WAVE tag",                      memcmp(h + 8,  "WAVE", 4) == 0);
    ok("fmt  chunk id",                 memcmp(h + 12, "fmt ", 4) == 0);
    ok("data chunk id",                 memcmp(h + 36, "data", 4) == 0);
    ok("fmt body size is 16",           rd_u32(h + 16) == 16);
    ok("format is PCM (1)",             rd_u16(h + 20) == 1);
    ok("mono",                          rd_u16(h + 22) == 1);
    ok("12000 Hz",                      rd_u32(h + 24) == 12000);
    ok("byte rate 24000",               rd_u32(h + 28) == 24000);
    ok("block align 2",                 rd_u16(h + 32) == 2);
    ok("16 bits",                       rd_u16(h + 34) == 16);
    ok("data size = 2 * nsamples",      rd_u32(h + 40) == 2u * n);
    /* The off-by-eight this comment in wspr_wav.c warns about. */
    ok("RIFF size = 36 + data, not 44", rd_u32(h + 4) == 36u + 2u * n);

    /* ---- 2. refusals - a zero-length WAV must never be written ---- */
    ok("nsamples 0 refused",  wspr_wav_header(h, 0, 12000) == 0);
    ok("rate 0 refused",      wspr_wav_header(h, n, 0) == 0);
    ok("NULL refused",        wspr_wav_header(NULL, n, 12000) == 0);

    /* ---- 3. EXTERNAL VECTORS: WSJT's own reference recordings ---- */

    /* 3a. The WSPR reference - a clean canonical file, 120 s at 12 kHz, i.e.
     * exactly the window we dump. Its first 44 bytes must equal ours BYTE FOR
     * BYTE; anything else means wsprd is entitled to treat our file
     * differently from the ones it was written against. */
    const char *wref = "test/wav_reference/wspr/150426_0918.wav";
    FILE *f = fopen(wref, "rb");
    if (!f) {
        printf("*** could not open %s - run from the repo root ***\n", wref);
        fails++;
    } else {
        uint8_t theirs[WSPR_WAV_HDR_BYTES];
        size_t rd = fread(theirs, 1, sizeof(theirs), f);
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fclose(f);
        ok("WSPR ref: read 44 bytes", rd == WSPR_WAV_HDR_BYTES);
        ok("WSPR ref: 120 s at 12 kHz",
           rd_u32(theirs + 24) == 12000 && rd_u32(theirs + 40) == 2u * n);
        wspr_wav_header(h, n, 12000);
        int same = memcmp(h, theirs, WSPR_WAV_HDR_BYTES) == 0;
        ok("WSPR ref: byte-identical to ours", same);
        if (!same)
            for (int i = 0; i < WSPR_WAV_HDR_BYTES; i++)
                if (h[i] != theirs[i])
                    printf("      [%2d] ours 0x%02X  theirs 0x%02X\n",
                           i, h[i], theirs[i]);
        /* Validates the VECTOR, not just our use of it: a canonical file with
         * no trailing chunks has RIFF size = filesize - 8. */
        ok("WSPR ref: RIFF size = filesize - 8",
           (long)rd_u32(theirs + 4) == fsize - 8);
    }

    /* 3b. An FT8 reference, which carries a trailing LIST chunk after the
     * audio. Pinned deliberately, because it is the one legitimate way a real
     * file RIFF size DISAGREES with ours: the field counts the trailing chunk
     * too. Our dumps have no trailing chunks, so 36 + data is right for us -
     * and this case is why a byte-compare against an arbitrary WAV is the
     * WRONG test. This harness failed on it first, which is how it was found. */
    const char *fref = "test/wav_reference/191111_110115.wav";
    f = fopen(fref, "rb");
    if (!f) {
        printf("      (skipped 3b - %s absent)\n", fref);
    } else {
        uint8_t theirs[WSPR_WAV_HDR_BYTES];
        if (fread(theirs, 1, sizeof(theirs), f) == sizeof(theirs)) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            uint32_t data = rd_u32(theirs + 40);
            uint32_t their_n = data / 2u;
            wspr_wav_header(h, their_n, rd_u32(theirs + 24));
            ok("FT8 ref: all bytes but the RIFF size identical",
               memcmp(h, theirs, 4) == 0 &&
               memcmp(h + 8, theirs + 8, WSPR_WAV_HDR_BYTES - 8) == 0);
            ok("FT8 ref: its RIFF size counts the trailing chunk",
               (long)rd_u32(theirs + 4) == fsize - 8 &&
               rd_u32(theirs + 4) > 36u + data);
        }
        fclose(f);
    }

    /* ---- 4. filenames in WSJT-X's convention ---- */
    char nm[32];
    /* 2019-11-11 11:00:00 UTC = 1573470000 */
    wspr_wav_filename(nm, sizeof(nm), 1573470000LL);
    ok("filename 191111_1100.wav", strcmp(nm, "191111_1100.wav") == 0);
    /* Century roll: 2000-01-01 00:00:00 UTC = 946684800 -> year "00" */
    wspr_wav_filename(nm, sizeof(nm), 946684800LL);
    ok("filename 000101_0000.wav", strcmp(nm, "000101_0000.wav") == 0);
    ok("NULL out refused",  wspr_wav_filename(NULL, 32, 1573470000LL) == 0);
    ok("zero n refused",    wspr_wav_filename(nm, 0, 1573470000LL) == 0);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
