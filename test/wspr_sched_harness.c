/* Host harness for the UTC-anchored WSPR transmit schedule.
 *
 * This decides when a transmitter keys, so it is checked exhaustively rather
 * than by watching a beacon for an afternoon. The properties below are the ones
 * that actually matter on the air:
 *
 *   - the radio never keys continuously (the failure this project has already
 *     produced on the bench with a 1-in-2 / 2-burst setting);
 *   - exactly tx_cycles bursts happen in every period, forever, with no drift;
 *   - the schedule depends ONLY on the cycle index, so it is identical before
 *     and after a reboot - which is the whole point of the change;
 *   - John W5JSS's own settings land on the minutes he expected.
 *
 * Build: see test/CMakeLists.txt, or directly
 *   gcc -I main test/wspr_sched_harness.c main/wspr_sched.c -o wspr_sched_harness
 */

#include "wspr_sched.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); g_fail++; }
}

/* No setting may ever key the radio on every cycle. */
static void test_never_continuous(void)
{
    printf("never transmits continuously, all settings:\n");
    for (unsigned tx = 1; tx <= 4; tx++) {
        for (unsigned rx = 0; rx <= 20; rx++) {
            int consecutive = 0, worst = 0;
            for (int64_t c = 0; c < 2000; c++) {
                if (wspr_sched_is_tx_cycle(c, (uint8_t)tx, (uint8_t)rx)) {
                    if (++consecutive > worst) worst = consecutive;
                } else {
                    consecutive = 0;
                }
            }
            char msg[96];
            snprintf(msg, sizeof msg, "tx=%u rx=%u ran %d bursts back to back (max %u)",
                     tx, rx, worst, tx);
            /* rx==0 is clamped to 1, so the longest legitimate run is tx. */
            check(worst <= (int)tx, msg);
        }
    }
    printf("  done\n");
}

/* Exactly tx bursts per period, in every period, with no accumulation. */
static void test_rate_holds_forever(void)
{
    printf("exactly tx bursts per period, 5000 periods deep:\n");
    for (unsigned tx = 1; tx <= 4; tx++) {
        for (unsigned rx = 1; rx <= 20; rx++) {
            const int period = (int)tx + (int)rx;
            int bad = -1;
            for (int p = 0; p < 5000 && bad < 0; p++) {
                int n = 0;
                for (int k = 0; k < period; k++)
                    if (wspr_sched_is_tx_cycle((int64_t)p * period + k, (uint8_t)tx, (uint8_t)rx)) n++;
                if (n != (int)tx) bad = p;
            }
            char msg[96];
            snprintf(msg, sizeof msg, "tx=%u rx=%u wrong burst count at period %d", tx, rx, bad);
            check(bad < 0, msg);
        }
    }
    printf("  done\n");
}

/* The schedule must not care where it is started from - that is what "anchored"
 * means, and the drift being fixed is exactly a failure of this property. */
static void test_reboot_invariant(void)
{
    printf("identical regardless of start point (reboot invariance):\n");
    const int64_t offsets[] = { 0, 1, 7, 10, 999, 100000, 7777777 };
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        for (int64_t c = 0; c < 500; c++) {
            const int64_t idx = offsets[i] + c;
            const bool a = wspr_sched_is_tx_cycle(idx, 2, 8);
            /* Recomputed from scratch, as a fresh boot would. */
            const bool b = ((idx % 10) + 10) % 10 < 2;
            if (a != b) { check(0, "schedule depends on history, not the cycle index"); return; }
        }
    }
    printf("  done\n");
}

/* John W5JSS: 2 transmit + 8 receive, expecting minutes 0 and 2 of every 20. */
static void test_w5jss_expectation(void)
{
    printf("W5JSS 2tx+8rx lands on minutes 0 and 2 of every 20:\n");
    for (int hour = 0; hour < 24; hour++) {
        for (int min = 0; min < 60; min += 2) {
            const int64_t utc   = (int64_t)hour * 3600 + min * 60;
            const int64_t cycle = utc / 120;
            const bool    tx    = wspr_sched_is_tx_cycle(cycle, 2, 8);
            const bool    want  = (min % 20 == 0) || (min % 20 == 2);
            if (tx != want) {
                char msg[96];
                snprintf(msg, sizeof msg, "%02d:%02d expected tx=%d got %d", hour, min, want, tx);
                check(0, msg);
                return;
            }
        }
    }
    printf("  done\n");
}

/* An unset clock must not transmit on the wrong cycle. */
static void test_negative_cycles(void)
{
    printf("negative cycle index stays inside the period:\n");
    for (int64_t c = -5000; c < 0; c++) {
        const uint8_t bi = wspr_sched_burst_index(c, 2, 8);
        check(bi <= 2, "burst index out of range for a negative cycle");
        if (g_fail) return;
    }
    printf("  done\n");
}

/* next_tx_cycle must agree with is_tx_cycle, and never point backwards. */
static void test_next_agrees(void)
{
    printf("next_tx_cycle agrees with is_tx_cycle:\n");
    for (unsigned tx = 1; tx <= 4; tx++) {
        for (unsigned rx = 1; rx <= 20; rx++) {
            for (int64_t c = 0; c < 300; c++) {
                const int64_t n = wspr_sched_next_tx_cycle(c, (uint8_t)tx, (uint8_t)rx);
                if (n < c)                                            { check(0, "next_tx_cycle pointed backwards"); return; }
                if (!wspr_sched_is_tx_cycle(n, (uint8_t)tx, (uint8_t)rx)) { check(0, "next_tx_cycle is not a TX cycle"); return; }
                /* Nothing between c and n may be a TX cycle. */
                for (int64_t k = c; k < n; k++)
                    if (wspr_sched_is_tx_cycle(k, (uint8_t)tx, (uint8_t)rx)) { check(0, "next_tx_cycle skipped one"); return; }
            }
        }
    }
    printf("  done\n");
}

static void test_tx_zero_is_silent(void)
{
    printf("tx_cycles == 0 never transmits:\n");
    for (int64_t c = -100; c < 1000; c++)
        check(!wspr_sched_is_tx_cycle(c, 0, 8), "transmitted with tx_cycles == 0");
    check(wspr_sched_next_tx_cycle(0, 0, 8) == -1, "next_tx_cycle should be -1 with tx_cycles == 0");
    printf("  done\n");
}

int main(void)
{
    printf("=== WSPR schedule harness ===\n");
    test_never_continuous();
    test_rate_holds_forever();
    test_reboot_invariant();
    test_w5jss_expectation();
    test_negative_cycles();
    test_next_agrees();
    test_tx_zero_is_silent();
    printf("=== %s ===\n", g_fail ? "FAILURES" : "all passed");
    return g_fail ? 1 : 0;
}
