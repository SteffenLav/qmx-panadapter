/*
 * CAN OSD EVEN REACH THE MISSED MESSAGES? - the instrument that decides
 * whether ordered-statistics decoding is worth building.
 *
 * ft8_decode_bench.c measures 235 of 345 ground-truth messages decoded over
 * test/wav_reference, so 110 are missed. OSD is the standing proposal for
 * that gap. OSD operates on a candidate that find_sync ALREADY FOUND and that
 * LDPC then failed: it re-orders the 174 soft bits by reliability and
 * re-solves. It cannot invent a candidate.
 *
 * So the gap splits in two, and only one half is OSD's to win:
 *
 *   MISS, candidate exists   LDPC failed on a found candidate  -> OSD's half
 *   MISS, no candidate       find_sync never saw it            -> OSD cannot
 *                                                                help at all
 *
 * This harness measures that split, and for the first half reports the
 * residual LDPC parity-check errors, because that number is what sets OSD's
 * order: a candidate left with a handful of unsatisfied checks is reachable
 * at low order, one left with dozens is not.
 *
 * It runs the same corpus twice per WAV:
 *
 *   DEVICE  MAXC 140,   MINSCORE 5   main/ft8_test.c's own constants
 *   WIDE    MAXC 60000, MINSCORE 0   every candidate find_sync can produce
 *
 * The two configs separate "the 140 cap cut it" from "find_sync never saw
 * it". ft8_decode_bench already showed the cap is not binding for DECODES
 * (140 -> 600 moves nothing), but that is a different question from whether
 * the cap hides candidates that only OSD could use.
 *
 * ⛔ THE WIDE CAP MUST NOT BIND, OR "NO CANDIDATE" IS A HARNESS ARTEFACT. At
 * MAXC_WIDE 1000 all 22 files returned exactly 1000 candidates and 2 missed
 * messages read as NO-CAND; at 60000 the busiest file returns 34253, nothing
 * is capped, and both of those 2 turn out to have a candidate after all.
 * Check the printed "wide=" count against MAXC_WIDE before reading the
 * no-cand column.
 *
 * Build (host, not the device). The two JS8 files are NOT optional - decode.c
 * has a JS8 branch and the link fails without them:
 *   gcc -O2 -o ft8_osd_reach test/ft8_osd_reach_harness.c \
 *       components/ft8_lib/ft8/{decode,encode,ldpc,text,message,crc,constants}.c \
 *       components/ft8_lib/ft8/{js8_codec,js8_tables}.c \
 *       components/ft8_lib/common/monitor.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -Icomponents/ft8_lib -Icomponents/ft8_lib/ft8 \
 *       -Icomponents/ft8_lib/common -Icomponents/ft8_lib/fft -lm
 *
 * Run over the 22 WAVs that have a .txt ground truth:
 *   for w in test/wav_reference/*.txt; do \
 *       ./ft8_osd_reach ${w%.txt}.wav $w; done
 *
 * Not a pass/fail harness - it has no correct answer, only a split you read.
 * run_harnesses.py does not run it.
 *
 * [!] WHAT A SMALL RESIDUAL ERROR COUNT DOES AND DOES NOT PROVE. It is
 * necessary, not sufficient. A candidate left with 2 unsatisfied parity
 * checks MIGHT be solved by a low-order OSD reprocessing; it might also be a
 * noise candidate whose soft bits happen to be nearly consistent and which
 * would fail the 14-bit CRC anyway. This harness sizes the opportunity. It
 * does not predict the yield. Only an OSD implementation measured on this
 * same corpus does that.
 *
 * [!] FREQUENCY MATCHING IS THE WEAK LINK. The .txt gives WSJT-X's frequency
 * to 1 Hz; a candidate is quantised to 6.25/freq_osr = 3.125 Hz. See FTOL
 * below for the window, why it is half the grid, and the sweep that shows the
 * answer does not depend on it. Read the counts, not an individual line.
 *
 * ===========================================================================
 * MEASURED 2026-11-20, 22 WAVs, 362 ground-truth messages, 260 decoded:
 *
 *   102 missed       cand-dev = 90    cand-wide = 12    NO-CAND = 0
 *
 * [*] FIND_SYNC IS NOT THE LIMITER. Every one of the 102 missed ground-truth
 * messages has a candidate that find_sync produced and that LDPC then failed
 * on. Not one is invisible to the sync search. So OSD's addressable half is
 * the WHOLE 102-message gap, and no amount of work on the sync stage can
 * claim any of it.
 *
 * [*] 90 of the 102 are already in the device's own 140-candidate list. The
 * other 12 need the cap lifted - they exist, but FT8_MAX_CANDIDATES 140 with
 * FT8_FIND_MIN_SCORE 5 does not keep them.
 *
 *     This does NOT contradict ft8_decode_bench's clean negative on the cap
 *     (140 -> 600 moves the decode count not at all for 5x the CPU). Both are
 *     true: the extra candidates decode nothing TODAY, because LDPC fails on
 *     them too. The cap would only start to matter if OSD existed to use
 *     them. Do not re-open the cap as a lever on its own.
 *
 * [*] Residual LDPC parity errors on the 90, device pool, is what sets the
 * OSD order needed: median 9, mean 8.8, min 1, max 23; 12 of 90 at <=4, 44 at
 * <=8, 75 at <=12.
 *
 * ⛔ THE ERROR COUNT IS A FUNCTION OF THE POOL SIZE, SO ONLY THE DEVICE-POOL
 * NUMBER MEANS ANYTHING. Taking the lowest-error candidate near a frequency
 * out of ~34000 instead of 140 drops the mean for THE SAME 90 MESSAGES from
 * 8.8 to 2.4, and the count at <=2 errors rises from 4 to 55. That is a
 * selection effect of drawing more samples, not a property of those signals.
 * Quote the dev_err column; wide_err is only there to answer "does a
 * candidate exist at all".
 * ===========================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <ft8/decode.h>
#include <ft8/message.h>
#include <common/monitor.h>

#define SR        12000
#define TIME_OSR  2
#define FREQ_OSR  2
#define ITERS     30
/* Hz, truth-to-candidate match window.
 *
 * The default is HALF THE CANDIDATE GRID, not a round number. With freq_osr 2
 * a candidate is quantised to 6.25/2 = 3.125 Hz, so a true signal can sit up
 * to 1.5625 Hz from the nearest bin. A window tighter than that would reject
 * candidates that are in fact the right one; a window wider than that starts
 * admitting the neighbour, which on a crowded slot inflates the answer.
 *
 * Overridable with -D so the instrument can be mutation-tested. SWEEP,
 * 2026-11-20, over the same 22 WAVs (102 missed truth lines):
 *
 *   FTOL Hz   cand-dev  cand-wide  no-cand
 *   0            1          0        101     <- matcher does the work
 *   1           75          7         20
 *   1.5625      90         10          2     <- default, half the grid
 *   2           91          9          2
 *   3.125       94          7          1
 *   4           96          6          0
 *   100000     102          0          0     <- matches anything
 *
 * [*] THE CONCLUSION IS THE SAME ACROSS EVERY DEFENSIBLE WINDOW. From
 * 1.5625 Hz to 4 Hz, no-cand is 2 or fewer of 102. The two ends of the sweep
 * are the mutation test: at 0 the matcher rejects everything, at 100000 it
 * accepts everything, so the middle is a measurement and not a constant.
 */
#ifndef FTOL
#define FTOL      1.5625f
#endif
#ifndef MAXC_WIDE
#define MAXC_WIDE 60000
#endif
#define MAXE      200

static int load_wav(float* sig, int* n, int* sr, const char* path)
{
    FILE* f = fopen(path, "rb"); if (!f) return -1;
    char riff[4]; if (fread(riff, 1, 4, f) != 4) { fclose(f); return -1; }
    fseek(f, 12, SEEK_SET); int cid, csz, srt, fmt;
    while (fread(&cid, 4, 1, f) == 1) {
        if (fread(&csz, 4, 1, f) != 1) break;
        if (cid == 0x20746d66) {
            if (fread(&fmt, 2, 1, f) != 1) break; short ch;
            if (fread(&ch, 2, 1, f) != 1) break;
            if (fread(&srt, 4, 1, f) != 1) break;
            *sr = srt; fseek(f, csz - 8, SEEK_CUR);
        } else if (cid == 0x61746164) {
            short* p = malloc(csz);
            if (fread(p, 1, csz, f) != (size_t)csz) {}
            int nr = csz / 2; *n = (nr < *n) ? nr : *n;
            for (int i = 0; i < *n; i++) sig[i] = (float)p[i] / 32768.0f;
            free(p); break;
        } else fseek(f, csz, SEEK_CUR);
    }
    fclose(f); return 0;
}

/* ground truth: "HHMMSS SNR DT FREQ ~ MESSAGE".  Unlike ft8_decode_bench's
 * loader this one keeps the frequency, which is the whole point here. */
static char  exp_txt[MAXE][64];
static float exp_hz[MAXE];
static int   n_exp;

static void load_expected(const char* p)
{
    n_exp = 0; FILE* f = fopen(p, "r"); if (!f) return; char line[256];
    while (fgets(line, sizeof line, f) && n_exp < MAXE) {
        char* s = strchr(line, '~'); if (!s) continue;
        /* the four fields before '~' */
        int   hhmmss; float snr, dt, hz;
        if (sscanf(line, "%d %f %f %f", &hhmmss, &snr, &dt, &hz) != 4) continue;
        s++; while (*s == ' ') s++;
        int len = strlen(s);
        while (len && (s[len-1] == '\n' || s[len-1] == '\r')) len--;
        /* the trailing country annotation is a 4th space-separated field */
        int sp = 0, rl = len;
        for (int j = 0; j < len; j++) if (s[j] == ' ') { if (++sp == 3) { rl = j; break; } }
        while (rl && s[rl-1] == ' ') rl--;
        if (rl > 0 && rl < 63) {
            memcpy(exp_txt[n_exp], s, rl); exp_txt[n_exp][rl] = 0;
            exp_hz[n_exp] = hz; n_exp++;
        }
    }
    fclose(f);
}

static int match(const char* a, const char* b)
{
    while (*a && *b) {
        while (*a == ' ') a++; while (*b == ' ') b++;
        if (!*a || !*b) break;
        if (tolower(*a) != tolower(*b)) return 0;
        a++; b++;
    }
    while (*a == ' ') a++; while (*b == ' ') b++;
    return !*a && !*b;
}

/* one pass over a config: fills decoded texts, and for every candidate its
 * frequency and - on failure - its residual LDPC parity errors. */
typedef struct {
    float hz;
    int   score;
    int   ldpc_err;    /* -1 if the candidate decoded successfully */
} cand_info_t;

static int run_config(const float* sig, int n, int sr, int maxc, int minscore,
                      char texts[][64], int* ntext,
                      cand_info_t* info, int info_max)
{
    monitor_t mon;
    monitor_config_t cfg = { 200, 3000, sr, TIME_OSR, FREQ_OSR, FTX_PROTOCOL_FT8 };
    monitor_init(&mon, &cfg);
    for (int p = 0; p + mon.block_size <= n; p += mon.block_size)
        monitor_process(&mon, sig + p);

    ftx_candidate_t* cands = malloc(sizeof(ftx_candidate_t) * maxc);
    int nc = ftx_find_candidates(&mon.wf, maxc, cands, minscore);

    *ntext = 0;
    int ni = 0;
    for (int i = 0; i < nc; i++) {
        ftx_message_t m; ftx_decode_status_t st;
        memset(&st, 0, sizeof st);
        int ok = ftx_decode_candidate(&mon.wf, &cands[i], ITERS, &m, &st);

        if (ni < info_max) {
            info[ni].hz = (mon.min_bin + cands[i].freq_offset
                           + (float)cands[i].freq_sub / mon.wf.freq_osr)
                          / mon.symbol_period;
            info[ni].score    = cands[i].score;
            info[ni].ldpc_err = ok ? -1 : st.ldpc_errors;
            ni++;
        }
        if (ok) {
            char t[64]; ftx_message_offsets_t off;
            if (ftx_message_decode(&m, NULL, t, &off) == FTX_MESSAGE_RC_OK) {
                int dup = 0;
                for (int j = 0; j < *ntext; j++) if (!strcmp(texts[j], t)) dup = 1;
                if (!dup && *ntext < MAXC_WIDE) snprintf(texts[(*ntext)++], 64, "%s", t);
            }
        }
    }
    free(cands);
    monitor_free(&mon);
    return ni;
}

/* best (lowest residual error) failing candidate within FTOL of hz */
static int nearest_fail(const cand_info_t* info, int ni, float hz, float* d_out)
{
    int best = 1 << 20; float bd = 0;
    for (int i = 0; i < ni; i++) {
        if (info[i].ldpc_err < 0) continue;              /* that one decoded */
        float d = fabsf(info[i].hz - hz);
        if (d > FTOL) continue;
        if (info[i].ldpc_err < best) { best = info[i].ldpc_err; bd = d; }
    }
    if (best == (1 << 20)) return -1;
    *d_out = bd; return best;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ft8_osd_reach <wav> <expected.txt>\n");
        return 1;
    }
    static float sig[SR * 20]; int n = SR * 20, sr = SR;
    if (load_wav(sig, &n, &sr, argv[1])) {
        fprintf(stderr, "bad wav %s\n", argv[1]); return 1;
    }
    load_expected(argv[2]);
    if (!n_exp) { fprintf(stderr, "no ground truth in %s\n", argv[2]); return 1; }

    static char dev_texts[MAXC_WIDE][64], wide_texts[MAXC_WIDE][64];
    static cand_info_t dev_info[MAXC_WIDE], wide_info[MAXC_WIDE];
    int dev_ntext, wide_ntext;

    int dev_ni  = run_config(sig, n, sr, 140,       5, dev_texts,  &dev_ntext,
                             dev_info,  MAXC_WIDE);
    int wide_ni = run_config(sig, n, sr, MAXC_WIDE, 0, wide_texts, &wide_ntext,
                             wide_info, MAXC_WIDE);

    int hit = 0, reach_dev = 0, reach_wide_only = 0, no_cand = 0;

    for (int e = 0; e < n_exp; e++) {
        int got = 0;
        for (int j = 0; j < dev_ntext; j++) if (match(exp_txt[e], dev_texts[j])) got = 1;
        if (got) { hit++; continue; }

        float d = 0;
        int ed = nearest_fail(dev_info,  dev_ni,  exp_hz[e], &d);
        float dw = 0;
        int ew = nearest_fail(wide_info, wide_ni, exp_hz[e], &dw);

        const char* cls;
        if (ed >= 0)      { cls = "CAND-DEV";  reach_dev++; }
        else if (ew >= 0) { cls = "CAND-WIDE"; reach_wide_only++; }
        else              { cls = "NO-CAND";   no_cand++; }

        char sd[8], sw[8];
        if (ed >= 0) snprintf(sd, sizeof sd, "%d", ed); else strcpy(sd, "-");
        if (ew >= 0) snprintf(sw, sizeof sw, "%d", ew); else strcpy(sw, "-");

        printf("  MISS %-9s %6.1f Hz  dev_err=%-4s wide_err=%-4s  %s\n",
               cls, exp_hz[e], sd, sw, exp_txt[e]);
    }

    printf("%-28s truth=%2d hit=%2d miss=%2d | cand-dev=%2d cand-wide=%2d"
           " no-cand=%2d | cands dev=%d wide=%d\n",
           argv[1], n_exp, hit, n_exp - hit,
           reach_dev, reach_wide_only, no_cand, dev_ni, wide_ni);
    return 0;
}
