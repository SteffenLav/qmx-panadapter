/* Host harness for main/ft8_wsjtx.c (the WS 3.2.1 FT8 decoder port).
 * Reads one 12 kHz mono int16 WAV, decodes it the way jt9 -8 -d 3 does, and
 * prints one line per decode:
 *   MSG\t<freq>\t<text>      plain decode
 *   APMSG\t<freq>\t<text>    a priori decode (jt9 marks these "aN")
 * The samples go in as the raw int16 values, unscaled: jt9 does the same,
 * and ft8b's false-decode screen depends on the absolute level.
 * Usage: ft8_wsjtx_harness <wav> [ignored]   (2nd arg: ft8_vs_wsjtx.py API)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ft8_wsjtx.h"

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

/* Callsign hash table: the firmware's main/ft8_hash.c without the RTOS
 * parts - linear scan, 12/10-bit hashes are the top bits of n22. jt9 starts
 * each process (one WAV) with an empty table, so does this. */
/* Newest wins, as in packjt77: calls10/calls12 are indexed by the hash
 * (the last save overwrites) and calls22 puts each save at the top. So a
 * save of a call already present moves it to the newest slot, and lookup
 * scans newest first. First-match resolved 3 colliding 12-bit hashes to
 * the older call (<DD1OO> where jt9 had <DG3NCZ>), 2026-11-21. */
static struct { char call[12]; uint32_t n22; } htab[1000];
static int hn;
static void hash_save(const char* c, uint32_t n22)
{
    if (!c || !c[0]) return;
    for (int i = 0; i < hn; i++)
        if (strncmp(htab[i].call, c, 11) == 0) {
            memmove(&htab[i], &htab[i + 1], (size_t)(hn - 1 - i) * sizeof htab[0]);
            hn--;
            break;
        }
    if (hn == 1000) { memmove(&htab[0], &htab[1], 999 * sizeof htab[0]); hn--; }
    strncpy(htab[hn].call, c, 11); htab[hn].call[11] = 0; htab[hn].n22 = n22;
    hn++;
}
static bool hash_lookup(ftx_callsign_hash_type_t t, uint32_t h, char* c)
{
    c[0] = 0;
    for (int i = hn - 1; i >= 0; i--) {
        uint32_t v = htab[i].n22;
        if (t == FTX_CALLSIGN_HASH_12_BITS) v >>= 10;
        else if (t == FTX_CALLSIGN_HASH_10_BITS) v >>= 12;
        if (v == h) { strcpy(c, htab[i].call); return true; }
    }
    return false;
}
static ftx_callsign_hash_interface_t hif = { .lookup_hash = hash_lookup, .save_hash = hash_save };

static int ndec, nap;
static void on_decode(void* ctx, float sync, int snr, float dt, float freq,
                      const char* msg, int ap, float qual)
{
    (void)ctx; (void)sync; (void)qual;
    printf("%s\t%.1f\t%s\t%d\t%.2f\n", ap ? "APMSG" : "MSG", freq, msg, snr, dt);
    if (ap) nap++; else ndec++;
}

/* jt9.f90: nutc is the 6 digits before ".wav" (4 if preceded by '_'). */
static int nutc_from_name(const char* path)
{
    const char* e = strstr(path, ".wav");
    if (!e) e = strstr(path, ".WAV");
    if (!e || e - path < 6) return 0;
    char t[8] = {0};
    if (e[-5] == '_') memcpy(t, e - 4, 4); else memcpy(t, e - 6, 6);
    for (char* q = t; *q; q++) if (*q < '0' || *q > '9') return 0;
    return atoi(t);
}

/* One WAV, or several in ONE run: like jt9 given several files, the hash
 * table and the a7 (previous-cycle) state carry from file to file. With
 * several, each file's lines follow a "FILE\t<path>" line. */
int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <wav> [<wav> ...]\n", argv[0]); return 1; }
    static int16_t iwave[WSX_NMAX];
    clock_t t0 = clock();
    for (int f = 1; f < argc; f++) {
        memset(iwave, 0, sizeof iwave);
        if (load_wav(iwave, WSX_NMAX, argv[f]) < 0) { fprintf(stderr, "bad wav %s\n", argv[f]); return 1; }
        wsx_params_t p;
        wsx_default_params(&p);
        p.hash_if = &hif;
        p.nutc = nutc_from_name(argv[f]);
        /* jt9's -c / -x / -g / -f, for an a8 referee run */
        if (getenv("WSX_MYCALL")) snprintf(p.mycall, sizeof p.mycall, "%s", getenv("WSX_MYCALL"));
        if (getenv("WSX_HISCALL")) snprintf(p.hiscall, sizeof p.hiscall, "%s", getenv("WSX_HISCALL"));
        if (getenv("WSX_HISGRID")) snprintf(p.hisgrid, sizeof p.hisgrid, "%s", getenv("WSX_HISGRID"));
        if (getenv("WSX_NFQSO")) p.nfqso = atoi(getenv("WSX_NFQSO"));
        if (getenv("WSX_NQSO")) p.nqso_progress = atoi(getenv("WSX_NQSO"));   /* jt9 -Q */
        if (argc > 2) printf("FILE\t%s\n", argv[f]);
        wsx_decode_slot(iwave, &p, on_decode, NULL);
        fflush(stdout);
    }
    wsx_prof_print();
    fprintf(stderr, "decodes=%d ap=%d ms=%.0f\n", ndec, nap, 1000.0 * (clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
