/* ft8_gap_harness.c - where do the decodes jt9 finds, and we do not, get lost?
 *
 * For one WAV and a list of jt9 decodes (freq, DT, text), prints:
 *   CAND <rank> <f_hz> <dt> <base_ok>     every ft8_lib candidate, strongest
 *                                         first, as the Tab5 decoder lists them
 *                                         (MAXC 140, min score 5); dt is WSJT-X DT
 *   EXACT <ok> <text>                     WS 3.2.1's ft8b (5 passes, BP) run at
 *                                         jt9's OWN frequency and DT
 * tools/wsjtx_port/gap_analysis.py does the classification.
 *
 * Usage: ft8_gap_harness <wav> <targets.txt>   (lines: "<freq> <dt> <text>")
 *
 * gcc -O2 -DFT8_DECODER_WSJTX -DWSX_PIECE_ONLY -o gap.exe test/ft8_gap_harness.c
 *   main/ft8_wsjtx.c <ft8_lib sources as in ft8_piece_harness.c> -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft8/decode.h"
#include "ft8/message.h"
#include "common/monitor.h"
#include "ft8_wsjtx.h"

#ifndef MAXC
#define MAXC 140
#endif
#ifndef DT_SHIFT
#define DT_SHIFT 0.67f
#endif

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
    if (argc < 3) { fprintf(stderr, "usage: %s wav targets\n", argv[0]); return 2; }
    static int16_t iwave[WSX_NMAX];
    static float sig[WSX_NMAX];
    if (load_wav(iwave, WSX_NMAX, argv[1]) < 0) { fprintf(stderr, "bad wav\n"); return 1; }
    for (int i = 0; i < WSX_NMAX; i++) sig[i] = iwave[i] / 32768.0f;

    monitor_t mon;
    monitor_config_t cfg = {200, 3000, 12000, 2, 2, FTX_PROTOCOL_FT8};
    monitor_init(&mon, &cfg);
    for (int p = 0; p + mon.block_size <= WSX_NMAX; p += mon.block_size) monitor_process(&mon, sig + p);
    static ftx_candidate_t cands[MAXC];
    int nc = ftx_find_candidates(&mon.wf, MAXC, cands, 5);
    for (int i = 0; i < nc; i++) {
        ftx_message_t m;
        ftx_decode_status_t st;
        int ok = ftx_decode_candidate(&mon.wf, &cands[i], 30, &m, &st) ? 1 : 0;
        float f = (mon.min_bin + cands[i].freq_offset + (float)cands[i].freq_sub / mon.wf.freq_osr)
                  / mon.symbol_period;
        float t = (cands[i].time_offset + (float)cands[i].time_sub / mon.wf.time_osr) * mon.symbol_period;
        printf("CAND\t%d\t%.2f\t%.3f\t%d\t%d\n", i, f, t - DT_SHIFT, ok, cands[i].score);
    }

#ifndef MAXOSD
#define MAXOSD -1
#endif
    wsx_piece_begin(iwave, 5, MAXOSD);   /* -1 = BP only (the Tab5), 2 = jt9 ndeep 2 */
    FILE* tf = fopen(argv[2], "r");
    char line[256];
    while (tf && fgets(line, sizeof line, tf)) {
        float fq, dt;
        int off = 0;
        if (sscanf(line, "%f %f %n", &fq, &dt, &off) < 2) continue;
        char* txt = line + off;
        txt[strcspn(txt, "\r\n")] = 0;
        char out[40] = {0};
        int ok = wsx_piece_try(fq, dt, 1, out, NULL, NULL);
        printf("EXACT\t%d\t%s\t%s\n", ok, txt, ok ? out : "");
    }
    if (tf) fclose(tf);
    wsx_piece_end();
    monitor_free(&mon);
    return 0;
}
