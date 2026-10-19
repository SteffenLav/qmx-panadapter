/* Host test for the worked-call cache in main/adif/adif_log.c.
 *
 *   sh test/build_adif_worked_cache.sh
 *
 * ⛔ A SCRIPT, NOT A gcc LINE, and tools/run_harnesses.py understands that.
 * Two things here cannot be one command: shim.c must NOT see the path-redirect
 * macros that are force-included into the firmware source, and the binary has
 * to run more than once because two cases need a virgin process (the cache
 * capacity is static and only ever grows).
 *
 * ⛔ WHY THIS EXISTS. Gyula HA3HZ, 2026-10-17: with ~1000 QSOs logged, a
 * station he had just worked kept showing as NOT worked before. Deleting the
 * log made it correct again. The cause was a FIXED 1024-entry cache whose
 * overflow branch did nothing, in silence - so every QSO after the table
 * filled went to the file and never to the cache. The defect was invisible to
 * every test that used a small log, which is the whole reason the sizes below
 * are large and awkward rather than round.
 *
 * Links the REAL adif_log.c. A harness that reimplemented the cache would
 * test the reimplementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "adif_log.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

extern void shim_set_spiffs_dir(const char *dir);
extern int  shim_lock_depth;

static int fails;

static void expect(const char *what, int got, int want)
{
    if (got == want) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %s: got %d want %d\n", what, got, want);
    fails++;
}

/* ------------------------------------------------------------------ */

static char s_dir[512];
static char s_log[768];

/* Write an ADIF log of n QSOs directly, the way adif_log.c's own writer does.
 * Callsigns are unique and ordinary-looking; the band rotates over four so the
 * (call, band) pair - not the call - is what the cache holds.
 */
static const char *BANDS[4] = { "20M", "40M", "15M", "80M" };

static void make_call(char *out, int i)
{
    /* "AA0AAA".."ZZ9ZZZ"-ish: 6 chars, well inside ADIF_CALL_MAX. */
    sprintf(out, "%c%c%d%c%c%c",
            'A' + (i / 17576) % 26, 'A' + (i / 676) % 26, i % 10,
            'A' + (i / 26) % 26, 'A' + i % 26, 'A' + (i / 169) % 26);
}

static void write_record(FILE *f, const char *call, const char *band)
{
    fprintf(f, "<CALL:%zu>%s <BAND:%zu>%s <MODE:3>FT8 "
               "<QSO_DATE:8>20261017 <TIME_ON:6>120000 "
               "<STATION_CALLSIGN:6>OZ1LAV <EOR>\n",
            strlen(call), call, strlen(band), band);
}

static void write_log(int n)
{
    FILE *f = fopen(s_log, "w");
    if (!f) { printf("  FAIL cannot write %s\n", s_log); fails++; return; }
    fprintf(f, "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n");
    for (int i = 0; i < n; i++) {
        char call[16];
        make_call(call, i);
        write_record(f, call, BANDS[i % 4]);
    }
    fclose(f);
}

static void fresh(int n_records)
{
    remove(s_log);
    write_log(n_records);
    shim_set_spiffs_dir(s_dir);
    adif_log_init();
}

/* The frequency adif_log.c maps to each band, so the band-aware lookup can be
 * driven by its own freq_to_band(). Mid-band, well away from an edge. */
static uint32_t band_hz(const char *band)
{
    if (!strcmp(band, "20M")) return 14074000;
    if (!strcmp(band, "40M")) return  7074000;
    if (!strcmp(band, "15M")) return 21074000;
    if (!strcmp(band, "80M")) return  3573000;
    return 0;
}

static bool worked_i(int i)
{
    char call[16];
    make_call(call, i);
    return adif_log_contains_call_on_band(call, band_hz(BANDS[i % 4]));
}

/* ------------------------------------------------------------------ */

/* THE REGRESSION. 2000 QSOs is past the old fixed 1024 and past Gyula's
 * ~1000, and under the ~5800 a full 1 MB SPIFFS could hold. Every one of them
 * must be found, on its own band. Against the old code the first 1024 passed
 * and the rest failed - which is exactly the shape of his report. */
static void test_large_log(void)
{
    printf("large log (2000 QSOs, past the old 1024 ceiling)\n");
    fresh(2000);
    expect("count", adif_log_count(), 2000);

    int missing = 0;
    for (int i = 0; i < 2000; i++) if (!worked_i(i)) missing++;
    expect("every logged station reads as worked", missing, 0);

    /* The one at 1500 is the one the old code lost. Named on its own so a
       regression says WHICH end broke, not just "some". */
    expect("QSO #1500 (was past the old ceiling)", worked_i(1500), 1);
    expect("QSO #10 (was inside it)",              worked_i(10),   1);
}

/* A call logged on 20M is NOT worked before on 40M. The band half of the pair
 * has to survive the growth, not just the call. */
static void test_band_awareness(void)
{
    printf("band awareness across the growth\n");
    fresh(1500);

    char call[16];
    make_call(call, 1400);                      /* logged on BANDS[1400%4] */
    const char *logged = BANDS[1400 % 4];
    const char *other  = BANDS[(1400 + 1) % 4];

    expect("worked on its own band",  adif_log_contains_call_on_band(call, band_hz(logged)), 1);
    expect("NOT worked on another",   adif_log_contains_call_on_band(call, band_hz(other)),  0);
    expect("any-band lookup finds it", adif_log_contains_call(call), 1);
}

/* ONE CALL, TWO BANDS - both pairs must be in the cache.
 *
 * ⛔ Added after a mutation survived. The cache dedupes on the (call, band)
 * PAIR, but the hash is of the CALL only, so a dedupe that stopped at the
 * hash would keep the first band and silently drop the second - and every
 * other test here logged each call on exactly one band, so none of them could
 * see it. The symptom on the air would be the worst kind: a station worked on
 * 20M reads as worked on 40M too, and "Never auto-call worked-before" refuses
 * a band-slot that is genuinely new. */
static void test_same_call_two_bands(void)
{
    printf("one call on two bands (the pair is the key, not the call)\n");
    remove(s_log);
    FILE *f = fopen(s_log, "w");
    if (!f) { printf("  FAIL cannot write %s\n", s_log); fails++; return; }
    fprintf(f, "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n");
    /* Bulk, so the pair under test sits well past the old 1024 boundary. */
    for (int i = 0; i < 1500; i++) {
        char call[16];
        make_call(call, i);
        write_record(f, call, BANDS[i % 4]);
    }
    write_record(f, "OZ1LAV", "20M");
    write_record(f, "OZ1LAV", "40M");
    fclose(f);

    shim_set_spiffs_dir(s_dir);
    adif_log_init();

    expect("found on 20M", adif_log_contains_call_on_band("OZ1LAV", band_hz("20M")), 1);
    expect("found on 40M", adif_log_contains_call_on_band("OZ1LAV", band_hz("40M")), 1);
    expect("NOT found on 15M (never worked there)",
           adif_log_contains_call_on_band("OZ1LAV", band_hz("15M")), 0);
}

/* Unknown frequency (no CAT) falls back to a call-only match - the
 * conservative direction, and a documented decision in adif_log.c. Kept here
 * because the hash prefilter now sits in front of that branch too. */
static void test_unknown_band(void)
{
    printf("unknown band falls back to call-only\n");
    fresh(1500);
    char call[16];
    make_call(call, 1200);
    expect("freq 0 still matches", adif_log_contains_call_on_band(call, 0), 1);

    expect("a station never logged is not worked",
           adif_log_contains_call_on_band("ZZ9ZZZ", 14074000), 0);
}

/* A call that is in the log on NO band must not be found, whatever the hash
 * does. Guards the prefilter: a bug that compared only the hash would make
 * some unlogged call collide and read as worked. 20k probes is enough to hit
 * a 32-bit collision essentially never, so any hit here is a logic fault. */
static void test_no_false_positives(void)
{
    printf("no false positives (prefilter must not decide a match)\n");
    fresh(2000);
    int wrong = 0;
    for (int i = 2000; i < 22000; i++) {
        char call[16];
        make_call(call, i);
        if (adif_log_contains_call(call)) wrong++;
    }
    expect("20000 unlogged calls, none found", wrong, 0);
}

/* The ceiling must SAY SO. The silence is what cost Gyula the diagnosis, so
 * the loud branch is part of the fix and is tested as such. The cache cannot
 * reach ADIF_WORKED_MAX from a file this partition can hold, so the branch is
 * reached by starving the allocator instead.
 *
 * ⛔ RUNS IN ITS OWN PROCESS, and that is not tidiness. The cache CAPACITY is
 * static and adif_log_init() only ever grows it, so a second init in the same
 * process starts from whatever the previous test left - 2048 entries, big
 * enough that nothing needs to grow and the branch is never reached. The first
 * version of this test passed every other case and silently proved nothing
 * here. (On the device init runs once, so this is a harness property, not a
 * defect.) */
static void test_growth_failure_is_loud(void)
{
    printf("a cache that cannot grow says so\n");
    remove(s_log);
    write_log(2000);
    shim_set_spiffs_dir(s_dir);

    shim_log_errors = 0;
    shim_log_quiet  = 1;        /* the count is the assertion, not the noise */
    /* Fail EVERY allocation from the first one. adif_log_init() allocates for
       more than the cache, so a count would be tied to the order of calls
       inside it and would rot the moment that order changed. */
    shim_heap_fail_after = 0;
    adif_log_init();
    shim_heap_fail_after = -1;
    shim_log_quiet  = 0;

    expect("logged an error", shim_log_errors > 0, 1);
    expect("said it once, not once per QSO", shim_log_errors, 1);

    /* And it must still be USABLE, not crash. With no cache at all every
       lookup answers "not worked" - the documented safe direction - and the
       log itself is untouched. */
    expect("no crash; lookups answer not-worked", worked_i(5), 0);
    expect("logging still works (count intact)", adif_log_count(), 2000);
}

/* THE CEILING ITSELF, not the allocation failure.
 *
 * ⛔ Added after a mutation survived: dropping the ESP_LOGE from the
 * ADIF_WORKED_MAX branch changed nothing, because no earlier test ever got
 * near 8192 pairs. A silent ceiling is the SAME defect as the silent 1024
 * that started all this, so it has to be exercised, not reasoned about.
 *
 * 10000 QSOs is past ADIF_WORKED_MAX and past anything the 1 MB `storage`
 * partition can actually hold (~5800 records) - which is the point: the
 * ceiling must never be reachable from a legal file, AND must say so if it
 * ever is. Runs in its own process: it leaves the static capacity at the
 * ceiling, which would mask growth in any test after it. */
static void test_ceiling_is_loud(void)
{
    printf("the ADIF_WORKED_MAX ceiling says so (10000 QSOs)\n");
    remove(s_log);
    write_log(10000);
    shim_set_spiffs_dir(s_dir);

    shim_log_errors = 0;
    shim_log_quiet  = 1;
    adif_log_init();
    shim_log_quiet  = 0;

    expect("logged an error", shim_log_errors > 0, 1);
    expect("said it once, not once per QSO past it", shim_log_errors, 1);
    expect("count still right", adif_log_count(), 10000);
    /* What fitted is still correct - a full cache must not corrupt the part
       it does hold. */
    expect("an early station is still found", worked_i(7), 1);
}

/* Every lookup must give the mutex back. A path that returns while holding it
 * wedges the UI, and this file has three early returns per lookup. */
static void test_lock_balance(void)
{
    printf("lock balance\n");
    fresh(100);
    shim_lock_depth = 0;
    (void)adif_log_contains_call("AA0AAA");
    (void)adif_log_contains_call_on_band("AA0AAA", 14074000);
    (void)adif_log_contains_call_on_band("AA0AAA", 0);
    (void)adif_log_contains_call("");                 /* early return, no lock */
    (void)adif_log_contains_call_on_band(NULL, 14074000);
    expect("depth back to 0", shim_lock_depth, 0);
}

int main(int argc, char **argv)
{
    snprintf(s_dir, sizeof(s_dir), "%s",
             argc > 1 ? argv[1] : "build_host/spiffs");
    mkdir(s_dir
#ifndef _WIN32
          , 0777
#endif
          );
    snprintf(s_log, sizeof(s_log), "%s/qso.adi", s_dir);

    /* argv[2] == "starve" selects the one test that needs a virgin process.
       build_adif_worked_cache.sh runs the harness twice. */
    if (argc > 2 && strcmp(argv[2], "starve") == 0) {
        test_growth_failure_is_loud();
        printf("\n%s\n", fails ? "FAILURES" : "all passed");
        return fails ? 1 : 0;
    }
    if (argc > 2 && strcmp(argv[2], "ceiling") == 0) {
        test_ceiling_is_loud();
        printf("\n%s\n", fails ? "FAILURES" : "all passed");
        return fails ? 1 : 0;
    }

    test_large_log();
    test_band_awareness();
    test_same_call_two_bands();
    test_unknown_band();
    test_no_false_positives();
    test_lock_balance();

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}

/* MUTATION TESTS - run 2026-10-17, by test/mutate_adif_worked_cache.py. Each
 * is applied alone to main/adif/adif_log.c, the harness is rebuilt and run,
 * and the source is restored. All seven are KILLED.
 *
 *  1. Restore the fixed ceiling (cache_grow() refuses past 1024)
 *        -> "every logged station reads as worked" FAILS: 976 of 2000 missing.
 *           That is Gyula's defect, reproduced exactly.
 *  2. cache_add() dedupes on s_worked[i].hash alone, no strcmp
 *        -> "found on 40M" FAILS.
 *  3. The band strcmp dropped from adif_log_contains_call_on_band()
 *        -> "NOT worked on another" FAILS.
 *  4. The ADIF_WORKED_MAX branch of cache_grow() made silent
 *        -> "logged an error" FAILS (ceiling pass).
 *  4b. The out-of-PSRAM branch of cache_grow() made silent
 *        -> "logged an error" FAILS (starve pass).
 *  5. s_worked_full never latched, so the error repeats
 *        -> "said it once, not once per QSO" FAILS: 2001 errors.
 *  6. xSemaphoreGive() removed from the any-band lookup
 *        -> "depth back to 0" FAILS.
 *
 * ⚠ TWO OF THESE SURVIVED THE FIRST VERSION of this harness and are the
 * reason test_same_call_two_bands() and test_ceiling_is_loud() exist. #2
 * survived because every other test logged each call on exactly one band, so
 * nothing ever depended on the pair being the key. #4 survived because no
 * test came near 8192 pairs, so the ceiling branch was never executed. Both
 * gaps were invisible from reading the tests; only the mutation run showed
 * them.
 */
