/* ft8_piece_harness - single WSJT-X pieces on the Tab5 decoder's own
 * candidates.
 *
 * The full WSJT-X port does not fit the Tab5 (memory and CPU, measured
 * 2026-11-21). This asks which ONE piece is worth porting: ft8_lib builds the
 * waterfall and the candidate list exactly as the device does (time_osr 2,
 * freq_osr 2, min score 5, cap MAXC), and each candidate is decoded twice -
 *   base : ft8_lib's own LDPC decode (ftx_decode_candidate, 30 iterations)
 *   piece: WSJT-X ft8b on the same frequency and time (wsx_piece_try), with
 *          -DNPASSES (1 = single-symbol metric only, 5 = all five metric
 *          passes) and -DMAXOSD (-1 = BP only, 2 = ft8b's OSD)
 * One pass, no subtraction, no AP, so the two differ only in the piece.
 *
 * Prints, per WAV:  BASE\t<text>   PIECE\t<text>
 * and on stderr the per-candidate timing of each side.
 *
 * gcc -O2 -DFT8_DECODER_WSJTX [-DNPASSES=5 -DMAXOSD=2 -DMAXC=200] -o p.exe \
 *   test/ft8_piece_harness.c main/ft8_wsjtx.c components/ft8_lib/ft8/{decode,encode,
 *   ldpc,text,message,crc,constants,js8_codec,js8_tables}.c components/ft8_lib/
 *   common/monitor.c components/ft8_lib/fft/kiss_fft{,r}.c -Imain -Icomponents/ft8_lib
 *   -Icomponents/ft8_lib/ft8 -Icomponents/ft8_lib/common -Icomponents/ft8_lib/fft -lm
 */
#include <stdio.h>
#include "ft8/encode.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ft8/decode.h"
#include "ft8/message.h"
#include "common/monitor.h"
#include "ft8_wsjtx.h"

#ifndef NPASSES
#define NPASSES 5
#endif
#ifndef MAXOSD
#define MAXOSD 2
#endif
#ifndef MAXC
#define MAXC 200
#endif
#ifndef PIECE_K
#define PIECE_K 100000   /* try the piece on at most this many base FAILURES, strongest first */
#endif
#define SR 12000
#ifndef DT_SHIFT
#define DT_SHIFT 0.5f   /* ft8_lib time - this = WSJT-X DT; measured, see below */
#endif

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int load_wav(int16_t* out, int nmax, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 12, SEEK_SET);
    int cid, csz, n = -1;
    while (fread(&cid, 4, 1, f) == 1 && fread(&csz, 4, 1, f) == 1) {
        if (cid == 0x61746164) {
            int nr = csz / 2;
            n = nr < nmax ? nr : nmax;
            if (fread(out, 2, n, f) != (size_t)n) n = -1;
            break;
        }
        fseek(f, csz, SEEK_CUR);
    }
    fclose(f);
    return n;
}

int main(int argc, char** argv)
{
    static int16_t iwave[WSX_NMAX];
    static float sig[WSX_NMAX];
    double t_base = 0, t_piece = 0;
    long n_cand_total = 0;
    for (int fi = 1; fi < argc; fi++) {
        memset(iwave, 0, sizeof iwave);
        if (load_wav(iwave, WSX_NMAX, argv[fi]) < 0) { fprintf(stderr, "bad wav %s\n", argv[fi]); return 1; }
        for (int i = 0; i < WSX_NMAX; i++) sig[i] = iwave[i] / 32768.0f;   /* the harnesses' scale */
        printf("FILE\t%s\n", argv[fi]);

        monitor_t mon;
        monitor_config_t cfg = {200, 3000, SR, 2, 2, FTX_PROTOCOL_FT8};
        monitor_init(&mon, &cfg);
        for (int p = 0; p + mon.block_size <= WSX_NMAX; p += mon.block_size) monitor_process(&mon, sig + p);
        static ftx_candidate_t cands[MAXC];
        int nc = ftx_find_candidates(&mon.wf, MAXC, cands, 5);
        n_cand_total += nc;

        char seen_b[MAXC][40], seen_p[MAXC][40];
        int nb = 0, np = 0;
#ifdef TONE_CHECK
        /* The device dedups a piece decode against the base decodes by
         * comparing 79 tones: ft8_encode(base payload) vs ft8b's itone.
         * This checks that rule against text equality on real slots. */
        static uint8_t base_tones[MAXC][79];
        int n_bt = 0;
#endif
        static unsigned char base_ok[MAXC];
        memset(base_ok, 0, sizeof base_ok);
        double a = now_ms();
        for (int i = 0; i < nc; i++) {
            ftx_message_t m;
            ftx_decode_status_t stt;
            if (!ftx_decode_candidate(&mon.wf, &cands[i], 30, &m, &stt)) continue;
            char t[64];
            ftx_message_offsets_t off;
            if (ftx_message_decode(&m, NULL, t, &off) != FTX_MESSAGE_RC_OK) continue;
            base_ok[i] = 1;
#ifdef TONE_CHECK
            if (n_bt < MAXC) ft8_encode(m.payload, base_tones[n_bt++]);
#endif
            int dup = 0;
            for (int j = 0; j < nb; j++) if (!strcmp(seen_b[j], t)) dup = 1;
            if (dup) continue;
            snprintf(seen_b[nb++], 40, "%s", t);
            printf("BASE\t%s\n", t);
        }
        t_base += now_ms() - a;

        a = now_ms();
#ifdef BEGIN_AUDIO
        /* The Tab5's path: raw floats at an arbitrary level into the port's
         * own buffer, normalised in place. Must decode exactly as the int16
         * path above does. */
        {
            float* dd = wsx_piece_audio();
            for (int i = 0; i < WSX_NMAX; i++) dd[i] = sig[i] * 0.013f;
            wsx_piece_begin_audio(WSX_NMAX, NPASSES, MAXOSD);
        }
#else
        wsx_piece_begin(iwave, NPASSES, MAXOSD);
#endif
#ifdef NDEEP
        wsx_piece_set_ndeep(NDEEP);
#endif
#ifdef APCQ
        wsx_piece_set_ap_cq(1);
#endif
        int tried = 0;
        for (int i = 0; i < nc; i++) {
            /* PIECE_K set: only candidates ft8_lib FAILED, strongest first
             * (find_candidates returns them by score), at most PIECE_K. */
            if (PIECE_K < 100000) {
                if (base_ok[i]) continue;
                if (tried++ >= PIECE_K) break;
            }
#ifdef BUDGET_MS
            /* The device's wall-clock budget, scaled to the host (P4 ~120x). */
            if (now_ms() - a >= BUDGET_MS) break;
#endif
            float f = (mon.min_bin + cands[i].freq_offset + (float)cands[i].freq_sub / mon.wf.freq_osr)
                      / mon.symbol_period;
            float tsec = (cands[i].time_offset + (float)cands[i].time_sub / mon.wf.time_osr)
                         * mon.symbol_period;
            char t[40];
            /* ft8_lib's time is the Costas start from the WAV start; WSJT-X's
             * DT is that minus the nominal 0.5 s. */
            float fo, dto;
#ifdef TONE_CHECK
            int itone[79];
            float snr;
            if (!wsx_piece_try_ex(f, tsec - DT_SHIFT, 1, t, &fo, &dto, &snr, itone)) continue;
            {
                int tdup = 0, xdup = 0;
                for (int k = 0; k < n_bt && !tdup; k++) {
                    int j = 0;
                    while (j < 79 && base_tones[k][j] == (uint8_t)itone[j]) j++;
                    tdup = (j == 79);
                }
                for (int j = 0; j < nb; j++) if (!strcmp(seen_b[j], t)) xdup = 1;
                printf("TCHK\t%d\t%d\t%s\n", tdup, xdup, t);
            }
#else
            if (!wsx_piece_try(f, tsec - DT_SHIFT, 1, t, &fo, &dto)) continue;
#endif
#ifdef DT_DEBUG
            fprintf(stderr, "DTDBG %.3f %.2f\n", dto - (tsec - DT_SHIFT), fo - f);
#endif
            int dup = 0;
            for (int j = 0; j < np; j++) if (!strcmp(seen_p[j], t)) dup = 1;
            if (dup) continue;
            snprintf(seen_p[np++], 40, "%s", t);
            printf("PIECE\t%s\n", t);
        }
        wsx_piece_end();
#ifdef RELEASE_EACH
        wsx_piece_release();   /* FT8 stop/start: realloc must change nothing */
#endif
        t_piece += now_ms() - a;
        fflush(stdout);
    }
#ifdef WSX_PROF
    { extern void wsx_prof_print(void); wsx_prof_print(); }
#endif
    fprintf(stderr, "candidates %ld; base %.1f ms; piece %.1f ms (incl. per-slot setup)\n",
            n_cand_total, t_base, t_piece);
    return 0;
}
