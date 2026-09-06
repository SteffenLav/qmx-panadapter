// Host harness for main/ft8_recent.c - links the REAL functions, not a copy.
//
//   gcc -std=c11 -Wall -Wextra -I../main -o ft8_recent.exe \
//       ft8_recent_harness.c ../main/ft8_recent.c
//
// Exists because the on-device route to this logic is unusable: exercising it
// needs a completed QSO, and in FT8 simulation mode on 2026-08-28 the decode
// took 15.4 s against a 15 s slot, so slots overran 2x and the robot never saw
// a decode whose slot matched the current one. A flaky simulator is a worse
// instrument than a deterministic test that can step the clock at will.

#include "ft8_recent.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void check(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("  FAIL %-52s got %s want %s\n", what,
               got ? "true" : "false", want ? "true" : "false");
        fails++;
    } else {
        printf("  ok   %-52s %s\n", what, got ? "true" : "false");
    }
}

#define T0 1800000000LL          /* a plausible epoch, well past the guard */

int main(void)
{
    printf("ft8_recent harness\n");

    /* 1. nothing recorded */
    ft8_recent_reset();
    check("unknown call is not recent", ft8_recent_worked("W1AW", "20m", T0), false);

    /* 2/3/4. the window boundary - the whole point of the fix */
    ft8_recent_note("W1AW", "20m", T0);
    check("just worked", ft8_recent_worked("W1AW", "20m", T0), true);
    check("1 s later", ft8_recent_worked("W1AW", "20m", T0 + 1), true);
    check("1 s before the window closes",
          ft8_recent_worked("W1AW", "20m", T0 + FT8_RECENT_GRACE_SEC - 1), true);
    check("exactly at the window",
          ft8_recent_worked("W1AW", "20m", T0 + FT8_RECENT_GRACE_SEC), false);
    check("after the window",
          ft8_recent_worked("W1AW", "20m", T0 + FT8_RECENT_GRACE_SEC + 1), false);

    /* 5. band-aware: a new band is a new contact, which is what the ADIF
     *    checkbox path already assumes. */
    check("same call, different band", ft8_recent_worked("W1AW", "40m", T0), false);

    /* 6. call-aware */
    check("different call, same band", ft8_recent_worked("K0FOX", "20m", T0), false);

    /* 7. ring eviction: 17 distinct stations, the first must fall out */
    ft8_recent_reset();
    char c[16];
    for (int i = 0; i < FT8_RECENT_MAX + 1; i++) {
        snprintf(c, sizeof(c), "CALL%d", i);
        ft8_recent_note(c, "20m", T0);
    }
    check("oldest of 17 evicted", ft8_recent_worked("CALL0", "20m", T0), false);
    check("newest of 17 held",    ft8_recent_worked("CALL16", "20m", T0), true);

    /* 8. re-noting must REFRESH, not consume a second slot - otherwise one
     *    station worked repeatedly evicts everybody else. */
    ft8_recent_reset();
    ft8_recent_note("W1AW", "20m", T0);
    for (int i = 0; i < FT8_RECENT_MAX * 2; i++) ft8_recent_note("W1AW", "20m", T0 + i);
    ft8_recent_note("K0FOX", "20m", T0);
    check("repeat-noted call did not flush the ring",
          ft8_recent_worked("K0FOX", "20m", T0), true);
    // ⚠ Query at the REFRESHED time, not at T0. The first version of this test
    // asked at T0 after refreshing the entry to T0+31 - which is a backwards
    // clock step, so `false` was the correct answer and the harness was wrong,
    // not the code. Exactly the class of mistake a harness exists to catch.
    check("repeat-noted call still held",
          ft8_recent_worked("W1AW", "20m", T0 + FT8_RECENT_MAX * 2), true);
    // ⚠ THE discriminating case for the refresh loop, and my first version of
    // this test did not have it. Checking only that repeat-noted calls are still
    // PRESENT cannot fail: the ring holds 16 entries either way. What refresh
    // actually protects is an EARLIER, DIFFERENT station - without it, 33 notes
    // of one call wrap the ring twice and evict everybody. A mutation that
    // disabled the refresh loop passed the whole suite until this was added.
    ft8_recent_reset();
    ft8_recent_note("EARLY", "20m", T0);
    for (int i = 0; i < FT8_RECENT_MAX * 2 + 1; i++) ft8_recent_note("W1AW", "20m", T0 + i);
    check("repeats did not evict an earlier station",
          ft8_recent_worked("EARLY", "20m", T0 + FT8_RECENT_MAX * 2), true);

    ft8_recent_reset();
    ft8_recent_note("W1AW", "20m", T0);
    for (int i = 0; i < FT8_RECENT_MAX * 2; i++) ft8_recent_note("W1AW", "20m", T0 + i);
    check("refresh moved the timestamp forward",
          ft8_recent_worked("W1AW", "20m",
                            T0 + FT8_RECENT_MAX * 2 + FT8_RECENT_GRACE_SEC - 2), true);

    /* 9. an unset clock must never become a permanent refusal */
    ft8_recent_reset();
    ft8_recent_note("W1AW", "20m", 50);
    check("clock not set: no grace", ft8_recent_worked("W1AW", "20m", 60), false);

    /* 10. clock stepped BACKWARDS (SNTP or an FT8-derived correction can do
     *     this) must read as "not recent", not as a huge age. */
    ft8_recent_reset();
    ft8_recent_note("W1AW", "20m", T0 + 1000);
    check("clock stepped back", ft8_recent_worked("W1AW", "20m", T0), false);

    /* 11. defensive: NULLs and empties must not match or crash */
    ft8_recent_reset();
    ft8_recent_note(NULL, "20m", T0);
    ft8_recent_note("", "20m", T0);
    ft8_recent_note("W1AW", NULL, T0);
    check("NULL call is not recent", ft8_recent_worked(NULL, "20m", T0), false);
    check("empty call is not recent", ft8_recent_worked("", "20m", T0), false);
    check("NULL band is not recent", ft8_recent_worked("W1AW", NULL, T0), false);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
