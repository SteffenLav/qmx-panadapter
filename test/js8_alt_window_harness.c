/* Host test: can an ALTERNATE JS8 submode be decoded out of the audio we
 * already captured for the primary submode's slot? (T132)
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -I components/ft8_lib/fft -I components/ft8_lib/common \
 *       -o js8_alt_window_harness test/js8_alt_window_harness.c \
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
 * WHY THIS EXISTS.
 *
 * T132 measured that decoding all four submodes every slot needs 118% of one
 * core, so the affordable shape is to decode ONE alternate submode on a duty
 * cycle. That only works if the alternate can be decoded from the audio the
 * primary slot ALREADY captured - a second capture is not available, because
 * the firmware holds exactly one slot of samples in s_cap_scratch and the next
 * slot overwrites it.
 *
 * The part that can quietly be wrong is the ALIGNMENT. Each submode has its
 * own slot grid and all of them divide 60 s, but they do not line up with each
 * other inside a minute. Feeding a Fast monitor from the wrong offset produces
 * no error of any kind - it produces silence, exactly like a quiet band. That
 * is the failure this harness exists to catch, before any of it is written
 * into ft8_test.c.
 *
 * WHAT IT PROVES, AND WHAT IT DOES NOT.
 *
 * It proves the arithmetic and the decode path on synthesised audio. It is NOT
 * evidence that a real station on Fast or Turbo will be decoded: nothing in
 * this firmware has ever exchanged Fast, Turbo or Slow with a real JS8Call
 * station, and this harness uses our own encoder, so a shared misunderstanding
 * of the physical layer would pass it. See js8_submode_harness.c, which makes
 * the same disclaimer for the same reason.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/js8.h"
#include "ft8/js8_message.h"
#include "ft8/js8_text.h"
#include "common/monitor.h"

#define SR_HZ            12000
#define PRIMARY_SLOT_S   15                       /* JS8 Normal, and FT8 */
#define PRIMARY_SAMPLES  (SR_HZ * PRIMARY_SLOT_S)
#define MAX_CAND         140
#define FIND_MIN_SCORE   5
#define LDPC_MAX_ITERS   30
#define TONE_BASE_HZ     1000.0

static float g_capture[PRIMARY_SAMPLES + 16];
static int   g_fail = 0;

static int nsps_of(ftx_protocol_t p)
{
    return (int)((int64_t)SR_HZ * ftx_protocol_symbol_period_us(p) / 1000000);
}

/* Write one JS8 burst into `dst` at sample offset `at`, leaving the rest as it
 * is. Mirrors synth_slot() in js8_submode_harness.c - same tone base, same
 * spacing from the protocol table, no shaping. */
static void place_burst(float *dst, int at, const uint8_t tones[JS8_NN],
                        int nsps, double spacing_hz)
{
    double phase = 0.0;
    int    n     = 0;
    for (int s = 0; s < JS8_NN; s++) {
        double f = TONE_BASE_HZ + spacing_hz * (double)tones[s];
        for (int i = 0; i < nsps; i++, n++) {
            dst[at + n] += (float)(0.5 * sin(phase));
            phase += 2.0 * M_PI * f / SR_HZ;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
}

/* Decode `n` samples starting at `signal` as `proto`. Returns the number of
 * candidates that decoded, and copies the first decoded text into `out`. This
 * is the same sequence ft8_test.c runs per slot: monitor_process over whole
 * blocks, then ftx_find_candidates, then ftx_decode_candidate. */
static int decode_window(const float *signal, int n, ftx_protocol_t proto,
                         char *out, size_t out_sz)
{
    monitor_t        mon;
    monitor_config_t cfg = {
        .f_min = 200.0f, .f_max = 3000.0f, .sample_rate = SR_HZ,
        .time_osr = 2, .freq_osr = 2, .protocol = proto,
    };
    monitor_init(&mon, &cfg);
    if (!monitor_alloc_ok(&mon)) {
        monitor_free(&mon);
        fprintf(stderr, "  monitor_init failed for %s\n", ftx_protocol_label(proto));
        return -1;
    }
    for (int pos = 0; pos + mon.block_size <= n; pos += mon.block_size)
        monitor_process(&mon, signal + pos);

    ftx_candidate_t cands[MAX_CAND];
    int n_cand = ftx_find_candidates(&mon.wf, MAX_CAND, cands, FIND_MIN_SCORE);
    int hits = 0;
    if (out && out_sz) out[0] = '\0';
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t       msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon.wf, &cands[i], LDPC_MAX_ITERS, &msg, &st))
            continue;
        char text[JS8_TEXT_MAX];
        if (js8_frame_to_text(msg.payload, text, sizeof(text)) && text[0]) {
            if (hits == 0 && out && out_sz) snprintf(out, out_sz, "%s", text);
            hits++;
        }
    }
    monitor_free(&mon);
    return hits;
}

/* ---- the alignment question ------------------------------------------------
 *
 * A primary slot starts at UTC second `t0` (a multiple of 15). An alternate
 * submode with period `alt_s` has its slots at multiples of alt_s. A complete
 * alternate slot lies inside our capture when some multiple of alt_s is in
 * [t0, t0 + 15 - alt_s].
 *
 * This returns how many complete alternate slots the capture holds, and writes
 * their sample offsets into offs[].
 */
static int alt_slots_in_primary(int t0_sec, int alt_s, int *offs, int max_offs)
{
    int n = 0;
    for (int t = ((t0_sec + alt_s - 1) / alt_s) * alt_s;
         t + alt_s <= t0_sec + PRIMARY_SLOT_S; t += alt_s) {
        if (n < max_offs) offs[n] = (t - t0_sec) * SR_HZ;
        n++;
    }
    return n;
}

static void expect_int(const char *what, int got, int want)
{
    const char *verdict = (got == want) ? "ok  " : "FAIL";
    if (got != want) g_fail++;
    printf("  %s %-56s got %d, want %d\n", verdict, what, got, want);
}

/* One case: put a burst of `alt` into the primary capture at `off`, then
 * decode that window as `alt` and check the text comes back. */
static void round_trip_at(ftx_protocol_t alt, int off, const char *text)
{
    memset(g_capture, 0, sizeof(g_capture));

    uint8_t frame[9], itype = 0;
    uint8_t tones[JS8_NN];
    if (!js8_text_to_frame(text, frame, &itype)) {
        printf("  FAIL %-56s could not encode\n", text);
        g_fail++;
        return;
    }
    js8_encode(frame, itype, alt, tones);

    int nsps = nsps_of(alt);
    int need = JS8_NN * nsps;
    int alt_samples = ftx_protocol_slot_ms(alt) * (SR_HZ / 1000);
    if (off + alt_samples > PRIMARY_SAMPLES) {
        printf("  FAIL %-56s window runs past the capture\n",
               ftx_protocol_label(alt));
        g_fail++;
        return;
    }
    /* Lead-in of the submode's own start delay, INSIDE its slot - the same
     * thing js8_submode_harness.c does, and for the same reason: a burst that
     * begins at sample 0 leaves the sync scorer nothing to the left of the
     * first Costas block, which is a harder case than anything on the air. */
    int lead = SR_HZ * ftx_protocol_start_delay_ms(alt) / 1000;
    if (lead + JS8_NN * nsps > alt_samples) lead = 0;
    place_burst(g_capture, off + lead, tones, nsps,
                ftx_protocol_tone_spacing_hz(alt));

    char got[64];
    int hits = decode_window(g_capture + off, alt_samples, alt, got, sizeof(got));

    char label[80];
    snprintf(label, sizeof(label), "%s at +%d.%03d s decodes",
             ftx_protocol_label(alt), off / SR_HZ,
             (off % SR_HZ) * 1000 / SR_HZ);
    if (hits > 0 && strcmp(got, text) == 0) {
        printf("  ok   %-56s \"%s\"\n", label, got);
    } else {
        printf("  FAIL %-56s hits=%d got=\"%s\"\n", label, hits, got);
        g_fail++;
    }
    (void)need;
}

int main(void)
{
    /* The same message js8_submode_harness.c uses. Deliberate: if the text
     * itself cannot encode, this harness would report an alignment failure
     * for a reason that has nothing to do with alignment. */
    const char *TXT = "N2VGU OZ1LAV -07";

    printf("T132: alternate-submode decode out of the primary slot's capture\n");
    printf("primary slot %d s (%d samples at %d Hz)\n\n",
           PRIMARY_SLOT_S, PRIMARY_SAMPLES, SR_HZ);

    /* ---- 1. how many complete alternate slots each primary slot holds ----
     *
     * The four primary slots of a minute are NOT equivalent, which is the
     * whole trap: Fast fits in two of them and not the other two. */
    printf("complete alternate slots inside each primary slot:\n");
    int offs[8];
    for (int t0 = 0; t0 < 60; t0 += PRIMARY_SLOT_S) {
        int nf = alt_slots_in_primary(t0, 10, offs, 8);
        int nt = alt_slots_in_primary(t0, 6, offs, 8);
        int ns = alt_slots_in_primary(t0, 30, offs, 8);
        printf("    t0=%2d s   Fast %d   Turbo %d   Slow %d\n", t0, nf, nt, ns);
    }
    printf("\n");

    /* Fast (10 s) gives exactly one complete slot in EVERY primary slot.
     *
     * ⚠ I first wrote this expecting 0 at t0=15 and t0=45, reasoning that
     * the Fast slots there are [10,20) and [20,30) and that neither is
     * contained in [15,30). The second half of that is simply false - [20,30)
     * is contained in [15,30) - and the harness caught it on the first run.
     * The wrong version is the intuitive one, which is why it is written down
     * here rather than quietly corrected. */
    for (int t0 = 0; t0 < 60; t0 += PRIMARY_SLOT_S) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Fast slots inside primary t0=%d", t0);
        expect_int(lbl, alt_slots_in_primary(t0, 10, offs, 8), 1);
    }

    /* Turbo (6 s) gives exactly two in every primary slot. */
    for (int t0 = 0; t0 < 60; t0 += PRIMARY_SLOT_S) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Turbo slots inside primary t0=%d", t0);
        expect_int(lbl, alt_slots_in_primary(t0, 6, offs, 8), 2);
    }

    /* Slow (30 s) never fits in a 15 s capture. This is not a limitation to
     * work around - it is arithmetic, and it is why Slow is excluded from the
     * duty-cycled decode rather than being added later. */
    for (int t0 = 0; t0 < 60; t0 += PRIMARY_SLOT_S) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Slow slots inside primary t0=%d (never fits)", t0);
        expect_int(lbl, alt_slots_in_primary(t0, 30, offs, 8), 0);
    }
    printf("\n");

    /* ---- 2. the decode itself, at every offset the table above allows ---- */
    printf("decoding a burst placed at each allowed offset:\n");

    for (int t0 = 0; t0 < 60; t0 += PRIMARY_SLOT_S) {
        alt_slots_in_primary(t0, 10, offs, 8);
        round_trip_at(FTX_PROTOCOL_JS8_FAST, offs[0], TXT);      /* Fast */
    }

    int nt = alt_slots_in_primary(0, 6, offs, 8);
    for (int i = 0; i < nt; i++)
        round_trip_at(FTX_PROTOCOL_JS8_TURBO, offs[i], TXT);      /* Turbo, t0=0 */

    nt = alt_slots_in_primary(15, 6, offs, 8);
    for (int i = 0; i < nt; i++)
        round_trip_at(FTX_PROTOCOL_JS8_TURBO, offs[i], TXT);      /* Turbo, t0=15 */

    /* ---- 3. MUTATION TEST ------------------------------------------------
     *
     * Everything above passed on the first clean run, which is exactly when a
     * harness is least trustworthy: a decode_window() that found the burst
     * whatever it was given, or an alt_slots_in_primary() that always returned
     * a plausible number, would look identical from here. So break the
     * alignment on purpose and require a FAILURE.
     *
     * The burst goes in at the right offset and is then decoded from a window
     * shifted by HALF the alternate slot, so it hangs off the window edge. A
     * small shift would not do: the decoder searches time offsets inside the
     * window by design, and tolerating a little slip is the point of that. */
    printf("\nmutation test - a misaligned window must NOT decode:\n");
    for (int k = 0; k < 2; k++) {
        ftx_protocol_t alt = k ? FTX_PROTOCOL_JS8_TURBO : FTX_PROTOCOL_JS8_FAST;
        int alt_samples = ftx_protocol_slot_ms(alt) * (SR_HZ / 1000);
        int nsps = nsps_of(alt);

        memset(g_capture, 0, sizeof(g_capture));
        uint8_t frame[9], itype = 0;
        uint8_t tones[JS8_NN];
        if (!js8_text_to_frame(TXT, frame, &itype)) {
            printf("  FAIL %-56s could not encode\n", ftx_protocol_label(alt));
            g_fail++;
            continue;
        }
        js8_encode(frame, itype, alt, tones);
        /* Burst at the START of the capture, window taken half a slot LATER.
         * ⚠ The other way round - burst at +alt_samples, window at the start -
         * segfaulted on the first run: a Fast burst is 79 x 1200 = 94800
         * samples, so placing it at +120000 ran 34800 past the 180000-sample
         * capture. The buffer is the primary slot's, not the alternate's. */
        place_burst(g_capture, 0, tones, nsps,
                    ftx_protocol_tone_spacing_hz(alt));

        int  bad = alt_samples / 2;      /* half a slot late */
        char got[JS8_TEXT_MAX];
        int  hits = decode_window(g_capture + bad, alt_samples, alt,
                                  got, sizeof(got));
        if (hits > 0) {
            printf("  FAIL %-56s decoded \"%s\" from a window it should not\n",
                   ftx_protocol_label(alt), got);
            g_fail++;
        } else {
            printf("  ok   %-56s silent, as it must be\n",
                   ftx_protocol_label(alt));
        }
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
