/* Host test: every JS8 submode round-trips, and the submodes are DISTINCT.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -I components/ft8_lib/fft -I components/ft8_lib/common \
 *       -o js8_submode_harness test/js8_submode_harness.c \
 *       components/ft8_lib/ft8/decode.c components/ft8_lib/ft8/ldpc.c \
 *       components/ft8_lib/ft8/constants.c components/ft8_lib/ft8/crc.c \
 *       components/ft8_lib/ft8/message.c components/ft8_lib/ft8/text.c \
 *       components/ft8_lib/ft8/encode.c components/ft8_lib/ft8/js8_codec.c \
 *       components/ft8_lib/ft8/js8_tables.c \
 *       components/ft8_lib/ft8/js8_message.c \
 *       components/ft8_lib/ft8/js8_text.c \
 *       components/ft8_lib/ft8/js8_jsc.c \
 *       components/ft8_lib/ft8/js8_jsc_tables.c \
 *       components/ft8_lib/common/monitor.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -lm
 *
 * Run:
 *   ./js8_submode_harness
 *
 * WHY THIS EXISTS, AND WHAT IT IS NOT EVIDENCE FOR.
 *
 * JS8 Fast, Turbo and Slow are each a different physical layer: NSPS, slot
 * period, tone spacing and - the one that is easy to miss - THREE DIFFERENT
 * Costas sync arrays instead of one array used three times. Get any of those
 * wrong and the frame still encodes, the radio still transmits, and nothing
 * anywhere ever decodes it. There is no error message for that failure.
 *
 * So this harness does two things:
 *
 *   1. ROUND TRIP. For each submode: text -> frame -> tones -> synthesised
 *      audio at that submode's rate -> decoder configured for that submode ->
 *      the same text back.
 *
 *   2. CROSS-REJECT, which is the half that actually has teeth. A signal
 *      synthesised for submode X must NOT decode when the receiver is set to
 *      submode Y. Without this, a build where every submode silently fell back
 *      to Normal's parameters would pass test 1 four times over and look
 *      perfect. The cross-reject matrix is what proves the per-submode numbers
 *      are reaching the physical layer at all.
 *
 * THIS IS OUR OWN SIGNAL AT BOTH ENDS. js8_encode() is the transmitter's
 * encoder and the decoder here is ours. A pass proves the two agree with each
 * other and that the submodes are distinguishable - it proves NOTHING about
 * whether JS8Call can hear us, or we it, in any submode but Normal (which was
 * settled on air, 2026-10-08). Do not cite a pass here as interop.
 *
 * The submode parameters come from JS8Call 2.3.1 (tag v2.3.1): commons.h and
 * lib/js8/js8[abce]_params.f90, cross-checked against JS8Submode.cpp. The
 * Costas arrays are genjs8.f90's icos7a/b/c.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/js8.h"
#include "ft8/js8_message.h"
#include "ft8/js8_text.h"
#include "common/monitor.h"

#define SR_HZ            12000
#define MAX_CAND         100
#define FIND_MIN_SCORE   10
#define LDPC_MAX_ITERS   25
/* The longest slot any submode uses (Slow, 30 s), so one buffer serves all. */
#define MAX_SLOT_SAMPLES (SR_HZ * 30)

static float g_signal[MAX_SLOT_SAMPLES + 16];

static const ftx_protocol_t kSubmodes[] = {
    FTX_PROTOCOL_JS8,
    FTX_PROTOCOL_JS8_FAST,
    FTX_PROTOCOL_JS8_TURBO,
    FTX_PROTOCOL_JS8_SLOW,
};
#define NSUB ((int)(sizeof(kSubmodes) / sizeof(kSubmodes[0])))

static int g_fail = 0;

/* Samples per symbol for a protocol, from its symbol period. The decoder
 * derives the same number in monitor_init(); deriving it the same way here
 * rather than hardcoding a table is deliberate - a second table could drift,
 * and then the harness would be testing its own copy of the parameters. */
static int nsps_of(ftx_protocol_t p)
{
    return (int)((int64_t)SR_HZ * ftx_protocol_symbol_period_us(p) / 1000000);
}

static int slot_samples_of(ftx_protocol_t p)
{
    return SR_HZ * ftx_protocol_slot_ms(p) / 1000;
}

/* Plain continuous-phase FSK at the submode's own rate. No GFSK shaping and
 * no noise: the question is whether a CLEAN signal is heard at all, and
 * anything softer makes a failure ambiguous about which end is at fault. */
static void synth_slot(float *signal, int n_samples, const uint8_t tones[JS8_NN],
                       int nsps, double base_hz, double spacing_hz, int t0)
{
    for (int i = 0; i < n_samples; i++) signal[i] = 0.0f;
    double phase = 0.0;
    for (int s = 0; s < JS8_NN; s++) {
        double f  = base_hz + spacing_hz * (double)tones[s];
        double dp = 2.0 * M_PI * f / (double)SR_HZ;
        for (int k = 0; k < nsps; k++) {
            int idx = t0 + s * nsps + k;
            if (idx >= n_samples) break;
            signal[idx] = 0.25f * (float)sin(phase);
            phase += dp;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
}

/* Decode `signal` with the receiver configured for `rx_proto`. Returns the
 * number of frames that rendered to text, and copies the first into `out`. */
static int decode_as(const float *signal, int n, ftx_protocol_t rx_proto,
                     char *out, size_t out_len)
{
    monitor_t mon;
    monitor_config_t cfg = {
        .f_min       = 200,
        .f_max       = 3000,
        .sample_rate = SR_HZ,
        .time_osr    = 2,
        .freq_osr    = 2,
        .protocol    = rx_proto,
    };
    monitor_init(&mon, &cfg);
    if (!monitor_alloc_ok(&mon)) {
        fprintf(stderr, "  monitor_init could not allocate for %s\n",
                ftx_protocol_name(rx_proto));
        monitor_free(&mon);
        return -1;
    }
    for (int pos = 0; pos + mon.block_size <= n; pos += mon.block_size)
        monitor_process(&mon, signal + pos);

    ftx_candidate_t cands[MAX_CAND];
    int n_cand = ftx_find_candidates(&mon.wf, MAX_CAND, cands, FIND_MIN_SCORE);

    int got = 0;
    if (out && out_len) out[0] = 0;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon.wf, &cands[i], LDPC_MAX_ITERS, &msg, &st))
            continue;
        char text[JS8_TEXT_MAX];
        if (!js8_frame_to_text(msg.payload, text, sizeof(text))) continue;
        if (got == 0 && out && out_len) snprintf(out, out_len, "%s", text);
        got++;
    }
    monitor_free(&mon);
    return got;
}

/* Build one submode's slot into g_signal. Returns the sample count. */
static int build_slot(ftx_protocol_t tx_proto, const char *text)
{
    uint8_t frame[9], itype = 0;
    if (!js8_text_to_frame(text, frame, &itype)) {
        printf("  js8_text_to_frame failed on '%s'\n", text);
        return -1;
    }
    uint8_t tones[JS8_NN];
    js8_encode(frame, itype, tx_proto, tones);

    int nsps = nsps_of(tx_proto);
    int n    = slot_samples_of(tx_proto);
    /* Lead-in of the submode's own start delay, so the signal does not begin
     * in the very first STFT block. Any offset inside the slot works - the
     * decoder searches time offsets - but starting at sample 0 leaves the sync
     * scorer nothing to the left of the first Costas block. */
    int t0   = SR_HZ * ftx_protocol_start_delay_ms(tx_proto) / 1000;
    if (t0 + JS8_NN * nsps > n) t0 = 0;
    synth_slot(g_signal, n, tones, nsps,
               1000.0, ftx_protocol_tone_spacing_hz(tx_proto), t0);
    return n;
}

int main(void)
{
    const char *text = "N2VGU OZ1LAV -07";

    printf("JS8 submode parameters (from JS8Call 2.3.1):\n");
    printf("  %-10s %6s %8s %10s %10s  costas\n",
           "submode", "NSPS", "slot ms", "symbol us", "spacing Hz");
    for (int i = 0; i < NSUB; i++) {
        ftx_protocol_t p = kSubmodes[i];
        const uint8_t (*c)[7] = ftx_protocol_js8_costas(p);
        printf("  %-10s %6d %8d %10d %10.3f  %s\n",
               ftx_protocol_name(p), nsps_of(p), ftx_protocol_slot_ms(p),
               ftx_protocol_symbol_period_us(p),
               (double)ftx_protocol_tone_spacing_hz(p),
               (c == kJS8_Costas_original) ? "ORIGINAL" :
               (c == kJS8_Costas_modified) ? "MODIFIED" : "??");
    }

    /* The parameters above are only worth printing if they are not all the
     * same. Every other submode must differ from Normal in BOTH the symbol
     * period and the Costas set, or the rest of this harness is theatre. */
    printf("\n-- parameters are distinct --\n");
    for (int i = 1; i < NSUB; i++) {
        ftx_protocol_t p = kSubmodes[i];
        int same_sym = ftx_protocol_symbol_period_us(p) ==
                       ftx_protocol_symbol_period_us(FTX_PROTOCOL_JS8);
        int same_cos = ftx_protocol_js8_costas(p) ==
                       ftx_protocol_js8_costas(FTX_PROTOCOL_JS8);
        if (same_sym || same_cos) {
            printf("  FAIL %s shares Normal's %s%s%s\n", ftx_protocol_name(p),
                   same_sym ? "symbol period" : "",
                   (same_sym && same_cos) ? " and " : "",
                   same_cos ? "Costas set" : "");
            g_fail++;
        }
    }
    if (!g_fail) printf("  PASS - Fast, Turbo and Slow each differ from Normal\n");

    /* 1. Round trip, each submode to itself. */
    printf("\n-- round trip --\n");
    for (int i = 0; i < NSUB; i++) {
        ftx_protocol_t p = kSubmodes[i];
        int n = build_slot(p, text);
        if (n < 0) { g_fail++; continue; }
        char got[JS8_TEXT_MAX];
        int k = decode_as(g_signal, n, p, got, sizeof(got));
        int ok = (k > 0) && !strcmp(got, text);
        printf("  %-10s %s  (%d decoded, first '%s')\n",
               ftx_protocol_name(p), ok ? "PASS" : "FAIL", k < 0 ? 0 : k, got);
        if (!ok) g_fail++;
    }

    /* 2. Cross-reject. The diagonal is the round trip above; every
     *    off-diagonal cell must decode NOTHING. A cell that decodes is a
     *    submode whose parameters are not actually being applied. */
    printf("\n-- cross-reject (rows transmitted, columns received) --\n");
    printf("  %-10s", "tx / rx");
    for (int j = 0; j < NSUB; j++) printf(" %10s", ftx_protocol_name(kSubmodes[j]));
    printf("\n");
    for (int i = 0; i < NSUB; i++) {
        ftx_protocol_t tx = kSubmodes[i];
        int n = build_slot(tx, text);
        if (n < 0) { g_fail++; continue; }
        printf("  %-10s", ftx_protocol_name(tx));
        for (int j = 0; j < NSUB; j++) {
            ftx_protocol_t rx = kSubmodes[j];
            /* The signal is built once per row, so every column is decoded
             * from that one buffer before moving on. */
            int k = decode_as(g_signal, n, rx, NULL, 0);
            if (k < 0) k = 0;
            printf(" %10d", k);
            if (i == j && k == 0) g_fail++;          /* must hear itself  */
            if (i != j && k >  0) g_fail++;          /* must not hear it  */
        }
        printf("\n");
    }

    printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
