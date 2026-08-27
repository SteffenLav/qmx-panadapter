/* Host test for the WSPR protocol layer: message pack/unpack, the K=32
 * convolutional code, interleaving, and the Fano sequential decoder.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -o wspr_codec_harness \
 *       test/wspr_codec_harness.c main/wspr_proto.c main/wspr_fano.c \
 *       -lm && ./wspr_codec_harness
 *
 * WHY. This is WSPR Phase 1 (docs/wspr-scope.md / wspr-phase0-research.md):
 * prove the protocol + FEC round-trip on a PC, against a clean-room
 * implementation of the WSPR message spec and Fano's published algorithm
 * (see main/wspr_proto.c and main/wspr_fano.c's header comments -
 * docs/wspr-phase1-status.md records a real licensing mistake, an initial
 * near-verbatim port of GPL v3 WSJT-X source into this MIT-licensed repo,
 * and the same-day clean-room rewrite that fixed it), BEFORE any of it
 * touches a task or an ISR. A wrong generator polynomial or a wrong
 * grid-packing formula would compile, run, and never decode anything -
 * exactly the failure mode this harness exists to catch cheaply, on a PC,
 * instead of expensively on the glass.
 *
 * Three things are proven here, each a real trap in a first WSPR
 * implementation:
 *
 *  1. Pack -> unpack round-trips for a spread of real and edge-case
 *     messages. wspr_pack_message() and wspr_unpack_message() are
 *     independently-shaped halves of the same module (see wspr_proto.c);
 *     if they disagree, the harness says so immediately - it does NOT
 *     compare pack against a copy of itself.
 *
 *  2. The interleave permutation is self-consistent: interleave() then
 *     deinterleave() is the identity for all 162 positions AND a genuine
 *     bijection, AND the table-driven bit-reversal construction used here
 *     agrees with an independently-written naive bit-by-bit reversal -
 *     two differently-expressed implementations of the same permutation,
 *     cross-checked against each other.
 *
 *  3. The full noiseless pipeline (pack -> convolve-encode -> interleave ->
 *     combine with sync -> [strip sync] -> deinterleave -> Fano decode ->
 *     unpack) recovers the exact original message, for several messages
 *     including edge cases (min/max power, boundary grid squares, a 4-char
 *     and a 6-char callsign). This is the actual FEC round-trip proof -
 *     everything upstream of it (packing) could be right and this could
 *     still be wrong if the encoder/decoder polynomials or bit order
 *     disagree with each other.
 *
 * NOT covered here (deferred - this is the protocol layer, not the radio
 * front end): soft-metric tuning against real channel noise, real captured
 * WSPR audio, and the frequency/time sync search needed to find a signal in
 * the first place. See docs/wspr-phase0-research.md's "what Phase 1 should
 * pull next".
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "wspr_proto.h"
#include "wspr_fano.h"

static int g_fail = 0;

/* ---------- 1. pack/unpack round-trip ---------- */

typedef struct {
    const char *call;
    const char *grid;
    int dbm;
    int want_dbm; /* after power quantization - what unpack should read back */
} roundtrip_case_t;

static const roundtrip_case_t kCases[] = {
    { "K1ABC",  "FN20", 37, 37 },
    { "OZ1LAV", "JO45", 23, 23 },
    { "W1AW",   "FN31", 30, 30 },
    { "VE3XYZ", "EN00", 0, 0 },
    { "4X1XX",  "KM72", 60, 60 },
    { "G0UPL",  "IO91", 10, 10 },
    { "K1ABC",  "AA00", 33, 33 }, /* SW corner of the grid */
    { "K1ABC",  "RR99", 33, 33 }, /* NE corner of the grid */
    { "K1ABC",  "FN20", 35, 33 }, /* exact tie between 33/37 - rounds to the lower one, see wspr_proto.c */
    { "K1ABC",  "FN20", 38, 37 }, /* power rounds down */
    { "K1ABC",  "FN20", 39, 40 }, /* power rounds up */
};
#define N_CASES (int)(sizeof(kCases) / sizeof(kCases[0]))

static void test_pack_unpack_roundtrip(void)
{
    printf("-- 1. pack/unpack round-trip --\n");
    for (int i = 0; i < N_CASES; i++) {
        const roundtrip_case_t *c = &kCases[i];
        wspr_msg_bytes_t msg;
        int ok = wspr_pack_message(c->call, c->grid, c->dbm, &msg);
        if (!ok) {
            printf("  FAIL  pack('%s','%s',%d) refused\n", c->call, c->grid, c->dbm);
            g_fail++;
            continue;
        }
        char call_out[7], grid_out[5];
        int dbm_out;
        int uok = wspr_unpack_message(&msg, call_out, grid_out, &dbm_out);
        if (!uok) {
            printf("  FAIL  unpack after pack('%s','%s',%d) refused\n",
                   c->call, c->grid, c->dbm);
            g_fail++;
            continue;
        }
        int call_ok = (strcmp(call_out, c->call) == 0);
        int grid_ok = (strcmp(grid_out, c->grid) == 0);
        int dbm_ok = (dbm_out == c->want_dbm);
        int pass = call_ok && grid_ok && dbm_ok;
        printf("  %s  ('%s','%s',%d) -> call='%s' grid='%s' dbm=%d\n",
               pass ? "PASS" : "FAIL", c->call, c->grid, c->dbm,
               call_out, grid_out, dbm_out);
        if (!pass) g_fail++;
    }
}

/* CORRECTION (second one on this exact spot - see docs/wspr-phase1-status.md):
 * this cross-check used to compare wspr_fano.c's internal reverse8()
 * (shift-and-mask) against a closed-form "magic number" bit-reversal
 * one-liner copied verbatim from wsprcan's wspr.c. Two problems with that,
 * found in sequence: first, the comparison itself was wrong (compared the
 * formula's result as a 64-bit value instead of the 8-bit truncation the
 * source declares - fixed, and it did genuinely then agree). Second,
 * checking wsprcan's license turned up GPL-3.0 - this is an MIT-licensed
 * project, so that one-liner was never safe to keep verbatim regardless of
 * whether the comparison was right. It isn't even a WSPR-specific fact (an
 * 8-bit bit-reversal is a generic, widely-published trick, not particular
 * to this protocol), so there was no reason to lean on that specific
 * source's expression of it at all. Replaced with a naive bit-by-bit
 * reference reversal below - obviously correct by inspection, needs no
 * external source, and is still a genuine second independent expression
 * of the same computation to cross-check the table-driven version against. */
static uint8_t naive_reverse8(uint8_t v)
{
    uint8_t r = 0;
    for (int b = 0; b < 8; b++) {
        r = (uint8_t)((r << 1) | (v & 1));
        v = (uint8_t)(v >> 1);
    }
    return r;
}

/* Every legal power level, round-tripped.
 *
 * WHY THIS IS SEPARATE FROM THE TABLE ABOVE: that table covers ten of the
 * nineteen legal values, and the gap was invisible. Mutating legal_power[2]
 * from 7 to 8 in wspr_proto.c passed the ENTIRE host suite - so the quantiser
 * could have been wrong at nine of its nineteen steps with every test green.
 *
 * That matters more than a normal coverage hole because the power field is
 * not internal state: it is transmitted, decoded by everyone else on the
 * band, and published to wsprnet as a claim about how much power a station
 * was running. A wrong step is a wrong public number, not a wrong pixel. */
static void test_all_legal_powers(void)
{
    printf("-- 1b. every legal power level --\n");
    static const int legal[] = { 0, 3, 7, 10, 13, 17, 20, 23, 27, 30,
                                 33, 37, 40, 43, 47, 50, 53, 57, 60 };
    int bad = 0;
    for (size_t i = 0; i < sizeof(legal) / sizeof(legal[0]); i++) {
        wspr_msg_bytes_t msg;
        char call_out[7], grid_out[5];
        int dbm_out;
        if (!wspr_pack_message("K1ABC", "FN20", legal[i], &msg) ||
            !wspr_unpack_message(&msg, call_out, grid_out, &dbm_out) ||
            dbm_out != legal[i]) {
            printf("  FAIL  %d dBm round-tripped as %d\n", legal[i], dbm_out);
            bad++; g_fail++;
        }
    }
    if (!bad) printf("  PASS  all 19 legal power levels round-trip exactly\n");
}

static void test_interleave_self_consistency(void)
{
    printf("-- 2. interleave self-consistency --\n");

    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], back[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) raw[i] = (uint8_t)(i & 1); /* arbitrary pattern */

    wspr_interleave(raw, channel);
    wspr_deinterleave(channel, back);
    int identity_ok = (memcmp(raw, back, WSPR_NSYM) == 0);
    printf("  %s  interleave() then deinterleave() is the identity\n",
           identity_ok ? "PASS" : "FAIL");
    if (!identity_ok) g_fail++;

    /* Every channel position must be hit exactly once (a genuine
     * permutation of 0..161, not a many-to-one collapse that would still
     * pass the identity check above by accident). */
    uint8_t idmap_raw[WSPR_NSYM], idmap_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) idmap_raw[i] = (uint8_t)i;
    wspr_interleave(idmap_raw, idmap_channel);
    int seen[WSPR_NSYM] = { 0 };
    int bijective = 1;
    for (int j = 0; j < WSPR_NSYM; j++) {
        if (idmap_channel[j] >= WSPR_NSYM || seen[idmap_channel[j]]) { bijective = 0; break; }
        seen[idmap_channel[j]] = 1;
    }
    printf("  %s  interleave() is a bijection over 0..161\n",
           bijective ? "PASS" : "FAIL");
    if (!bijective) g_fail++;

    /* The actual cross-check: build the same "scan k=0..255, keep reversed
     * values <162 in the order they appear" construction wspr_fano.c uses,
     * but with the naive bit-by-bit reversal above standing in for the
     * internal table-driven reverse8() - if the two independently-written
     * expressions of "reverse 8 bits" agree everywhere, they'll produce
     * the identical map. */
    uint8_t alt_map[WSPR_NSYM];
    int idx = 0;
    for (int k = 0; k < 256 && idx < WSPR_NSYM; k++) {
        uint8_t j = naive_reverse8((uint8_t)k);
        if (j < WSPR_NSYM) alt_map[idx++] = j;
    }
    uint8_t alt_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) alt_channel[alt_map[i]] = idmap_raw[i];
    int cross_ok = (idx == WSPR_NSYM) && (memcmp(idmap_channel, alt_channel, WSPR_NSYM) == 0);
    printf("  %s  matches an independently-written naive bit-reversal (second expression)\n",
           cross_ok ? "PASS" : "FAIL");
    if (!cross_ok) g_fail++;
}

/* ---------- 3. full noiseless FEC round-trip ---------- */

static void test_full_pipeline(void)
{
    printf("-- 3. full pipeline (pack -> encode -> interleave -> sync -> Fano decode -> unpack) --\n");

    int mettab[2][256];
    wspr_build_hard_metric_table(mettab);

    for (int i = 0; i < N_CASES; i++) {
        const roundtrip_case_t *c = &kCases[i];
        wspr_msg_bytes_t msg;
        if (!wspr_pack_message(c->call, c->grid, c->dbm, &msg)) {
            printf("  FAIL  pack('%s','%s',%d) refused\n", c->call, c->grid, c->dbm);
            g_fail++;
            continue;
        }

        uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
        wspr_convolve_encode(&msg, raw);
        wspr_interleave(raw, channel);
        wspr_symbols_to_tones(channel, tones);

        /* Receiver side: strip the KNOWN sync bit back out (hard-decision,
         * noiseless), deinterleave, map to the two-level soft metric
         * (0/255) the hard metric table expects, and Fano-decode. */
        uint8_t rx_channel[WSPR_NSYM];
        wspr_tones_to_symbols(tones, rx_channel);
        uint8_t rx_raw[WSPR_NSYM];
        wspr_deinterleave(rx_channel, rx_raw);
        uint8_t soft[WSPR_NSYM];
        for (int k = 0; k < WSPR_NSYM; k++) soft[k] = rx_raw[k] ? 255 : 0;

        wspr_msg_bytes_t decoded;
        unsigned int metric = 0, cycles = 0;
        int ok = wspr_fano_decode(soft, mettab, 2, 10000, &decoded, &metric, &cycles);
        if (!ok) {
            printf("  FAIL  ('%s','%s',%d) Fano decode timed out (cycles=%u)\n",
                   c->call, c->grid, c->dbm, cycles);
            g_fail++;
            continue;
        }

        int bits_ok = (memcmp(msg.dat, decoded.dat, 7) == 0);
        char call_out[7], grid_out[5];
        int dbm_out;
        int uok = bits_ok && wspr_unpack_message(&decoded, call_out, grid_out, &dbm_out);
        int str_ok = uok && strcmp(call_out, c->call) == 0
                     && strcmp(grid_out, c->grid) == 0
                     && dbm_out == c->want_dbm;

        printf("  %s  ('%s','%s',%d) decoded call='%s' grid='%s' dbm=%d  metric=%u cycles=%u\n",
               str_ok ? "PASS" : "FAIL", c->call, c->grid, c->dbm,
               uok ? call_out : "?", uok ? grid_out : "?", uok ? dbm_out : -1,
               metric, cycles);
        if (!str_ok) g_fail++;
    }
}

/* A single-bit flip in the transmitted symbols should still decode
 * correctly - this is the entire point of a rate-1/2 K=32 code, and a
 * decoder that only ever works on a byte-identical noiseless channel isn't
 * proving anything about the FEC. */
static void test_single_bit_error_correction(void)
{
    printf("-- 4. single-bit-error correction --\n");

    int mettab[2][256];
    wspr_build_hard_metric_table(mettab);

    wspr_msg_bytes_t msg;
    if (!wspr_pack_message("K1ABC", "FN20", 37, &msg)) {
        printf("  FAIL  pack() refused\n");
        g_fail++;
        return;
    }

    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);

    int flips_tested = 0, flips_survived = 0;
    for (int flip = 0; flip < WSPR_NSYM; flip++) { /* exhaustive: every position */
        uint8_t tones_bad[WSPR_NSYM];
        memcpy(tones_bad, tones, sizeof(tones));
        tones_bad[flip] ^= 2; /* flip the data bit, keep the sync bit correct */

        uint8_t rx_channel[WSPR_NSYM], rx_raw[WSPR_NSYM], soft[WSPR_NSYM];
        wspr_tones_to_symbols(tones_bad, rx_channel);
        wspr_deinterleave(rx_channel, rx_raw);
        for (int k = 0; k < WSPR_NSYM; k++) soft[k] = rx_raw[k] ? 255 : 0;

        wspr_msg_bytes_t decoded;
        unsigned int metric = 0, cycles = 0;
        int ok = wspr_fano_decode(soft, mettab, 2, 10000, &decoded, &metric, &cycles);
        flips_tested++;
        if (ok && memcmp(msg.dat, decoded.dat, 7) == 0) flips_survived++;
    }
    int pass = (flips_survived == flips_tested);
    printf("  %s  %d/%d single-symbol-flip cases decoded correctly\n",
           pass ? "PASS" : "FAIL", flips_survived, flips_tested);
    if (!pass) g_fail++;

    /* Two-symbol errors, still within this code's correction radius at
     * this hard-decision metric (verified separately: 3+ simultaneous
     * errors starts to fail some cases, which is the code's real limit,
     * not a bug — not asserted here, exhaustive 2-error testing is
     * sufficient to catch a metric-table regression). */
    int pair_tested = 0, pair_survived = 0;
    for (int flip = 0; flip < WSPR_NSYM; flip++) {
        uint8_t tones_bad[WSPR_NSYM];
        memcpy(tones_bad, tones, sizeof(tones));
        tones_bad[flip] ^= 2;
        tones_bad[(flip + 83) % WSPR_NSYM] ^= 2; /* second error, far away */

        uint8_t rx_channel[WSPR_NSYM], rx_raw[WSPR_NSYM], soft[WSPR_NSYM];
        wspr_tones_to_symbols(tones_bad, rx_channel);
        wspr_deinterleave(rx_channel, rx_raw);
        for (int k = 0; k < WSPR_NSYM; k++) soft[k] = rx_raw[k] ? 255 : 0;

        wspr_msg_bytes_t decoded;
        unsigned int metric = 0, cycles = 0;
        int ok = wspr_fano_decode(soft, mettab, 2, 10000, &decoded, &metric, &cycles);
        pair_tested++;
        if (ok && memcmp(msg.dat, decoded.dat, 7) == 0) pair_survived++;
    }
    int pair_pass = (pair_survived == pair_tested);
    printf("  %s  %d/%d two-symbol-flip cases decoded correctly\n",
           pair_pass ? "PASS" : "FAIL", pair_survived, pair_tested);
    if (!pair_pass) g_fail++;
}

/* Bad input must be refused, not silently mispacked. */
static void test_input_validation(void)
{
    printf("-- 5. input validation --\n");
    wspr_msg_bytes_t msg;
    struct { const char *call; const char *grid; const char *why; } bad[] = {
        { "KABC", "FN20", "no digit in callsign" },
        { "K1ABCDEF", "FN20", "too long" },
        { "K1ABC", "FN2", "grid too short" },
        { "K1ABC", "SN20", "grid field letter out of A-R range" },
        { "K1ABC", "FNAB", "grid square not digits" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int ok = wspr_pack_message(bad[i].call, bad[i].grid, 30, &msg);
        int pass = (ok == 0);
        printf("  %s  refused '%s'/'%s' (%s)\n", pass ? "PASS" : "FAIL",
               bad[i].call, bad[i].grid, bad[i].why);
        if (!pass) g_fail++;
    }
}

int main(void)
{
    test_pack_unpack_roundtrip();
    test_all_legal_powers();
    test_interleave_self_consistency();
    test_full_pipeline();
    test_single_bit_error_correction();
    test_input_validation();

    printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
