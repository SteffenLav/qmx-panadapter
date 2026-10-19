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

#define SR 12000
#define MAXC 140
#define MINSCORE 5
#define ITERS 30

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

    monitor_t mon; monitor_config_t cfg={200,3000,sr,2,2,FTX_PROTOCOL_FT8};
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
