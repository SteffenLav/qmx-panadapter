/* Host test: run the JS8 decoder over recorded 12 kHz slot audio.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -I components/ft8_lib/fft -I components/ft8_lib/common \
 *       -o js8_wav_harness test/js8_wav_harness.c \
 *       components/ft8_lib/ft8/decode.c components/ft8_lib/ft8/ldpc.c \
 *       components/ft8_lib/ft8/constants.c components/ft8_lib/ft8/crc.c \
 *       components/ft8_lib/ft8/message.c components/ft8_lib/ft8/text.c \
 *       components/ft8_lib/ft8/encode.c components/ft8_lib/ft8/js8_codec.c \
 *       components/ft8_lib/ft8/js8_tables.c \
 *       components/ft8_lib/ft8/js8_message.c \
 *       components/ft8_lib/ft8/js8_text.c \
 *       components/ft8_lib/common/monitor.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -lm
 *
 * Run:
 *   ./js8_wav_harness                        # selftest + the whole corpus
 *   ./js8_wav_harness test/wav_reference_js8 # a corpus directory
 *   ./js8_wav_harness slot.wav slot.txt      # one pair
 *   ./js8_wav_harness --selftest             # the synthetic slot alone
 *
 * WHY THIS EXISTS. js8_decode_harness.c feeds the decoder a waterfall this
 * code built itself, so it can only prove the implementation agrees with
 * itself. This one takes AUDIO - the real thing, off the air, recorded by
 * /api/slot.wav from the decoder's own 12 kHz slot buffer - and so it is the
 * first JS8 test whose input this project did not synthesise.
 *
 * ⛔ WHAT THE .txt IS. The expected set beside each WAV is what the BOARD
 * decoded for that slot (tools/js8_corpus.py). That makes this a REGRESSION
 * test: it fails when a change decodes less than today's firmware does. It is
 * not a sensitivity test and it is not ground truth - a station JS8Call would
 * have decoded and the board did not is recorded as absent, so this harness
 * will happily call that slot a pass. Ground truth needs JS8Call's own decoder
 * over the same WAV, which is J6's business, not this file's.
 *
 * ⭐ Decoding MORE than the .txt is reported, never failed. The corpus is a
 * floor. A run that finds an extra station is the improvement this harness is
 * here to let someone make without fear, so it prints it as EXTRA and leaves
 * the exit code alone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>

#include "decode.h"
#include "monitor.h"
#include "js8.h"
#include "js8_text.h"
#include "js8_message.h"

#define SR_HZ            12000
#define SLOT_SAMPLES     (SR_HZ * 15)
#define MAX_CAND         140          /* FT8_MAX_CANDIDATES on the board */
#define FIND_MIN_SCORE   10
#define LDPC_MAX_ITERS   20
#define MAX_ROWS         64

/* Relative to the repo root, which is where run_harnesses.py runs it from. */
#define DEFAULT_CORPUS   "test/wav_reference_js8"

typedef struct {
    char text[JS8_TEXT_MAX];
    int  hz;
} row_t;

static int g_fail = 0;      /* slots that lost a decode */
/* CRC-valid frames with no renderer, by frame type. */
static int g_unrendered[8];
static int g_slots = 0;
static int g_exp_total = 0, g_got_total = 0, g_match_total = 0, g_extra_total = 0;

/* ---- WAV ------------------------------------------------------------- */

/* Deliberately strict. A corpus WAV that is not exactly 12 kHz mono 16-bit is
 * not a thing to resample and carry on with: the decoder's own buffer is that
 * format by construction, so anything else means the file came from somewhere
 * else - most likely /api/rxaudio.wav, which is 48 kHz and the wrong signal
 * entirely. Saying so beats silently measuring the wrong thing. */
static int load_slot_wav(const char *path, float *dst, int max_samples, int *n_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "  cannot open %s\n", path); return -1; }

    unsigned char h[44];
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) {
        fprintf(stderr, "  %s: shorter than a WAV header\n", path);
        fclose(f); return -1;
    }
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "  %s: not a RIFF/WAVE file\n", path);
        fclose(f); return -1;
    }
    int ch    =  h[22] | (h[23] << 8);
    int rate  =  h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    int bits  =  h[34] | (h[35] << 8);
    if (memcmp(h + 36, "data", 4)) {
        fprintf(stderr, "  %s: unexpected chunk layout (no 'data' at 36)\n", path);
        fclose(f); return -1;
    }
    if (rate != SR_HZ || ch != 1 || bits != 16) {
        fprintf(stderr, "  %s: %d Hz %d ch %d bit - the corpus is %d Hz mono "
                        "16-bit. A 48 kHz file is /api/rxaudio.wav (the codec "
                        "output), NOT the decoder's input.\n",
                path, rate, ch, bits, SR_HZ);
        fclose(f); return -1;
    }

    int n = 0;
    short s;
    /* /32768 - the +/-1.0 convention the 35 FT8 references use, and the one
     * monitor_process() needs: it packs each bin as a CLAMPED `2*db + 240`
     * over -120..0 dB, so a raw int16-magnitude signal saturates the whole
     * waterfall to 255 and decodes nothing. Measured here on 2026-10-09: the
     * synthetic self-test found 0 candidates at amplitude 8000 and decodes at
     * 0.25. The board's slot WAVs are normalised for the same reason. */
    /* JS8_WAV_SCALE re-scales the loaded slot. It exists for ONE measurement:
     * the stored WAV is normalised, so setting this to the slot's own
     * peak_float from /api/slot.json puts the harness at the LIVE level and
     * answers whether the board's own waterfall is clamping. Unset = 1.0.
     *
     * ⭐ THAT MEASUREMENT IS DONE. One real 40 m JS8 slot off bench dev,
     * 2026-10-09, candidates found at each input scale:
     *
     *     scale     1    22   100   300  1000  3000  8000
     *     cand     56    56    56    54    16     3     0
     *
     * The live peak_float was 22. The clamp does not bite until about 300 and
     * does not dominate until 1000, so the live path has 15-45x of margin and
     * the normalised corpus is NOT easier than the board. The 0 at 8000 is
     * what made the first self-test fail and is what put this knob here. */
    const char *sc = getenv("JS8_WAV_SCALE");
    float extra = sc ? (float)atof(sc) : 1.0f;
    while (n < max_samples && fread(&s, sizeof(s), 1, f) == 1)
        dst[n++] = (float)s / 32768.0f * extra;
    fclose(f);
    *n_out = n;
    return 0;
}

/* ---- expected file --------------------------------------------------- */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
}

/* Format written by tools/js8_corpus.py, matching the .txt files in test/wav_reference:
 *     HHMMSS  SNR  DT  FREQ ~  MESSAGE
 * Lines starting with '#' are the provenance header and are skipped. */
static int parse_expected(const char *path, row_t *rows, int max_rows)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;                 /* no .txt: caller reports, not fails */
    int n = 0;
    char line[512];
    while (n < max_rows && fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        const char *tilde = strstr(line, "~");
        if (!tilde) continue;
        const char *msg = tilde + 1;
        while (*msg == ' ' || *msg == '\t') msg++;
        if (!*msg) continue;
        snprintf(rows[n].text, sizeof(rows[n].text), "%s", msg);
        /* The frequency column is parsed but NOT compared: the match is on
         * message text alone, because a 1-2 Hz shift in the candidate search
         * is not a regression and failing on it would make the corpus useless
         * as a floor. Kept so the column is not silently dropped. */
        rows[n].hz = 0;
        n++;
    }
    fclose(f);
    return n;
}

/* ---- the decode, driven exactly as ft8_test.c drives it --------------- */

static int decode_slot(const float *signal, int n, row_t *out, int max_out)
{
    monitor_t mon;
    monitor_config_t cfg = {
        .f_min        = 200,
        .f_max        = 3000,
        .sample_rate  = SR_HZ,
        .time_osr     = 2,
        .freq_osr     = 2,
        .protocol     = FTX_PROTOCOL_JS8,
    };
    monitor_init(&mon, &cfg);
    if (!monitor_alloc_ok(&mon)) {
        fprintf(stderr, "  monitor_init could not allocate\n");
        monitor_free(&mon);
        return -1;
    }

    for (int pos = 0; pos + mon.block_size <= n; pos += mon.block_size)
        monitor_process(&mon, signal + pos);

    ftx_candidate_t cands[MAX_CAND];
    int n_cand = ftx_find_candidates(&mon.wf, MAX_CAND, cands, FIND_MIN_SCORE);
    if (getenv("JS8_WAV_DEBUG"))
        fprintf(stderr, "  [dbg] blk=%d sub=%d bins=%d blocks=%d cand=%d\n",
                mon.block_size, mon.subblock_size, mon.wf.num_bins,
                mon.wf.num_blocks, n_cand);

    int got = 0;
    for (int i = 0; i < n_cand && got < max_out; i++) {
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon.wf, &cands[i], LDPC_MAX_ITERS, &msg, &st))
            continue;
        /* The same seam the firmware uses (decode_msg_to_text, ft8_test.c:1323):
         * js8_decode_candidate leaves the 9-byte varicode frame in payload. A
         * second renderer here would keep passing after the shipped one drifted. */
        /* ⭐ A frame can pass CRC-12 and still have no renderer. js8_frame_type()
         * returns the top 3 bits, so there are EIGHT types; js8_text.c names
         * four and drops 4..7 through its default:. Those are JS8's data
         * frames - the free-text traffic J7 is about - so a drop here is not a
         * decode failure, it is an unimplemented feature arriving. Counted
         * separately because the two are indistinguishable in a decode total. */
        char text[JS8_TEXT_MAX];
        if (!js8_frame_to_text(msg.payload, text, sizeof(text))) {
            int ft = (int)js8_frame_type(msg.payload);
            g_unrendered[ft & 7]++;
            continue;
        }

        int dup = 0;
        for (int k = 0; k < got; k++)
            if (!strcmp(out[k].text, text)) { dup = 1; break; }
        if (dup) continue;

        snprintf(out[got].text, sizeof(out[got].text), "%s", text);
        out[got].hz = (int)(cands[i].freq_offset * (SR_HZ / 2) / mon.wf.num_bins);
        got++;
    }
    monitor_free(&mon);
    return got;
}

/* ---- one pair -------------------------------------------------------- */

static void run_pair(const char *wav, const char *txt)
{
    static float signal[SLOT_SAMPLES + 16];
    int n = 0;
    if (load_slot_wav(wav, signal, SLOT_SAMPLES, &n) < 0) { g_fail++; return; }

    row_t exp[MAX_ROWS], got[MAX_ROWS];
    int n_exp = parse_expected(txt, exp, MAX_ROWS);
    int n_got = decode_slot(signal, n, got, MAX_ROWS);
    if (n_got < 0) { g_fail++; return; }

    g_slots++;
    g_got_total += n_got;

    const char *base = strrchr(wav, '/');
    const char *b2   = strrchr(wav, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : wav;

    if (n_exp < 0) {
        printf("  %-20s  %d decoded   (no .txt - nothing to compare)\n",
               base, n_got);
        for (int i = 0; i < n_got; i++) printf("        %s\n", got[i].text);
        return;
    }
    g_exp_total += n_exp;

    int matched = 0, missing = 0;
    for (int i = 0; i < n_exp; i++) {
        int hit = 0;
        for (int k = 0; k < n_got; k++)
            if (!strcmp(exp[i].text, got[k].text)) { hit = 1; break; }
        if (hit) matched++; else missing++;
    }
    int extra = 0;
    for (int k = 0; k < n_got; k++) {
        int in_exp = 0;
        for (int i = 0; i < n_exp; i++)
            if (!strcmp(exp[i].text, got[k].text)) { in_exp = 1; break; }
        if (!in_exp) extra++;
    }
    g_match_total += matched;
    g_extra_total += extra;

    printf("  %-20s  %d/%d%s%s\n", base, matched, n_exp,
           extra   ? "  +EXTRA" : "",
           missing ? "  <-- LOST" : "");
    if (missing) {
        g_fail++;
        for (int i = 0; i < n_exp; i++) {
            int hit = 0;
            for (int k = 0; k < n_got; k++)
                if (!strcmp(exp[i].text, got[k].text)) { hit = 1; break; }
            if (!hit) printf("        LOST : %s\n", exp[i].text);
        }
    }
    for (int k = 0; k < n_got; k++) {
        int in_exp = 0;
        for (int i = 0; i < n_exp; i++)
            if (!strcmp(exp[i].text, got[k].text)) { in_exp = 1; break; }
        if (!in_exp) printf("        EXTRA: %s\n", got[k].text);
    }
}

static int ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcmp(s + ls - lf, suf);
}

static void run_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "cannot open %s\n", dir); g_fail++; return; }
    /* Names are YYMMDD_HHMMSS, so readdir order is close enough to time order
     * for a report; nothing here depends on it. */
    struct dirent *e;
    char wav[1024], txt[1024];
    int seen = 0;
    while ((e = readdir(d))) {
        if (!ends_with(e->d_name, ".wav")) continue;
        snprintf(wav, sizeof(wav), "%s/%s", dir, e->d_name);
        snprintf(txt, sizeof(txt), "%s/%.*s.txt", dir,
                 (int)(strlen(e->d_name) - 4), e->d_name);
        run_pair(wav, txt);
        seen++;
    }
    closedir(d);
    if (!seen) {
        /* An empty corpus directory silently "passing" is how a regression
         * test stops being one. */
        fprintf(stderr, "ERROR: no .wav files in %s - record a corpus first:\n"
                        "  python tools/js8_corpus.py <board-ip> --out %s\n",
                dir, dir);
        g_fail++;
    }
}

/* Plain CPFSK at the JS8 Normal rate: 79 symbols, 1920 samples each at 12 kHz
 * (160 ms), tone spacing 1/0.160 = 6.25 Hz. Continuous phase, so there is no
 * symbol-boundary click for the STFT to trip over. No GFSK shaping and no
 * noise: the question the selftest asks is "does the harness hear a clean
 * signal at all", and anything softer makes a failure ambiguous.
 *
 * One function rather than two copies, because the mutation test below must
 * drive the SAME synthesiser as the positive case - two copies would let the
 * two halves drift apart and still both pass. */
static void synth_slot(float *signal, const uint8_t *tones, int nsps,
                       float base, float spc, int t0)
{
    for (int i = 0; i < SLOT_SAMPLES; i++) signal[i] = 0.0f;
    double phase = 0.0;
    for (int s_i = 0; s_i < JS8_NN; s_i++) {
        double f  = base + spc * (double)tones[s_i];
        double dp = 2.0 * M_PI * f / (double)SR_HZ;
        for (int k = 0; k < nsps; k++) {
            int idx = t0 + s_i * nsps + k;
            if (idx >= SLOT_SAMPLES) break;
            signal[idx] = 0.25f * (float)sin(phase);
            phase += dp;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
}

/* ---- self-test: prove the harness can hear a JS8 signal --------------
 *
 * ⚠ MUTATION-TESTED ON PURPOSE. An empty corpus directory, an unreadable WAV
 * and a decoder that returns nothing all look identical in a report that only
 * prints totals, so the harness gets a signal it MUST decode and fails loudly
 * if it does not. Without this, "0 slots, PASS" would be an acceptable run.
 *
 * ⛔ This is still OUR OWN signal - js8_encode() is the transmitter's encoder -
 * so it proves the harness and the audio front end, NOT the protocol. The
 * protocol was settled by a real JS8Call station being decoded on 2026-10-08
 * (project_js8_rx_proven_on_air) and by the bit-exact check against
 * genjs8.f90. Do not cite a pass here as interop. */
static int selftest(void)
{
    static float signal[SLOT_SAMPLES + 16];
    const char *text = "N2VGU OZ1LAV -07";

    uint8_t frame[9], itype = 0;
    if (!js8_text_to_frame(text, frame, &itype)) {
        printf("selftest: js8_text_to_frame failed on '%s'\n", text);
        return 1;
    }
    uint8_t tones[JS8_NN];
    js8_encode(frame, itype, tones);

    /* Plain CPFSK at the JS8 Normal rate: 79 symbols, 1920 samples each at
     * 12 kHz (160 ms), 6.25 Hz tone spacing = 1/0.160. Continuous phase, so
     * there is no symbol-boundary click for the STFT to trip over. No GFSK
     * shaping and no noise: the question here is "does the harness hear a
     * clean signal at all", and anything softer makes a failure ambiguous. */
    const int   nsps = 1920;
    const float base = 1000.0f;             /* audio Hz of tone 0 */
    const float spc  = (float)SR_HZ / (float)nsps;
    const int   t0   = SR_HZ / 2;           /* 0.5 s of lead-in silence */
    synth_slot(signal, tones, nsps, base, spc, t0);

    row_t got[MAX_ROWS];
    int n = decode_slot(signal, SLOT_SAMPLES, got, MAX_ROWS);
    printf("selftest: %d decoded from a synthetic slot\n", n < 0 ? 0 : n);
    for (int i = 0; i < n; i++) printf("   %s\n", got[i].text);

    int heard = 0;
    for (int i = 0; i < n; i++) if (!strcmp(got[i].text, text)) heard = 1;
    if (!heard) {
        printf("selftest: FAIL - expected '%s'. The harness cannot hear a clean\n"
               "          JS8 signal, so any corpus result from it is meaningless.\n",
               text);
        return 1;
    }

    /* ⚠ MUTATION TEST of the unrendered-frame counter.
     *
     * A counter reading 0 because nothing arrived is indistinguishable from
     * one that cannot count, and this counter's only job is to say whether
     * free-text traffic (J7) is passing through and being dropped. The whole
     * corpus reported 0 on 2026-10-09; that number is worth nothing until the
     * counter has been seen to move.
     *
     * So: take the frame that just decoded, overwrite its 3 type bits with
     * 100 - a data frame, which js8_text.c has no renderer for - re-encode,
     * and require that the counter moves while the decode count does not. */
    int before = 0;
    for (int i = 0; i < 8; i++) before += g_unrendered[i];

    frame[0] = (uint8_t)((frame[0] & 0x1Fu) | (4u << 5));
    js8_encode(frame, itype, tones);
    synth_slot(signal, tones, nsps, base, spc, t0);
    int n2 = decode_slot(signal, SLOT_SAMPLES, got, MAX_ROWS);

    int after = 0;
    for (int i = 0; i < 8; i++) after += g_unrendered[i];
    if (n2 != 0 || after <= before) {
        printf("selftest: FAIL - a type-4 frame gave %d decodes and moved the\n"
               "          unrendered counter by %d. Expected 0 and >0, so the\n"
               "          counter cannot be trusted to report dropped data\n"
               "          frames - the only thing it is for.\n",
               n2, after - before);
        return 1;
    }
    printf("selftest: PASS (clean signal decodes; a type-4 data frame is "
           "counted as unrendered, +%d)\n", after - before);
    for (int i = 0; i < 8; i++) g_unrendered[i] = 0;   /* not a corpus result */
    return 0;
}

int main(int argc, char **argv)
{
    /* No arguments is how tools/run_harnesses.py invokes every harness, so
     * that case has to be the useful one: prove the harness can hear a clean
     * signal, then hold the recorded corpus to its floor. Printing a usage
     * line here would have made this a permanent RUN FAIL in the suite. */
    if (argc == 1) {
        if (selftest() != 0) return 1;
        printf("\n=== JS8 WAV harness: %s ===\n", DEFAULT_CORPUS);
        run_dir(DEFAULT_CORPUS);
    } else if (argc == 2 && !strcmp(argv[1], "--selftest")) {
        return selftest();
    } else if (argc == 2) {
        printf("=== JS8 WAV harness: %s ===\n", argv[1]);
        run_dir(argv[1]);
    } else if (argc == 3) {
        printf("=== JS8 WAV harness ===\n");
        run_pair(argv[1], argv[2]);
    } else {
        fprintf(stderr, "Usage: %s <corpus-dir> | <slot.wav> <slot.txt> | --selftest\n",
                argv[0]);
        return 2;
    }

    printf("\n=== Totals ===\n");
    printf("slots      : %d\n", g_slots);
    printf("expected   : %d\n", g_exp_total);
    printf("decoded    : %d\n", g_got_total);
    printf("matched    : %d\n", g_match_total);
    printf("extra      : %d  (not a failure - the corpus is a floor)\n", g_extra_total);
    printf("\n%s\n", g_fail ? "FAIL - a decode present in the corpus was lost"
                            : "PASS");
    return g_fail ? 1 : 0;
}
