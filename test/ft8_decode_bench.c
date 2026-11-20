/*
 * FT8 decode-COST bench - how long a slot's decode takes, and what it buys.
 *
 * ft8_test_harness.c answers "did we decode it". This answers "what did it
 * cost", because dec_ms is the real limit on replying inside an FT8 slot:
 * capture alone is ~13.2 s of 15 s, so the whole reply window is 2260 ms into
 * the next slot. Randy N4OPI's busy 20 m slot, 2026-10-03:
 * cap 13281 + stft 953 + dec 5441 = 19675 ms - 2415 ms past the deadline.
 *
 * It mirrors the device's own settings deliberately: FT8_MAX_CANDIDATES 140,
 * FT8_FIND_MIN_SCORE 5, 30 LDPC iterations. Change those here if you change
 * them in main/ft8_test.c, or the number stops meaning anything.
 *
 * Reports per WAV: candidates, unique decodes, ground-truth hits, and the
 * decode time split between SUCCEEDING and FAILING candidates - the split that
 * matters, because bp_decode() exits early only on success, so a failing
 * candidate always burns the full iteration count. Measured 2026-10-04: a
 * failure cost 0.26 ms against 0.02 ms for a success, so a busy slot is
 * ~entirely paid for by candidates that decode nothing.
 *
 * Build (host, not the device):
 *   gcc -O2 -o ft8_decode_bench test/ft8_decode_bench.c \
 *       components/ft8_lib/ft8/{decode,encode,ldpc,text,message,crc,constants}.c \
 *       components/ft8_lib/ft8/{js8_codec,js8_tables}.c \
 *       components/ft8_lib/common/monitor.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -Icomponents/ft8_lib -Icomponents/ft8_lib/ft8 \
 *       -Icomponents/ft8_lib/common -Icomponents/ft8_lib/fft -lm
 *
 * Run over the whole corpus from test/wav_reference:
 *   for w in *.wav; do ./ft8_decode_bench $w ${w%.wav}.txt; done
 *
 * Not a pass/fail harness, so run_harnesses.py does not run it - it has no
 * correct answer, only a number you compare between two builds.
 *
 * ⛔ THE TWO JS8 FILES ARE NOT OPTIONAL. decode.c grew a JS8 branch after
 * this build line was written, and without them the link fails on
 * js8_check_payload_crc, kJS8_LDPC_colorder, kJS8_Costas_original and three
 * more. A stale build line in a comment reads as a broken harness.
 *
 * Baseline over test/wav_reference (the 22 WAVs that have a .txt ground
 * truth), measured 2026-11-20 on the Ryzen box, gcc -O2:
 *   236 unique decodes, 235 of 345 ground-truth messages (68.1 %)
 *
 * ⛔ DO NOT COMPARE THAT ROW WITH A PER-FILE SUM OF THIS BENCH'S OWN OUTPUT.
 * Summing the printed uniq/truth/hit columns across the 22 files, same binary,
 * same settings, gives 264 uniq and 260 of 362 - not 236 and 235 of 345. The
 * truth count differs too, so this is an AGGREGATION difference and not a
 * decoder change: a per-file sum counts a message appearing in two files
 * twice, and how the 236/345 row above was totalled is not recorded.
 * UNEXPLAINED and left that way, because only whoever produced that row can
 * say. 362/260 is the figure ft8_subtract_bench.c, ft8_osd_reach_harness.c
 * and ft8_osd_proto_harness.c all agree on, and those three are mutually
 * validated; quote one of them, or re-derive this row's method before
 * quoting it.
 *   stft 141.3 + find 52.5 + dec 199.3 = 393.1 ms for all 22 slots
 * The 110 missed ground-truth messages are the size of the prize, and what
 * subtract-and-repeat and OSD would be for; neither exists in ft8_lib.
 *
 * SWEEP 2026-11-20, same 22 WAVs, one binary per setting (-D overrides):
 *
 *   setting        hit/345  uniq   stft ms   dec ms
 *   2x2 baseline      235    236     142      200
 *   cand 300          235    236     146      487
 *   cand 600          235    236     143     1031
 *   minscore 2        235    236     144      200
 *   iters 60          235    236     147      202
 *   time_osr 4        241    252     298      176    <-- the only winner
 *   freq_osr 4        167    163     305      207
 *   freq_osr 1        175    173      72      228
 *
 * [*] CANDIDATES, MIN SCORE AND LDPC ITERATIONS ARE NOT BINDING, and this is
 * an INDEPENDENT replication of what the device measured: ft8_test.c records
 * 140 -> 300 as a clean negative on 20 m. Here 140 -> 600 moves nothing at
 * all while decode cost goes up 5x. Stop proposing these three.
 *
 * [*] time_osr 2 -> 4 IS A REAL GAIN: +16 unique decodes (+6.8 %), and decode
 * gets CHEAPER (176 vs 200 ms) because better time resolution means fewer
 * candidates burn the full LDPC iteration count before failing. STFT doubles,
 * 142 -> 298 ms, which on the device is PSRAM and core-1 time it does not
 * have, and on a host is nothing.
 *
 * [!] THE EXTRA DECODES ARE GENUINE, NOT FALSE POSITIVES. uniq rises 236 ->
 * 252 while hit rises 235 -> 241, so 11 decodes are not in the .txt. Every
 * decode ft8_lib returns has passed a 14-bit CRC (decode.c:815), so the
 * expected number of random passes across 252 decodes is 252/16384 = 0.015.
 * The ground truth is WSJT-X's own output and is simply not exhaustive.
 *
 * [*] WHY freq_osr 4 IS WORSE, AND WHY 2 IS THE OPTIMUM - explained, not
 * just observed. The decoder reads ONE sub-bin per tone: get_cand_mag()
 * (decode.c:182) selects a single (time_sub, freq_sub) plane and every
 * magnitude after that is read inside it. The STFT bin is 6.25/freq_osr Hz
 * wide against FT8's 6.25 Hz tone spacing, so:
 *
 *     freq_osr 1   6.25 Hz   one bin per tone, but NO sub-bin correction -
 *                            a tone sitting between bins is scalloped away
 *     freq_osr 2   3.125 Hz  half-bin correction available, bin still wide
 *                            enough to hold most of one tone   <-- optimum
 *     freq_osr 4   1.5625 Hz bin NARROWER than the tone, so the one bin the
 *                            decoder reads holds a fraction of its energy
 *
 * The measurements follow that exactly: 175, 235, 167. Raising the candidate
 * cap recovers only 6 of the 68 lost at freq_osr 4 (173 at cand 280, and no
 * further at 560), so candidate dilution is a small second-order effect and
 * the lost energy is the cause. This is a property of the decoder's design,
 * not a bug with a cheap fix - reading all freq_osr sub-bins per tone would
 * be the fix, and that is a change to ft8_lib's core.
 *
 * [!] time_osr 8 and 4x4 CRASH partway through the corpus (the run stops at
 * truth 139 and 121 of 345) while a single file at 4x4 completes. Unexplained,
 * and it is a harness limit, not a decoder result - do not quote those two
 * rows as measurements.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <ft8/decode.h>
#include <ft8/message.h>
#include <common/monitor.h>
#if FT8_OSD_ORDER0
#include <ft8/osd.h>
#endif

#define SR 12000
/* Overridable with -D so one binary per setting can be swept against the same
 * corpus. The DEFAULTS must stay equal to the device's own constants in
 * main/ft8_test.c, or the number this prints stops meaning anything - that is
 * what the file header promises and it is the whole point of the harness. */
#ifndef MAXC
#define MAXC 140          /* FT8_MAX_CANDIDATES */
#endif
#ifndef MINSCORE
#define MINSCORE 5        /* FT8_FIND_MIN_SCORE */
#endif
#ifndef ITERS
#define ITERS 30          /* FT8_LDPC_MAX_ITERS */
#endif
#ifndef TIME_OSR
#define TIME_OSR 2
#endif
#ifndef FREQ_OSR
#define FREQ_OSR 2
#endif

static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000.0 + ts.tv_nsec/1e6; }

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

/* ground truth: lines "HHMMSS SNR CONF FREQ ~ MESSAGE" */
#define MAXE 200
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

int main(int argc,char** argv){
    if(argc<2){ fprintf(stderr,"usage: bench <wav> [expected.txt]\n"); return 1; }
    static float sig[SR*20]; int n=SR*20, sr=SR;
    if(load_wav(sig,&n,&sr,argv[1])<0){ fprintf(stderr,"bad wav %s\n",argv[1]); return 1; }
    if(argc>2) load_expected(argv[2]);

#if FT8_OSD_ORDER0
    ft8_osd_init();
#endif
    monitor_t mon; monitor_config_t cfg={200,3000,sr,TIME_OSR,FREQ_OSR,FTX_PROTOCOL_FT8};
    monitor_init(&mon,&cfg);
    double t0=now_ms();
    for(int p=0;p+mon.block_size<=n;p+=mon.block_size) monitor_process(&mon,sig+p);
    double t_stft=now_ms()-t0;

    ftx_candidate_t cands[MAXC];
    t0=now_ms();
    int nc=ftx_find_candidates(&mon.wf,MAXC,cands,MINSCORE);
    double t_find=now_ms()-t0;

    char texts[MAXC][64]; int ntext=0;
    double t_ok=0,t_fail=0; int n_ok=0,n_fail=0;
    double t_dec0=now_ms();
    for(int i=0;i<nc;i++){
        ftx_message_t m; ftx_decode_status_t st;
        double a=now_ms();
        bool ok=ftx_decode_candidate(&mon.wf,&cands[i],ITERS,&m,&st);
        double d=now_ms()-a;
        if(ok){ t_ok+=d; n_ok++;
            char t[64]; ftx_message_offsets_t off;
            if(ftx_message_decode(&m,NULL,t,&off)==FTX_MESSAGE_RC_OK && ntext<MAXC){
                int dup=0; for(int j=0;j<ntext;j++) if(!strcmp(texts[j],t)) dup=1;
                if(!dup) snprintf(texts[ntext++],64,"%s",t);
            }
        } else { t_fail+=d; n_fail++; }
    }
    double t_dec=now_ms()-t_dec0;

    int hit=0; for(int i=0;i<n_exp;i++) for(int j=0;j<ntext;j++)
        if(match(exp_txt[i],texts[j])){ hit++; break; }

    printf("%-28s cand=%3d uniq=%2d truth=%2d hit=%2d | stft=%6.1f find=%5.1f dec=%7.1f ms"
           " | ok n=%2d %6.1fms (%.2f/cand) fail n=%3d %7.1fms (%.2f/cand)\n",
           argv[1], nc, ntext, n_exp, hit, t_stft, t_find, t_dec,
           n_ok, t_ok, n_ok? t_ok/n_ok:0.0, n_fail, t_fail, n_fail? t_fail/n_fail:0.0);
    monitor_free(&mon);
    return 0;
}
