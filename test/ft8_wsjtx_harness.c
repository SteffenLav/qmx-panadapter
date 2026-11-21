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
static struct { char call[12]; uint32_t n22; } htab[256];
static int hnext;
static void hash_save(const char* c, uint32_t n22)
{
    if (!c || !c[0]) return;
    for (int i = 0; i < 256; i++) if (htab[i].call[0] && strncmp(htab[i].call, c, 11) == 0) return;
    strncpy(htab[hnext].call, c, 11); htab[hnext].call[11] = 0; htab[hnext].n22 = n22;
    hnext = (hnext + 1) % 256;
}
static bool hash_lookup(ftx_callsign_hash_type_t t, uint32_t h, char* c)
{
    c[0] = 0;
    for (int i = 0; i < 256; i++) {
        if (!htab[i].call[0]) continue;
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

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <wav>\n", argv[0]); return 1; }
    static int16_t iwave[WSX_NMAX];
    memset(iwave, 0, sizeof iwave);
    if (load_wav(iwave, WSX_NMAX, argv[1]) < 0) { fprintf(stderr, "bad wav %s\n", argv[1]); return 1; }
    wsx_params_t p;
    wsx_default_params(&p);
    p.hash_if = &hif;
    clock_t t0 = clock();
    wsx_decode_slot(iwave, &p, on_decode, NULL);
    wsx_prof_print();
    fprintf(stderr, "decodes=%d ap=%d ms=%.0f\n", ndec, nap, 1000.0 * (clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
