// Host harness for the spot de-duplication in main/net/spots.c.
//
// Build (mingw/gcc, from the repo root):
//   gcc -O1 -Wall -Wextra -o spots_dedup.exe test/spots_dedup_harness.c
//   ./spots_dedup.exe <tsv>     # call<TAB>freq<TAB>src<TAB>age, one per line
//   ./spots_dedup.exe           # built-in self-tests only
//
// Why this exists: the collapse loop compacts an array in place while walking
// it, with an i-- rewind and a swap that moves the survivor into the earlier
// slot. That is exactly the shape of loop that looks right and silently drops
// or duplicates an entry, and it runs on the LVGL thread holding the store
// lock, where a bug is a wrong picture rather than a crash. The device build
// cannot easily be unit-tested, so the algorithm is mirrored here BYTE FOR
// BYTE and checked against real feed captures plus hand-built edge cases.
//
// KEEP IN STEP with get_in_range_locked() in main/net/spots.c. If that changes
// and this does not, this harness is worse than useless - it will keep passing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { SPOT_SRC_POTA = 0, SPOT_SRC_RBN } spot_source_t;

typedef struct {
    char          call[12];
    char          ref[10];
    uint32_t      freq_hz;
    int           mode;
    spot_source_t source;
    long long     heard_unix;
    bool          rbn_confirmed;
} spot_t;

#define SPOT_DUP_TOL_HZ 2000

// ---- mirrored from main/net/spots.c ----------------------------------------

static inline bool spot_same_station(const spot_t *a, const spot_t *b)
{
    uint32_t d = (a->freq_hz > b->freq_hz) ? a->freq_hz - b->freq_hz
                                           : b->freq_hz - a->freq_hz;
    return d <= SPOT_DUP_TOL_HZ && strcasecmp(a->call, b->call) == 0;
}

static int collapse(spot_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {
            if (!spot_same_station(&out[i], &out[j])) continue;
            if (out[i].source != SPOT_SRC_RBN && out[j].source == SPOT_SRC_RBN) {
                spot_t tmp = out[j]; out[j] = out[i]; out[i] = tmp;
            }
            if (out[i].source == SPOT_SRC_RBN && out[j].source != SPOT_SRC_RBN)
                out[j].rbn_confirmed = true;
            if (out[i].rbn_confirmed) out[j].rbn_confirmed = true;
            if (out[i].heard_unix > out[j].heard_unix)
                out[j].heard_unix = out[i].heard_unix;
            memmove(&out[i], &out[i + 1], (size_t)(n - i - 1) * sizeof(spot_t));
            n--;
            i--;
            break;
        }
    }
    return n;
}

// ---- checks ----------------------------------------------------------------

static int g_fail = 0;

static void check(bool cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); g_fail++; }
}

// The invariant that matters: no two survivors are the same station, every
// survivor was present in the input, and nothing that was NOT a duplicate got
// dropped. Verified against the input rather than against a expected-output
// literal, so the check does not encode the same mistake as the code.
static void verify(const spot_t *in, int n_in, const spot_t *out, int n_out)
{
    for (int i = 0; i < n_out; i++)
        for (int j = i + 1; j < n_out; j++)
            check(!spot_same_station(&out[i], &out[j]), "survivors still contain a duplicate pair");

    for (int i = 0; i < n_out; i++) {
        bool found = false;
        for (int j = 0; j < n_in; j++)
            if (strcmp(out[i].call, in[j].call) == 0 && out[i].freq_hz == in[j].freq_hz) { found = true; break; }
        check(found, "survivor was not in the input (fabricated entry)");
    }

    // Every input station must still be represented by exactly one survivor.
    for (int j = 0; j < n_in; j++) {
        int reps = 0;
        for (int i = 0; i < n_out; i++)
            if (spot_same_station(&in[j], &out[i])) reps++;
        check(reps == 1, "an input station is missing or over-represented among survivors");
    }
}

static spot_t mk(const char *call, uint32_t f, spot_source_t s, long long age)
{
    spot_t sp; memset(&sp, 0, sizeof sp);
    snprintf(sp.call, sizeof sp.call, "%s", call);
    sp.freq_hz = f; sp.source = s; sp.heard_unix = age;
    return sp;
}

static void selftests(void)
{
    printf("self-tests:\n");

    {   // POTA + RBN same freq -> one survivor, the POTA one, confirmed
        spot_t in[] = { mk("OK7DA/P", 14047000, SPOT_SRC_RBN, 50),
                        mk("OK7DA/P", 14047000, SPOT_SRC_POTA, 10) };
        spot_t w[8]; memcpy(w, in, sizeof in);
        int n = collapse(w, 2);
        verify(in, 2, w, n);
        check(n == 1, "POTA+RBN should collapse to 1");
        check(w[0].source == SPOT_SRC_POTA, "the POTA spot must be the survivor");
        check(w[0].rbn_confirmed, "survivor must be marked rbn_confirmed");
        check(w[0].heard_unix == 50, "survivor must take the later timestamp");
    }
    {   // reverse order - POTA first, RBN second
        spot_t in[] = { mk("SQ7N/P", 14047000, SPOT_SRC_POTA, 10),
                        mk("SQ7N/P", 14047000, SPOT_SRC_RBN, 90) };
        spot_t w[8]; memcpy(w, in, sizeof in);
        int n = collapse(w, 2);
        verify(in, 2, w, n);
        check(n == 1 && w[0].source == SPOT_SRC_POTA && w[0].rbn_confirmed, "order must not matter");
    }
    {   // RBN doubling itself 100 Hz apart - the measured real case
        spot_t in[] = { mk("JA1BJI", 14002300, SPOT_SRC_RBN, 10),
                        mk("JA1BJI", 14002400, SPOT_SRC_RBN, 20) };
        spot_t w[8]; memcpy(w, in, sizeof in);
        int n = collapse(w, 2);
        verify(in, 2, w, n);
        check(n == 1, "RBN self-duplicate should collapse");
        check(!w[0].rbn_confirmed, "RBN+RBN is not 'confirmed' - that flag means two SOURCES agree");
    }
    {   // three of the same station, mixed sources
        spot_t in[] = { mk("OK7DA/P", 14047000, SPOT_SRC_RBN, 10),
                        mk("OK7DA/P", 14047100, SPOT_SRC_RBN, 20),
                        mk("OK7DA/P", 14047000, SPOT_SRC_POTA, 5) };
        spot_t w[8]; memcpy(w, in, sizeof in);
        int n = collapse(w, 3);
        verify(in, 3, w, n);
        check(n == 1 && w[0].source == SPOT_SRC_POTA && w[0].rbn_confirmed, "3-way collapse to the POTA spot");
    }
    {   // same call, far apart in frequency = two genuinely different spots
        spot_t in[] = { mk("W1AW", 14020000, SPOT_SRC_RBN, 10),
                        mk("W1AW", 14060000, SPOT_SRC_RBN, 20) };
        spot_t w[8]; memcpy(w, in, sizeof in);
        int n = collapse(w, 2);
        verify(in, 2, w, n);
        check(n == 2, "same call >2 kHz apart must NOT be merged");
    }
    {   // different calls on the same frequency must never merge
        spot_t in[] = { mk("OK7DA/P", 14047000, SPOT_SRC_POTA, 10),
                        mk("SQ7N/P",  14047000, SPOT_SRC_POTA, 20) };
        spot_t w[8]; memcpy(w, in, sizeof in);
        int n = collapse(w, 2);
        verify(in, 2, w, n);
        check(n == 2, "different calls on one frequency must both survive");
    }
    {   // exactly at the tolerance edge, and one hertz past it
        spot_t a[] = { mk("G0ABC", 14000000, SPOT_SRC_RBN, 1),
                       mk("G0ABC", 14002000, SPOT_SRC_RBN, 2) };
        spot_t wa[4]; memcpy(wa, a, sizeof a);
        check(collapse(wa, 2) == 1, "exactly 2000 Hz apart should merge");
        spot_t b[] = { mk("G0ABC", 14000000, SPOT_SRC_RBN, 1),
                       mk("G0ABC", 14002001, SPOT_SRC_RBN, 2) };
        spot_t wb[4]; memcpy(wb, b, sizeof b);
        check(collapse(wb, 2) == 2, "2001 Hz apart should not merge");
    }
    {   // empty and single-entry inputs must not walk off the array
        spot_t w[2];
        check(collapse(w, 0) == 0, "empty input");
        w[0] = mk("K1ABC", 14030000, SPOT_SRC_RBN, 1);
        check(collapse(w, 1) == 1, "single entry");
    }
    {   // every entry identical - the worst case for the rewind logic
        spot_t in[6]; spot_t w[6];
        for (int i = 0; i < 6; i++) in[i] = mk("N0DUP", 14025000, SPOT_SRC_RBN, i);
        memcpy(w, in, sizeof in);
        int n = collapse(w, 6);
        verify(in, 6, w, n);
        check(n == 1, "six identical entries collapse to one");
        check(w[0].heard_unix == 5, "and keep the latest timestamp");
    }
    {   // callsign match must be case-insensitive
        spot_t in[] = { mk("oh2xx", 14030000, SPOT_SRC_RBN, 1),
                        mk("OH2XX", 14030100, SPOT_SRC_POTA, 2) };
        spot_t w[4]; memcpy(w, in, sizeof in);
        int n = collapse(w, 2);
        check(n == 1 && w[0].source == SPOT_SRC_POTA, "case-insensitive call match");
    }
    printf("  %s\n", g_fail ? "FAILURES ABOVE" : "all self-tests pass");
}

int main(int argc, char **argv)
{
    selftests();

    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { perror(argv[1]); return 2; }
        static spot_t in[512], w[512];
        int n_in = 0;
        char line[256];
        while (n_in < 512 && fgets(line, sizeof line, f)) {
            char call[32], src[16];
            unsigned long fr; long long age;
            if (sscanf(line, "%31s %lu %15s %lld", call, &fr, src, &age) != 4) continue;
            in[n_in] = mk(call, (uint32_t)fr,
                          strcmp(src, "rbn") == 0 ? SPOT_SRC_RBN : SPOT_SRC_POTA, -age);
            n_in++;
        }
        fclose(f);
        memcpy(w, in, sizeof(spot_t) * (size_t)n_in);
        int n_out = collapse(w, n_in);
        printf("\nlive capture: %d in -> %d out (%d collapsed)\n", n_in, n_out, n_in - n_out);
        for (int i = 0; i < n_out; i++)
            if (w[i].rbn_confirmed)
                printf("  confirmed by RBN: %-10s %lu Hz\n", w[i].call, (unsigned long)w[i].freq_hz);
        verify(in, n_in, w, n_out);
        printf("  invariants: %s\n", g_fail ? "VIOLATED" : "hold");
    }
    return g_fail ? 1 : 0;
}
