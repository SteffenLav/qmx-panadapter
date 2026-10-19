/* Host test for hosted_watchdog - WHEN the ESP32-C6 gets power-cycled because
 * the hosted WiFi link died (Bryan N0LUF, 2026-10-01).
 *
 * Build (from the repo root):
 *   gcc -I main/util -o hosted_watchdog_harness test/hosted_watchdog_harness.c \
 *       main/util/hosted_watchdog.c && ./hosted_watchdog_harness
 *
 * Why this exists: a healthy unit NEVER trips this. The probe succeeds every
 * 30 s on the bench, so a clean soak proves nothing whatever about the
 * thresholds, and the fault itself has not been reproduced here - it exists only
 * in Bryan's SD-card capture. The recovery cannot be tested without a wedge, but
 * the decision to fire can, and that is the dangerous half: CLAUDE.md records
 * the FT8 respawn watchdog firing ~390 times and degrading the device it was
 * meant to rescue. Too eager is worse than absent, because it power-cycles a
 * working radio.
 *
 * It links the REAL function, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>

#include "hosted_watchdog.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* n failing probes; returns how many of them asked for a re-link. */
static int fail_n(hosted_wd_t *w, int n, hosted_wd_action_t *last)
{
    int relinks = 0;
    for (int i = 0; i < n; i++) {
        hosted_wd_action_t a = hosted_wd_tick(w, true, false);
        if (a == HOSTED_WD_RELINK) relinks++;
        if (last) *last = a;
    }
    return relinks;
}

int main(void)
{
    printf("hosted_watchdog harness\n");

    /* 1. A healthy link does nothing at all, forever. This is every bench
     *    session and must stay silent. */
    {
        hosted_wd_t w = {0};
        for (int i = 0; i < 1000; i++)
            CHECK(hosted_wd_tick(&w, true, true) == HOSTED_WD_NOTHING,
                  "a healthy probe did something at tick %d", i);
        CHECK(w.relink_count == 0, "a healthy session used a re-link attempt");
    }

    /* 2. ⛔ A MOMENTARY HICCUP MUST NOT POWER-CYCLE THE RADIO. Five consecutive
     *    failures - two and a half minutes - still do nothing. */
    {
        hosted_wd_t w = {0};
        CHECK(fail_n(&w, HOSTED_WD_FAILS_BEFORE_RELINK - 1, 0) == 0,
              "re-linked before %d failures", HOSTED_WD_FAILS_BEFORE_RELINK);
        CHECK(w.relink_count == 0, "an attempt was spent early");
    }

    /* 3. The sixth consecutive failure acts - once. */
    {
        hosted_wd_t w = {0};
        fail_n(&w, HOSTED_WD_FAILS_BEFORE_RELINK - 1, 0);
        CHECK(hosted_wd_tick(&w, true, false) == HOSTED_WD_RELINK,
              "did not re-link on failure %d", HOSTED_WD_FAILS_BEFORE_RELINK);
        CHECK(hosted_wd_tick(&w, true, false) == HOSTED_WD_NOTHING,
              "re-linked again on the very next failure");
    }

    /* 4. CONSECUTIVE means consecutive. Five failures, one success, five more:
     *    ten failures in eleven probes and not a single power-cycle. */
    {
        hosted_wd_t w = {0};
        fail_n(&w, 5, 0);
        CHECK(hosted_wd_tick(&w, true, true) == HOSTED_WD_RECOVERED,
              "the recovery was not announced");
        CHECK(w.last_missed == 5, "wrong missed count reported: %d", w.last_missed);
        CHECK(fail_n(&w, 5, 0) == 0, "a broken streak still re-linked");
        CHECK(w.relink_count == 0, "a broken streak spent an attempt");
    }

    /* 5. A recovery with no missed probes says nothing - the log line is for a
     *    link that came back, not for every healthy pass. */
    {
        hosted_wd_t w = {0};
        CHECK(hosted_wd_tick(&w, true, true) == HOSTED_WD_NOTHING,
              "announced a recovery that never happened");
    }

    /* 6. ⛔ BOUNDED PER SESSION. Three attempts, then it says a reboot is needed
     *    and never power-cycles again however long the fault lasts. This is the
     *    FT8 respawn watchdog's ~390 firings, prevented. */
    {
        hosted_wd_t w = {0};
        hosted_wd_action_t last = HOSTED_WD_NOTHING;
        int relinks = fail_n(&w, HOSTED_WD_FAILS_BEFORE_RELINK * 100, &last);
        CHECK(relinks == HOSTED_WD_MAX_RELINKS,
              "%d re-links in a long outage, expected %d", relinks, HOSTED_WD_MAX_RELINKS);
        CHECK(last == HOSTED_WD_EXHAUSTED, "did not end in the exhausted state");
        CHECK(w.relink_count == HOSTED_WD_MAX_RELINKS,
              "attempt counter ran past its limit: %d", w.relink_count);
    }

    /* 7. Exhausted does not re-announce itself every 30 s: it waits out another
     *    full streak first, so the log is not flooded for the rest of a session
     *    that may run for days. */
    {
        hosted_wd_t w = {0};
        fail_n(&w, HOSTED_WD_FAILS_BEFORE_RELINK * HOSTED_WD_MAX_RELINKS, 0);
        CHECK(w.relink_count == HOSTED_WD_MAX_RELINKS, "setup wrong");
        for (int i = 0; i < HOSTED_WD_FAILS_BEFORE_RELINK - 1; i++)
            CHECK(hosted_wd_tick(&w, true, false) == HOSTED_WD_NOTHING,
                  "exhausted watchdog spoke again at failure %d", i + 1);
        CHECK(hosted_wd_tick(&w, true, false) == HOSTED_WD_EXHAUSTED,
              "exhausted state was not reported after a full streak");
    }

    /* 8. ⛔ A DELIBERATE DISCONNECTION IS NOT A FAULT. The operator switching
     *    WiFi off, or an association that has not happened yet, must never
     *    accumulate towards power-cycling the C6. */
    {
        hosted_wd_t w = {0};
        fail_n(&w, HOSTED_WD_FAILS_BEFORE_RELINK - 1, 0);
        CHECK(hosted_wd_tick(&w, false, false) == HOSTED_WD_NOTHING,
              "acted while not watching");
        CHECK(w.fail_streak == 0, "the streak survived a disconnection");
        CHECK(fail_n(&w, HOSTED_WD_FAILS_BEFORE_RELINK - 1, 0) == 0,
              "a disconnection topped up a streak into a power-cycle");
    }
    {   /* ... and a long spell switched off never acts on its own */
        hosted_wd_t w = {0};
        for (int i = 0; i < 500; i++)
            CHECK(hosted_wd_tick(&w, false, false) == HOSTED_WD_NOTHING,
                  "acted while WiFi was off at tick %d", i);
        CHECK(w.relink_count == 0, "spent an attempt with WiFi off");
    }

    /* 9. A null watchdog must refuse rather than fault. */
    CHECK(hosted_wd_tick(0, true, false) == HOSTED_WD_NOTHING, "null state did not refuse");

    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("all checks passed\n");
    return 0;
}
