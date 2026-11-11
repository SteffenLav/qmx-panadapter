/* WSPR decoder: NOTHING may allocate inside a cycle (docs/wspr-memory.md).
 *
 * 2026-11-11, v1.26.0, bench1: every WSPR cycle after the first decoded
 * nothing - "NO MEMORY for the candidate search" - because the search, stage 1,
 * stage 2 and the capture buffer were all allocated and freed inside the cycle,
 * and PSRAM fragmentation decided each one. Three earlier fixes moved those
 * allocations around instead of removing them. This harness is the gate that
 * keeps them removed: it links the REAL decoder with malloc/calloc/realloc/free
 * wrapped, attaches the arena once (page entry), runs three full cycles on the
 * WSJT reference recording - search, decode, subtract, search again, decode -
 * and fails on a single allocation between page entry and page exit.
 *
 * It also fails if the search rebuilds its kiss_fftr cfg inside a cycle: that
 * is computation, not allocation, but about 1.3 s of it per search on the P4
 * (28891b06, measured on bench1 2026-11-11).
 *
 * It also checks the arena is the ONLY source: with it detached the search
 * must say WSPR_CANDS_NOMEM, not quietly fall back to malloc.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I components/ft8_lib \
 *       -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=kiss_fftr_alloc \
 *       -o wspr_noalloc_harness test/wspr_noalloc_harness.c \
 *       main/wspr_proto.c main/wspr_fano.c main/wspr_decode.c \
 *       main/wspr_subtract.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -lm && ./wspr_noalloc_harness
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wspr_decode.h"
#include "wspr_subtract.h"

/* ---- allocation counter (GNU ld --wrap) -------------------------------- */
void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void  __real_free(void *p);

static int g_gate;          /* 1 between page entry and page exit */
static int g_allocs, g_frees;

void *__wrap_malloc(size_t n)            { if (g_gate) g_allocs++; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b)  { if (g_gate) g_allocs++; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n)  { if (g_gate) g_allocs++; return __real_realloc(p, n); }
void  __wrap_free(void *p)               { if (g_gate && p) g_frees++; __real_free(p); }

#include "fft/kiss_fftr.h"
static int g_cfg_builds;    /* kiss_fftr_alloc() calls that BUILD (mem given) */
kiss_fftr_cfg __real_kiss_fftr_alloc(int nfft, int inv, void *mem, size_t *lenmem);
kiss_fftr_cfg __wrap_kiss_fftr_alloc(int nfft, int inv, void *mem, size_t *lenmem)
{
    if (g_gate && mem) g_cfg_builds++;
    return __real_kiss_fftr_alloc(nfft, inv, mem, lenmem);
}

static int g_fail;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS  "); } else { printf("  FAIL  "); g_fail++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int16_t *load_wav_mono16(const char *path, long *n_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);
    long n = bytes / 2;
    int16_t *s = malloc((size_t)n * sizeof(int16_t));
    if (!s || fread(s, sizeof(int16_t), (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f);
    *n_out = n;
    return s;
}

/* One WSPR cycle as wspr_rx.c runs it: search, decode every candidate,
 * subtract what decoded, search again, decode again. Returns decodes. */
static int run_cycle(int16_t *pcm, long n)
{
    int decodes = 0;
    wspr_decode_begin_cycle();
    for (int pass = 0; pass < 2; pass++) {
        wspr_freq_candidate_t c[20];
        int nc = wspr_find_candidates(pcm, n, 1350.0, 1650.0, c, 20);
        if (nc < 0) return -1;
        for (int i = 0; i < nc; i++) {
            wspr_decode_result_t r;
            wspr_decode_candidate(pcm, n, c[i].freq_hz, &r);
            if (!r.ok) continue;
            decodes++;
            uint8_t tones[WSPR_NSYM];
            if (wspr_tones_from_message(r.callsign, r.grid, r.power_dbm, tones))
                wspr_subtract(pcm, n, r.freq_hz, r.best_dt_samples, tones);
        }
    }
    return decodes;
}

int main(void)
{
    const char *path = "test/wav_reference/wspr/150426_0918.wav";
    long n = 0;
    int16_t *orig = load_wav_mono16(path, &n);
    int16_t *pcm = orig ? malloc((size_t)n * sizeof(int16_t)) : NULL;
    if (!orig || !pcm) { printf("FAIL  could not load %s\n", path); return 1; }

    const size_t need = wspr_decode_scratch_bytes(n);
    const size_t big  = wspr_decode_scratch_largest(n);
    printf("arena for %ld samples: %zu bytes (%.2f MB), largest block %zu\n",
           n, need, need / 1048576.0, big);
    /* Ceilings, so growth is a decision and not a surprise (docs/wspr-memory.md).
     * The LARGEST BLOCK is the one that decides on the device: bench1's PSRAM
     * never offered more than 2304 KB contiguous with the page's buffers in
     * place, and a single 2.56 MB arena never fitted (2026-11-11). */
    CHECK(need <= 3150000, "arena %zu B is within the 3,150,000 B budget", need);
    CHECK(big <= 1400000, "largest block %zu B is within 1,400,000 B (device ceiling 2304 KB)", big);

    /* Page entry: the ONE allocation. */
    CHECK(wspr_decode_scratch_init(n), "arena allocated at page entry");

    g_gate = 1; g_allocs = g_frees = 0; g_cfg_builds = 0;
    int per_cycle[3];
    for (int cyc = 0; cyc < 3; cyc++) {
        memcpy(pcm, orig, (size_t)n * sizeof(int16_t));   /* a new capture */
        wspr_decode_capture_changed();
        per_cycle[cyc] = run_cycle(pcm, n);
    }
    g_gate = 0;

    CHECK(g_allocs == 0 && g_frees == 0,
          "%d allocation(s) and %d free(s) inside three cycles - must be 0", g_allocs, g_frees);
    CHECK(g_cfg_builds == 0,
          "%d kiss_fftr cfg build(s) inside three cycles - must be 0 (built at page entry)", g_cfg_builds);
    CHECK(per_cycle[0] >= 6, "cycle 1 decoded %d (the reference window gives >= 6)", per_cycle[0]);
    CHECK(per_cycle[0] == per_cycle[1] && per_cycle[1] == per_cycle[2],
          "every cycle decodes the same: %d / %d / %d", per_cycle[0], per_cycle[1], per_cycle[2]);

    /* Page exit, then the arena must be the only source. */
    wspr_decode_scratch_deinit();
    g_gate = 1; g_allocs = 0;
    wspr_freq_candidate_t c[4];
    memcpy(pcm, orig, (size_t)n * sizeof(int16_t));
    int nc = wspr_find_candidates(pcm, n, 1350.0, 1650.0, c, 4);
    g_gate = 0;
    CHECK(nc == WSPR_CANDS_NOMEM && g_allocs == 0,
          "no arena: the search says NOMEM (%d) and allocates nothing (%d)", nc, g_allocs);

    free(pcm);
    free(orig);
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
