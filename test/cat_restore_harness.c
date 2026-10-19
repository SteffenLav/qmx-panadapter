/* Host test for cat_restore - what the band scan sends to put the radio back
 * (John W5JSS, 2026-10-01, every CAT link-up leaving him on 160 m).
 *
 * Build (from the repo root):
 *   gcc -I main/util -o cat_restore_harness test/cat_restore_harness.c \
 *       main/util/cat_restore.c && ./cat_restore_harness
 *
 * Why this exists: the restore branch has never run. On the bench QMX the band
 * menus do NOT move the dial - frequency and mode are identical before and after
 * the scan - so the save is proven and the decision that follows it is dead code
 * on this hardware. The bench cannot reach it however many link-ups are watched.
 *
 * It links the REAL function, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>

#include "cat_restore.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

int main(void)
{
    printf("cat_restore harness\n");

    /* 1. ⛔ THE BENCH CASE, and the one that must stay silent: the menus left
     *    the radio alone. A needless FA write at link-up moves a dial the
     *    operator had just set. 14095600 Hz mode '6' is what bench dev reports,
     *    before and after, on every link-up. */
    {
        cat_restore_plan_t p = cat_restore_plan(14095600, '6', 14095600, '6');
        CHECK(!p.send_freq, "wrote a frequency the scan had not moved");
        CHECK(!p.send_mode, "wrote a mode the scan had not changed");
    }

    /* 2. John W5JSS's fault: 20 m DiGi in, 160 m CW out. Both must come back. */
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, '6', 1837700, '3');
        CHECK(p.send_freq && p.freq_hz == 14097000,
              "did not restore 14097000 Hz (got %u, send=%d)",
              (unsigned)p.freq_hz, p.send_freq);
        CHECK(p.send_mode && p.mode_digit == '6',
              "did not restore mode '6' (got '%c', send=%d)",
              p.mode_digit ? p.mode_digit : '?', p.send_mode);
    }

    /* 3. The halves are independent - the menus can move one and not the other. */
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, '6', 1837700, '6');
        CHECK(p.send_freq, "frequency moved and was not restored");
        CHECK(!p.send_mode, "rewrote a mode that had not changed");
    }
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, '6', 14097000, '3');
        CHECK(!p.send_freq, "rewrote a frequency that had not changed");
        CHECK(p.send_mode && p.mode_digit == '6', "mode changed and was not restored");
    }

    /* 4. ⛔ NO ANSWER MEANS NO WRITE. The pre-scan FA;MD; query waits 25 x 20 ms;
     *    if it times out, pre_freq is 0 and the radio's position is unknown.
     *    Restoring 0 Hz - or any invented value - is worse than leaving it. */
    {
        cat_restore_plan_t p = cat_restore_plan(0, '6', 1837700, '3');
        CHECK(!p.send_freq, "restored a frequency that was never read");
        CHECK(p.send_mode && p.mode_digit == '6',
              "a missed frequency must not suppress a known mode");
    }

    /* 5. The mode half has its own "not known": the MD reply gets only 60 ms
     *    after the FA one lands, so it can be absent on its own. 0 is the
     *    uninitialised value and must never be sent as MD. */
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, 0, 1837700, '3');
        CHECK(p.send_freq, "a missed mode must not suppress a known frequency");
        CHECK(!p.send_mode, "sent MD for a mode that was never read");
    }
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, 'X', 1837700, '3');
        CHECK(!p.send_mode, "sent a mode digit outside '1'..'9'");
    }
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, '0', 1837700, '3');
        CHECK(!p.send_mode, "'0' is not a Kenwood mode digit and must not be sent");
    }

    /* 6. Nothing known at all: total silence, not a flurry of zeros. */
    {
        cat_restore_plan_t p = cat_restore_plan(0, 0, 0, 0);
        CHECK(!p.send_freq && !p.send_mode, "wrote something knowing nothing");
    }

    /* 7. The "after" side being unreadable is still a move: the scan reported
     *    nothing back, and 0 Hz is not where the operator left it. */
    {
        cat_restore_plan_t p = cat_restore_plan(14097000, '6', 0, 0);
        CHECK(p.send_freq && p.freq_hz == 14097000, "did not restore after a silent scan");
        CHECK(p.send_mode && p.mode_digit == '6', "did not restore the mode after a silent scan");
    }

    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("all checks passed\n");
    return 0;
}
