/* Host test for the JS8 decode path in decode.c.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -I components/ft8_lib/fft -o js8_decode_harness \
 *       test/js8_decode_harness.c components/ft8_lib/ft8/decode.c \
 *       components/ft8_lib/ft8/ldpc.c components/ft8_lib/ft8/constants.c \
 *       components/ft8_lib/ft8/crc.c components/ft8_lib/ft8/message.c \
 *       components/ft8_lib/ft8/text.c components/ft8_lib/ft8/encode.c \
 *       components/ft8_lib/ft8/js8_codec.c components/ft8_lib/ft8/js8_tables.c \
 *       components/ft8_lib/ft8/js8_message.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -lm && ./js8_decode_harness
 *
 * WHY A SYNTHETIC WATERFALL. The decoder reads magnitudes per symbol per tone;
 * it never sees audio. So a transmitted frame can be put in front of it
 * directly - build the 79-symbol tone sequence the way genjs8.f90 does, write
 * an ideal magnitude for each, and call ftx_decode_candidate() with
 * FTX_PROTOCOL_JS8. That exercises the REAL decode path - symbol layout, the
 * absent Gray map, LDPC(174,87), the colorder permutation, CRC-12 and payload
 * packing - rather than a copy of it in the test.
 *
 * ⛔ WHAT IT STILL CANNOT PROVE. The signal is one this code produced. Every
 * agreement here is this implementation agreeing with itself, and a shared
 * misreading of JS8Call's source would pass every check below. The LDPC was
 * checked against JS8Call's own Fortran in J1 and the message layer against
 * hand-computed values in J2, so the pieces are not unexamined - but the chain
 * as a whole is settled only by a real JS8Call station decoding our frame.
 * NEVER by a second Tab5: two copies share their mistakes exactly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decode.h"
#include "js8.h"
#include "js8_message.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

#define NUM_BLOCKS   (JS8_NN + 8)   /* a little slack either side */
#define NUM_BINS     8
#define TIME_OSR     1
#define FREQ_OSR     1

static uint8_t g_mag[NUM_BLOCKS * NUM_BINS];

/* The 79 channel symbols, exactly as genjs8.f90 lays them out: Costas at 0, 36
 * and 72 (0-based), data at 7..35 and 43..71, and the tone is the plain binary
 * value of three codeword bits - NO Gray map. */
static void tones_from_codeword(const uint8_t cw[JS8_LDPC_N], uint8_t itone[JS8_NN])
{
    for (int i = 0; i < 7; i++)
    {
        itone[i]      = kJS8_Costas_pattern[i];
        itone[36 + i] = kJS8_Costas_pattern[i];
        itone[72 + i] = kJS8_Costas_pattern[i];
    }
    int k = 6;                       /* genjs8's k starts at 7, 1-based */
    for (int j = 0; j < JS8_ND; j++)
    {
        int i = 3 * j;
        k++;
        if (j == 29) k += 7;         /* step over the middle sync block */
        itone[k] = (uint8_t)(cw[i] * 4 + cw[i + 1] * 2 + cw[i + 2]);
    }
}

/* An ideal signal: full magnitude on the transmitted tone, a floor elsewhere.
 * `offset` is where the frame starts in the block grid, so the candidate's
 * time_offset is exercised rather than assumed to be zero. */
static void paint(const uint8_t itone[JS8_NN], int offset, uint8_t floor_mag, uint8_t peak_mag)
{
    memset(g_mag, floor_mag, sizeof(g_mag));
    for (int s = 0; s < JS8_NN; s++)
    {
        g_mag[(offset + s) * NUM_BINS + itone[s]] = peak_mag;
    }
}

static void make_wf(ftx_waterfall_t* wf)
{
    memset(wf, 0, sizeof(*wf));
    wf->max_blocks   = NUM_BLOCKS;
    wf->num_blocks   = NUM_BLOCKS;
    wf->num_bins     = NUM_BINS;
    wf->time_osr     = TIME_OSR;
    wf->freq_osr     = FREQ_OSR;
    wf->block_stride = TIME_OSR * FREQ_OSR * NUM_BINS;
    wf->mag          = g_mag;
    wf->protocol     = FTX_PROTOCOL_JS8;
}

/* Build a frame, put it on the synthetic waterfall, and decode it back. */
static bool round_trip(const js8_directed_t* in, js8_directed_t* out, int offset,
                       int flip_symbols)
{
    uint8_t frame[JS8_FRAME_BYTES];
    if (!js8_pack_directed(in, frame)) return false;

    uint8_t payload[JS8_LDPC_K], cw[JS8_LDPC_N], itone[JS8_NN];
    js8_pack_payload(frame, 0, payload);
    js8_encode174(payload, cw);
    tones_from_codeword(cw, itone);

    /* Corrupt whole SYMBOLS, which is what fading does - not individual bits.
     * Three bits go wrong together each time, so this is a harder load than
     * the bit flips the codec harnesses use. */
    unsigned seed = 9;
    for (int i = 0; i < flip_symbols; i++)
    {
        seed = seed * 1103515245u + 12345u;
        int s = 7 + (int)((seed >> 16) % 29);        /* inside the first data block */
        seed = seed * 1103515245u + 12345u;
        itone[s] = (uint8_t)((itone[s] + 1 + (seed >> 16) % 7) % 8);
    }

    paint(itone, offset, 10, 250);

    ftx_waterfall_t wf;
    make_wf(&wf);

    ftx_candidate_t cand = { 0 };
    cand.time_offset = (int16_t)offset;
    cand.freq_offset = 0;
    cand.time_sub    = 0;
    cand.freq_sub    = 0;
    cand.score       = 100;

    ftx_message_t      msg;
    ftx_decode_status_t st;
    memset(&msg, 0, sizeof(msg));
    memset(&st, 0, sizeof(st));
    if (!ftx_decode_candidate(&wf, &cand, 30, &msg, &st)) return false;

    /* The decoder hands back the 75 message bits packed MSB first; the first
     * 72 are the frame. */
    uint8_t got[JS8_FRAME_BYTES];
    memcpy(got, msg.payload, JS8_FRAME_BYTES);
    return js8_unpack_directed(got, out);
}

static void test_clean_decode(void)
{
    js8_directed_t in = { 0 }, out;
    snprintf(in.from, sizeof(in.from), "OZ1LAV");
    snprintf(in.to, sizeof(in.to), "K1ABC");
    in.cmd = JS8_CMD_SNR;
    in.num = -12;

    CHECK(round_trip(&in, &out, 2, 0), "a clean JS8 frame should decode\n");
    CHECK(strcmp(out.from, "OZ1LAV") == 0 && strcmp(out.to, "K1ABC") == 0 &&
          out.cmd == JS8_CMD_SNR && out.num == -12,
          "decoded %s -> %s %s %d\n", out.from, out.to,
          js8_cmd_text(out.cmd), out.num);
}

/* The frame does not start at block 0 on a real capture, and a decoder that
 * only works at offset 0 passes every lazy test and nothing on the air. */
static void test_time_offsets(void)
{
    js8_directed_t in = { 0 }, out;
    snprintf(in.from, sizeof(in.from), "W7STF");
    snprintf(in.to, sizeof(in.to), "OZ1LAV");
    in.cmd = JS8_CMD_73;
    in.num = JS8_NUM_NONE;

    for (int off = 0; off <= 6; off += 3)
    {
        CHECK(round_trip(&in, &out, off, 0), "offset %d should decode\n", off);
        CHECK(strcmp(out.from, "W7STF") == 0 && out.cmd == JS8_CMD_73,
              "offset %d gave %s %s\n", off, out.from, js8_cmd_text(out.cmd));
    }
}

/* Three corrupted symbols is nine wrong bits, in a rate-1/2 code. The point is
 * not the exact number - it is that the LDPC is actually correcting, rather
 * than the test only ever handing it a perfect codeword. */
static void test_corrupted_symbols(void)
{
    js8_directed_t in = { 0 }, out;
    snprintf(in.from, sizeof(in.from), "OZ1LAV");
    snprintf(in.to, sizeof(in.to), "W7STF");
    in.cmd = JS8_CMD_RR;
    in.num = JS8_NUM_NONE;

    CHECK(round_trip(&in, &out, 2, 3), "3 corrupted symbols should still decode\n");
    CHECK(strcmp(out.from, "OZ1LAV") == 0 && out.cmd == JS8_CMD_RR,
          "corrupted decode gave %s %s\n", out.from, js8_cmd_text(out.cmd));
}

/* A CQ uses the OTHER frame type and the other callsign codec, so it has to be
 * carried through the decoder separately. */
static void test_heartbeat(void)
{
    js8_heartbeat_t hb = { 0 };
    snprintf(hb.call, sizeof(hb.call), "OZ1LAV");
    snprintf(hb.grid, sizeof(hb.grid), "JO65");
    hb.is_cq = true;

    uint8_t frame[JS8_FRAME_BYTES], payload[JS8_LDPC_K];
    uint8_t cw[JS8_LDPC_N], itone[JS8_NN];
    CHECK(js8_pack_heartbeat(&hb, frame), "CQ pack\n");
    js8_pack_payload(frame, 0, payload);
    js8_encode174(payload, cw);
    tones_from_codeword(cw, itone);
    paint(itone, 1, 10, 250);

    ftx_waterfall_t wf;
    make_wf(&wf);
    ftx_candidate_t cand = { 0 };
    cand.time_offset = 1;
    cand.score = 100;

    ftx_message_t msg;
    ftx_decode_status_t st;
    memset(&msg, 0, sizeof(msg));
    memset(&st, 0, sizeof(st));
    CHECK(ftx_decode_candidate(&wf, &cand, 30, &msg, &st), "CQ should decode\n");

    uint8_t got[JS8_FRAME_BYTES];
    memcpy(got, msg.payload, JS8_FRAME_BYTES);
    CHECK(js8_frame_type(got) == JS8_FRAME_HEARTBEAT,
          "frame type %d, expected 0\n", (int)js8_frame_type(got));

    js8_heartbeat_t out;
    CHECK(js8_unpack_heartbeat(got, &out), "CQ unpack\n");
    CHECK(strcmp(out.call, "OZ1LAV") == 0 && strcmp(out.grid, "JO65") == 0 && out.is_cq,
          "CQ decoded as %s %s cq=%d\n", out.call, out.grid, (int)out.is_cq);
}

/* ⛔ NOISE MUST NOT DECODE. A decoder that accepts anything is worse than one
 * that accepts nothing: every slot would fill with invented callsigns. The
 * CRC-12 is what stops it, and this is the only test that exercises the
 * rejection rather than the acceptance. */
static void test_noise_is_rejected(void)
{
    unsigned seed = 4242;
    int decoded = 0;
    for (int t = 0; t < 50; t++)
    {
        uint8_t itone[JS8_NN];
        for (int i = 0; i < JS8_NN; i++)
        {
            seed = seed * 1103515245u + 12345u;
            itone[i] = (uint8_t)((seed >> 16) % 8);
        }
        paint(itone, 2, 10, 250);

        ftx_waterfall_t wf;
        make_wf(&wf);
        ftx_candidate_t cand = { 0 };
        cand.time_offset = 2;
        cand.score = 100;

        ftx_message_t msg;
        ftx_decode_status_t st;
        memset(&msg, 0, sizeof(msg));
        memset(&st, 0, sizeof(st));
        if (ftx_decode_candidate(&wf, &cand, 30, &msg, &st)) decoded++;
    }
    printf("  random tone sequences decoded: %d of 50\n", decoded);
    CHECK(decoded == 0, "noise must not decode, got %d\n", decoded);
}

/* The sync score has to prefer JS8's Costas array over FT8's, or the candidate
 * search never puts a JS8 signal in front of the decoder in the first place. */
static void test_sync_prefers_js8_costas(void)
{
    uint8_t itone[JS8_NN];
    memset(itone, 0, sizeof(itone));
    for (int i = 0; i < 7; i++)
    {
        itone[i] = kJS8_Costas_pattern[i];
        itone[36 + i] = kJS8_Costas_pattern[i];
        itone[72 + i] = kJS8_Costas_pattern[i];
    }
    paint(itone, 2, 10, 250);

    ftx_waterfall_t wf;
    make_wf(&wf);

    ftx_candidate_t cands[8];
    int n = ftx_find_candidates(&wf, 8, cands, 0);
    CHECK(n > 0, "a JS8 sync pattern should produce at least one candidate\n");

    bool at_two = false;
    for (int i = 0; i < n; i++)
    {
        if (cands[i].time_offset == 2 && cands[i].freq_offset == 0) at_two = true;
    }
    CHECK(at_two, "the candidate at the planted offset should be found\n");

    /* The same waterfall read as FT8 must score it lower - different Costas.
     * If this ever passes with the two equal, the sync branch is not wired. */
    wf.protocol = FTX_PROTOCOL_FT8;
    ftx_candidate_t ft8_cands[8];
    int n8 = ftx_find_candidates(&wf, 8, ft8_cands, 0);
    int js8_best = (n > 0) ? cands[0].score : -32768;
    int ft8_best = (n8 > 0) ? ft8_cands[0].score : -32768;
    printf("  sync score at the planted frame: JS8 %d, FT8 %d\n", js8_best, ft8_best);
    CHECK(js8_best > ft8_best,
          "JS8 sync should score the JS8 pattern above FT8's reading of it\n");
}

int main(void)
{
    printf("JS8 decode path\n");
    test_clean_decode();
    test_time_offsets();
    test_corrupted_symbols();
    test_heartbeat();
    test_noise_is_rejected();
    test_sync_prefers_js8_costas();

    if (g_fail) { printf("\nFAILED: %d check(s)\n", g_fail); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
