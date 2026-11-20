/*
 * OSD PROTOTYPE - does the reachable gap actually decode?
 *
 * ft8_osd_reach_harness.c measured that all 102 missed ground-truth messages
 * have a candidate that find_sync found and LDPC failed on, so the whole gap
 * is OSD's to win. It SIZED the opportunity and said so: it does not predict
 * the yield. This harness measures the yield.
 *
 * Ordered-statistics decoding, orders 0 and 1:
 *
 *   1. Take the 174 soft bits (ftx_extract_likelihood_probe, -D guarded).
 *   2. Sort bit positions by |LLR| descending - most reliable first.
 *   3. Walk that order, greedily keeping any column that raises the rank of
 *      the 91x174 systematic generator, until 91 independent columns are in
 *      hand. That is the most-reliable basis (MRB).
 *   4. Row-reduce so those 91 columns are the identity. The hard decisions on
 *      them then determine one whole codeword - that is ORDER 0.
 *   5. ORDER 1 flips each basis bit in turn: 91 more codewords.
 *   6. Every codeword is CRC-checked exactly as ftx_decode_candidate() does.
 *      Among those that pass, keep the one with the smallest soft distance,
 *      sum |LLR| over positions where it disagrees with the hard decision.
 *
 * ⛔ THE CRC IS 14 BITS, SO TRIALS COST FALSE DECODES. One trial false-accepts
 * with probability 2^-14. Order 1 is 92 trials per candidate and the device
 * runs 140 candidates a slot, so the EXPECTED false accepts are
 * 92*140/16384 = 0.79 per slot before any soft-distance filter. That is why
 * step 6 keeps the minimum-distance codeword instead of the first CRC pass,
 * and why this harness reports ground-truth hits separately from raw decode
 * counts: a yield figure that counts false accepts as wins is worthless.
 * Order 2 (4095 trials) would expect ~35 false accepts a slot and is NOT
 * implemented here for that reason - it needs a real distance threshold
 * first, which needs this measurement to set.
 *
 * Build (host, not the device). Note -DFTX_EXPOSE_LIKELIHOOD:
 *   gcc -O2 -DFTX_EXPOSE_LIKELIHOOD -o ft8_osd_proto \
 *       test/ft8_osd_proto_harness.c \
 *       components/ft8_lib/ft8/{decode,encode,ldpc,text,message,crc,constants}.c \
 *       components/ft8_lib/ft8/{js8_codec,js8_tables}.c \
 *       components/ft8_lib/common/monitor.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -Icomponents/ft8_lib -Icomponents/ft8_lib/ft8 \
 *       -Icomponents/ft8_lib/common -Icomponents/ft8_lib/fft -lm
 *
 * Run over the 22 WAVs that have a .txt ground truth:
 *   for w in test/wav_reference/*.txt; do ./ft8_osd_proto ${w%.txt}.wav $w; done
 *
 * -DOSD_ORDER=0 restricts it to order 0. -DOSD_OFF builds the same binary with
 * no OSD at all, which is the baseline row and the mutation test in one: it
 * must reproduce the plain decoder exactly.
 *
 * ===========================================================================
 * MEASURED 2026-11-20, 22 WAVs, 362 ground-truth messages:
 *
 *   build       uniq  hit/362   osd decodes  in truth  trials   osd ms
 *   OSD_OFF      264    260          -           -         0       0
 *   order 0      268    264          5          5/5     2306      61
 *   order 1      282    269         20         11/20  212152     153
 *
 * [*] OSD_OFF REPRODUCES THE PLAIN DECODER EXACTLY - 264 uniq / 260 hit is
 * ft8_subtract_bench's pass-1 row and ft8_osd_reach_harness's baseline to the
 * message. That is the mutation test: the OSD path is doing the work in the
 * other two rows, not the accounting.
 *
 * ⛔ REACHABLE IS NOT RECOVERABLE, AND THIS IS THE CORRECTION TO THE REACH
 * MEASUREMENT. ft8_osd_reach_harness showed all 102 missed messages have a
 * failing candidate, so the whole gap is OSD's to address. Low-order OSD
 * recovers 4 of them at order 0 and 9 at order 1. The prize is +3.5 % of the
 * corpus (260 -> 269), NOT the +28 % that "the whole gap is reachable" might
 * suggest. Anyone quoting the reach figure as an OSD forecast is wrong.
 *
 * [*] ORDER 0 IS FREE AND CLEAN: +4 ground-truth hits, 2306 trials over the
 * whole corpus (2.8 ms a slot), and all 5 of its decodes are in the truth.
 * Expected false accepts 2306/16384 = 0.14, observed 0.
 *
 * ⛔ ORDER 1'S EXTRA DECODES ARE MOSTLY CRC FALSE ACCEPTS. 212152 trials
 * predict 12.9 false accepts; 9 of its 20 decodes are not in the truth, and
 * six of those nine are visibly corrupt text - `2D33EBF16F02181448`,
 * `53/L/JIZOIN 73`, `0R7XJX/R YB5YGZ/R OQ53`. The prediction and the
 * observation agree, so these are false, not gaps in WSJT-X's .txt. ⚠ Note
 * this is the OPPOSITE situation to ft8_decode_bench's time_osr 4 row, where
 * 0.015 expected passes justified calling the extra decodes genuine. Same
 * arithmetic, opposite conclusion - always run it.
 *
 * [*] THE SOFT METRIC SEPARATES THEM, so order 1 is salvageable. Measured
 * metric ranges: in-truth 23.5 to 55.6, the six corrupt ones 73.2 to 99.7. A
 * threshold at 60 keeps all 11 in-truth decodes and drops all six. The three
 * not-in-truth below 60 (`CQ DX Z33Z KN11`, `KC8MUE V51MA RRR`,
 * `0R7XJX/R YB5YGZ/R OQ53`) include two plausible real messages that appear
 * in ANOTHER file's truth, so they may not be false at all.
 *
 * NOT DONE: the threshold is read off 20 samples and is not calibrated. Order
 * 2 (4095 trials, ~35 expected false accepts a slot) needs a calibrated
 * threshold before it is worth building at all.
 *
 * ---------------------------------------------------------------------------
 * SUBTRACTION AND OSD TOGETHER, measured 2026-11-20, same 22 WAVs / 362 truth.
 * -DNPASS=2 turns on subtract-and-repeat; every decode feeds the next pass,
 * OSD's own included.
 *
 *   variant                       uniq  hit/362  osd  in truth  osd ms/slot
 *   plain                          264    260     -      -           -
 *   subtract, -DSUB_SKIP_DUP       285    280     -      -           -
 *   subtract                       289    284     -      -           -
 *   OSD 0                          268    264     5     5/5         2.7
 *   subtract + OSD 0               293    288     5     5/5         6.5
 *   subtract + OSD 1               315    290    32    12/32       16.4
 *
 * [*] THE TWO LEVERS ARE ADDITIVE. Subtraction alone is +24 ground-truth
 * hits, OSD 0 alone is +4, and together they are +28 - exactly the sum, no
 * overlap. They fix different failures: subtraction removes a strong signal
 * that was masking a weak one, OSD re-solves a candidate whose soft bits were
 * nearly right. 288 of 362 is 79.6 % against the baseline's 71.8 %.
 *
 * [*] -DSUB_SKIP_DUP REPRODUCES ft8_subtract_bench.c EXACTLY, 285 uniq / 280
 * hit. That is this harness validated against a known-good number, and it
 * explains the difference: that bench `continue`s on a duplicate text before
 * collecting it for subtraction, so it never subtracts a signal decoded twice
 * from two candidate positions.
 *
 * [*] SUBTRACTING DUPLICATES IS WORTH +4 GROUND-TRUTH HITS, FREE (280 -> 284).
 * ft8_subtract_bench leaves that on the table. A duplicate text means the same
 * signal was found at two candidate positions; removing both instances fits
 * the residual better than removing one.
 *
 * ⛔ ORDER 1 IS STILL NOT WORTH IT. +2 hits over OSD 0 (288 -> 290) for 32 OSD
 * decodes of which only 12 are in the truth. 492016 trials predict 30.0 CRC
 * false accepts and ~20 were observed. The arithmetic holds across both
 * measurements; it needs the calibrated distance threshold first.
 * ===========================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <ft8/decode.h>
#include <ft8/message.h>
#include <ft8/encode.h>
#include <ft8/constants.h>
#include <ft8/crc.h>
#include <common/monitor.h>
#include "ft8_subtract.h"

#define SR       12000
#define TIME_OSR 2
#define FREQ_OSR 2
#define ITERS    30
#define MAXC     140
#define MINSCORE 5
#define MAXE     200
#define MAXMSG   400

#ifndef OSD_ORDER
#define OSD_ORDER 1
#endif

/* Subtract-and-repeat passes. 1 = no subtraction, which is how the first
 * measurement in the header was taken. */
#ifndef NPASS
#define NPASS 1
#endif

#define N FTX_LDPC_N   /* 174 */
#define K FTX_LDPC_K   /*  91 */

static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000.0 + ts.tv_nsec/1e6; }

/* ---------------------------------------------------------------- wav + truth
 * Same loaders as the other two FT8 harnesses, deliberately: the truth
 * accounting must be comparable with ft8_osd_reach_harness.c's 260 of 362. */
static int load_wav(float* sig,int* n,int* sr,const char* path){
    FILE* f=fopen(path,"rb"); if(!f) return -1;
    char riff[4]; if(fread(riff,1,4,f)!=4){fclose(f);return -1;}
    fseek(f,12,SEEK_SET); int cid,csz,srt,fmt;
    while(fread(&cid,4,1,f)==1){ if(fread(&csz,4,1,f)!=1) break;
        if(cid==0x20746d66){ if(fread(&fmt,2,1,f)!=1)break; short ch;
            if(fread(&ch,2,1,f)!=1)break; if(fread(&srt,4,1,f)!=1)break;
            *sr=srt; fseek(f,csz-8,SEEK_CUR);
        } else if(cid==0x61746164){ short* p=malloc(csz);
            if(fread(p,1,csz,f)!=(size_t)csz){} int nr=csz/2;
            *n=(nr<*n)?nr:*n; for(int i=0;i<*n;i++) sig[i]=(float)p[i]/32768.0f;
            free(p); break;
        } else fseek(f,csz,SEEK_CUR); }
    fclose(f); return 0;
}
static char exp_txt[MAXE][64]; static int n_exp;
static void load_expected(const char* p){
    n_exp=0; FILE* f=fopen(p,"r"); if(!f) return; char line[256];
    while(fgets(line,sizeof line,f) && n_exp<MAXE){
        char* s=strchr(line,'~'); if(!s) continue; s++;
        while(*s==' ')s++;
        int len=strlen(s); while(len&&(s[len-1]=='\n'||s[len-1]=='\r'))len--;
        int sp=0,rl=len;
        for(int j=0;j<len;j++) if(s[j]==' '){ if(++sp==3){rl=j;break;} }
        while(rl&&s[rl-1]==' ')rl--;
        if(rl>0&&rl<63){ memcpy(exp_txt[n_exp],s,rl); exp_txt[n_exp][rl]=0; n_exp++; }
    }
    fclose(f);
}
static int match(const char*a,const char*b){
    while(*a&&*b){ while(*a==' ')a++; while(*b==' ')b++;
        if(!*a||!*b)break; if(tolower(*a)!=tolower(*b))return 0; a++;b++; }
    while(*a==' ')a++; while(*b==' ')b++; return !*a&&!*b;
}

/* ------------------------------------------------------------------ the code
 * G is 91 x 174 systematic: row i is e_i in the first 91 columns, and in
 * parity column 91+j the bit kFTX_LDPC_generator[j][i>>3] >> (7-(i&7)).
 * Rows are bitsets over 174 columns, 3 x uint64. */
typedef struct { uint64_t w[3]; } row_t;

static inline int rget(const row_t* r,int c){ return (int)((r->w[c>>6] >> (c&63)) & 1u); }
static inline void rset(row_t* r,int c){ r->w[c>>6] |= (uint64_t)1u << (c&63); }
static inline void rxor(row_t* a,const row_t* b){ a->w[0]^=b->w[0]; a->w[1]^=b->w[1]; a->w[2]^=b->w[2]; }

static row_t G0[K];        /* the pristine generator, never modified */

static void build_generator(void)
{
    memset(G0,0,sizeof G0);
    for (int i = 0; i < K; i++) {
        rset(&G0[i], i);
        for (int j = 0; j < FTX_LDPC_M; j++)
            if ((kFTX_LDPC_generator[j][i >> 3] >> (7 - (i & 7))) & 1)
                rset(&G0[i], K + j);
    }
}

/* CRC check on the first 91 bits, byte-for-byte what ftx_decode_candidate
 * does. Returns 1 and fills payload[10] when it passes. */
static int crc_ok(const uint8_t cw[N], uint8_t payload[10])
{
    uint8_t a91[FTX_LDPC_K_BYTES];
    memset(a91, 0, sizeof a91);
    for (int i = 0; i < K; i++)
        if (cw[i]) a91[i / 8] |= (uint8_t)(0x80u >> (i % 8));

    uint16_t extracted = ftx_extract_crc(a91);
    uint8_t t[FTX_LDPC_K_BYTES];
    memcpy(t, a91, sizeof t);
    t[9] &= 0xF8; t[10] &= 0x00;
    if (extracted != ftx_compute_crc(t, 96 - 14)) return 0;
    memcpy(payload, a91, 10);
    return 1;
}

/* ---------------------------------------------------------------------- OSD
 * Returns 1 and fills payload/metric when a CRC-passing codeword was found.
 * trials_out counts CRC checks, which is what the false-accept arithmetic in
 * the header is about. */
static int osd(const float llr[N], int order, uint8_t payload[10],
               double* metric_out, int* trials_out)
{
    int idx[N];
    for (int i = 0; i < N; i++) idx[i] = i;
    /* insertion sort by |llr| descending - N is 174, this is not the cost */
    for (int i = 1; i < N; i++) {
        int k = idx[i]; float v = fabsf(llr[k]); int j = i - 1;
        while (j >= 0 && fabsf(llr[idx[j]]) < v) { idx[j+1] = idx[j]; j--; }
        idx[j+1] = k;
    }

    uint8_t hard[N];
    for (int i = 0; i < N; i++) hard[i] = (llr[i] > 0) ? 1 : 0;

    /* Greedy most-reliable basis: take columns in reliability order, keep the
     * ones that raise the rank. Row-reduce as we go so the kept columns end
     * up as the identity. */
    row_t M[K];
    memcpy(M, G0, sizeof M);
    int basis[K], nb = 0;
    int pivot_row[K];

    for (int t = 0; t < N && nb < K; t++) {
        int c = idx[t];
        int pr = -1;
        for (int r = nb; r < K; r++) if (rget(&M[r], c)) { pr = r; break; }
        if (pr < 0) continue;                 /* column dependent - skip it */
        row_t tmp = M[nb]; M[nb] = M[pr]; M[pr] = tmp;
        for (int r = 0; r < K; r++)
            if (r != nb && rget(&M[r], c)) rxor(&M[r], &M[nb]);
        basis[nb] = c; pivot_row[nb] = nb; nb++;
    }
    if (nb < K) return 0;                     /* cannot happen for this code */

    /* order 0: the codeword the hard decisions on the basis imply */
    uint8_t base_info[K];
    for (int k = 0; k < K; k++) base_info[k] = hard[basis[k]];

    int    best = 0;
    double best_metric = 0;
    uint8_t best_payload[10];
    int trials = 0;

    for (int flip = -1; flip < (order >= 1 ? K : 0); flip++) {
        uint8_t info[K];
        memcpy(info, base_info, sizeof info);
        if (flip >= 0) info[flip] ^= 1;

        /* re-encode: codeword = sum of the reduced rows for set info bits */
        row_t acc; acc.w[0]=acc.w[1]=acc.w[2]=0;
        for (int k = 0; k < K; k++) if (info[k]) rxor(&acc, &M[pivot_row[k]]);

        uint8_t cw[N];
        for (int i = 0; i < N; i++) cw[i] = (uint8_t)rget(&acc, i);

        uint8_t pl[10];
        trials++;
        if (!crc_ok(cw, pl)) continue;

        double d = 0;
        for (int i = 0; i < N; i++) if (cw[i] != hard[i]) d += fabs((double)llr[i]);

        if (!best || d < best_metric) {
            best = 1; best_metric = d; memcpy(best_payload, pl, 10);
        }
    }

    *trials_out = trials;
    if (!best) return 0;
    memcpy(payload, best_payload, 10);
    *metric_out = best_metric;
    return 1;
}

int main(int argc,char** argv)
{
    if(argc<3){ fprintf(stderr,"usage: ft8_osd_proto <wav> <expected.txt>\n"); return 1; }
    static float sig[SR*20]; int n=SR*20, sr=SR;
    if(load_wav(sig,&n,&sr,argv[1])){ fprintf(stderr,"bad wav %s\n",argv[1]); return 1; }
    load_expected(argv[2]);
    build_generator();

    char texts[MAXMSG][64]; int ntext=0;
    char osd_only[MAXMSG][64]; double osd_metric[MAXMSG]; int n_osd_only=0;
    int n_ldpc=0, n_osd=0, trials_total=0, n_removed=0;
    int per_pass[8]; memset(per_pass,0,sizeof per_pass);
    double t_osd=0;

    float *scratch = malloc(sizeof(float)*(size_t)ft8_subtract_scratch_floats(FT8_NN));
    if(!scratch){ fprintf(stderr,"scratch alloc failed\n"); return 1; }

    for(int pass=0; pass<NPASS; pass++){
        monitor_t mon; monitor_config_t cfg={200,3000,sr,TIME_OSR,FREQ_OSR,FTX_PROTOCOL_FT8};
        monitor_init(&mon,&cfg);
        for(int p=0;p+mon.block_size<=n;p+=mon.block_size) monitor_process(&mon,sig+p);

        ftx_candidate_t cands[MAXC];
        int nc=ftx_find_candidates(&mon.wf,MAXC,cands,MINSCORE);

        /* Collected during the pass, subtracted after it. Subtracting while
         * still walking the candidate list would change the waterfall under
         * the list - ft8_subtract_bench.c records the same rule. */
        struct { uint8_t tones[FT8_NN]; float f0; int start; } todo[MAXC];
        int n_todo=0;

        for(int i=0;i<nc;i++){
            uint8_t payload[10];
            int from_osd=0;
            double metric=0;

            ftx_message_t m; ftx_decode_status_t st;
            memset(&st,0,sizeof st);
            if(ftx_decode_candidate(&mon.wf,&cands[i],ITERS,&m,&st)){
                n_ldpc++;
                memcpy(payload,m.payload,10);
            } else {
#ifdef OSD_OFF
                continue;
#else
                /* LDPC failed: this is the candidate OSD exists for */
                float llr[N];
                ftx_extract_likelihood_probe(&mon.wf,&cands[i],llr);
                int trials=0;
                double a=now_ms();
                int got=osd(llr,OSD_ORDER,payload,&metric,&trials);
                t_osd+=now_ms()-a; trials_total+=trials;
                if(!got) continue;
                from_osd=1;
#endif
            }

            ftx_message_t om; memset(&om,0,sizeof om);
            memcpy(om.payload,payload,10);
            char t[64]; ftx_message_offsets_t off;
            if(ftx_message_decode(&om,NULL,t,&off)!=FTX_MESSAGE_RC_OK) continue;

            int dup=0; for(int j=0;j<ntext;j++) if(match(texts[j],t)) dup=1;
            if(!dup && ntext<MAXMSG){
                snprintf(texts[ntext++],64,"%s",t);
                per_pass[pass<8?pass:7]++;
                if(from_osd){
                    n_osd++;
                    if(n_osd_only<MAXMSG){
                        osd_metric[n_osd_only]=metric;
                        snprintf(osd_only[n_osd_only++],64,"%s",t);
                    }
                }
            }

            /* Every decode feeds the next pass, OSD's included - that is the
             * whole question this harness exists to answer.
             *
             * NOTE a duplicate text still gets subtracted here, which is what
             * makes this harness's subtract-only row differ from
             * ft8_subtract_bench.c's: that one `continue`s on a duplicate
             * before collecting it. -DSUB_SKIP_DUP reproduces its behaviour
             * and is the check that the difference is this and not a defect. */
#ifdef SUB_SKIP_DUP
            if(dup) continue;
#endif
            if(pass+1<NPASS && n_todo<MAXC){
                ft8_encode(payload,todo[n_todo].tones);
                todo[n_todo].f0 = (mon.min_bin + cands[i].freq_offset
                                   + (float)cands[i].freq_sub / mon.wf.freq_osr)
                                  / mon.symbol_period;
                float t_sec = (cands[i].time_offset
                               + (float)cands[i].time_sub / mon.wf.time_osr)
                              * mon.symbol_period;
                todo[n_todo].start = ft8_subtract_start_sample(t_sec, SR);
                n_todo++;
            }
        }
        monitor_free(&mon);

        for(int i=0;i<n_todo;i++){
            ft8_subtract_signal(sig,n,todo[i].tones,FT8_NN,
                                todo[i].f0,todo[i].start,SR,scratch);
            n_removed++;
        }
        if(n_todo==0 && pass>0) break;      /* nothing new: stop early */
    }
    free(scratch);

    int hit=0; for(int i=0;i<n_exp;i++) for(int j=0;j<ntext;j++)
        if(match(exp_txt[i],texts[j])){ hit++; break; }

    /* OSD's own decodes split into "in the ground truth" and "not" - the
     * false-accept question the header raises, measured and not assumed. */
    int osd_in_truth=0;
    for(int i=0;i<n_osd_only;i++)
        for(int e=0;e<n_exp;e++) if(match(exp_txt[e],osd_only[i])){ osd_in_truth++; break; }

    for(int i=0;i<n_osd_only;i++){
        int in=0; for(int e=0;e<n_exp;e++) if(match(exp_txt[e],osd_only[i])) in=1;
        printf("  OSD %-9s metric=%8.2f  %s\n", in?"IN-TRUTH":"NOT-TRUTH",
               osd_metric[i], osd_only[i]);
    }

    printf("%-28s truth=%2d uniq=%2d hit=%2d | ldpc=%2d osd=%2d (truth %d/%d)"
           " | removed=%d p1=%d p2=%d | trials=%d osd_ms=%.1f\n",
           argv[1], n_exp, ntext, hit, n_ldpc, n_osd,
           osd_in_truth, n_osd, n_removed, per_pass[0], per_pass[1],
           trials_total, t_osd);
    return 0;
}
