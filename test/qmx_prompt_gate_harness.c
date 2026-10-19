/* Host test for qmx_prompt_gate - WHEN the "Now turn on or reboot your QMX/+"
 * prompt may appear.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o qmx_prompt_gate_harness test/qmx_prompt_gate_harness.c \
 *       main/util/qmx_prompt_gate.c && ./qmx_prompt_gate_harness
 *
 * Why this exists: the gate cannot be exercised on the bench. The bench WiFi
 * always associates (5 boots, 2026-10-02, every one settled 5.1-6.2 s after
 * "Init complete - main task idle"), so the branch that matters - WiFi enabled
 * and never associating - never runs there. The first version of the gate
 * suppressed the prompt FOREVER in that case and the bench could not have shown
 * it. Checking it needs a harness or a field report, and a field report costs an
 * operator a blank screen.
 *
 * It links the REAL function, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>
#include <stdint.h>

#include "qmx_prompt_gate.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

#define TICK(g, boot, ms, en, conn) qmx_prompt_gate_tick(&(g), (boot), (ms), (en), (conn))

int main(void)
{
    printf("qmx_prompt_gate harness\n");

    /* 1. Nothing before start-up finishes - no timeout, no exceptions. This is
     *    the half that protects internal RAM. */
    {
        qmx_prompt_gate_t g = {0};
        CHECK(!TICK(g, false, 1000, true, true),   "shown before boot complete (wifi up)");
        CHECK(!TICK(g, false, 60000, false, false), "shown before boot complete (wifi off)");
        CHECK(!TICK(g, false, 600000, true, false), "a long wait must not substitute for boot");
    }

    /* 2. The bench case: WiFi enabled and associated. Measured on bench dev,
     *    boot complete ~9.5 s, sta ip ~15.5 s. */
    {
        qmx_prompt_gate_t g = {0};
        CHECK(!TICK(g, true,  9500, true, false), "shown while WiFi still associating");
        CHECK(!TICK(g, true, 14000, true, false), "shown while WiFi still associating");
        CHECK( TICK(g, true, 15500, true, true),  "not shown once WiFi is up");
    }

    /* 3. ⛔ THE REGRESSION. WiFi enabled, never associates - no credentials,
     *    wrong password, out of range, portable. The prompt must still arrive. */
    {
        qmx_prompt_gate_t g = {0};
        CHECK(!TICK(g, true,  9500, true, false), "shown immediately at boot complete");
        CHECK(!TICK(g, true, 20000, true, false), "shown before the grace period elapsed");
        CHECK(!TICK(g, true, 9500 + QMX_PROMPT_WIFI_GRACE_MS - 1, true, false),
              "shown one tick early");
        CHECK( TICK(g, true, 9500 + QMX_PROMPT_WIFI_GRACE_MS, true, false),
               "NEVER shown when WiFi never associates - this is the bug");
    }

    /* 4. WiFi switched off by the operator: settled at once, no grace period. */
    {
        qmx_prompt_gate_t g = {0};
        CHECK(TICK(g, true, 9500, false, false), "WiFi off is not treated as settled");
    }

    /* 5. The grace period is measured from BOOT COMPLETE, not from power-on.
     *    A unit that takes 40 s to finish start-up must still wait after it. */
    {
        qmx_prompt_gate_t g = {0};
        CHECK(!TICK(g, false, 40000, true, false), "shown before boot on a slow start");
        CHECK(!TICK(g, true,  40000, true, false), "boot complete alone must not show it");
        CHECK(!TICK(g, true,  69000, true, false), "grace measured from power-on, not boot");
        CHECK( TICK(g, true,  70000, true, false), "not shown after grace from boot complete");
    }

    /* 6. ⛔ IT LATCHES. The hosted C6 link dies mid-session (Bryan N0LUF) and
     *    wifi_is_connected() goes false again. A prompt the operator is reading
     *    must not vanish: the boot-time hazard is long past. */
    {
        qmx_prompt_gate_t g = {0};
        CHECK( TICK(g, true, 15500, true, true),  "not shown once WiFi is up");
        CHECK( TICK(g, true, 90000, true, false), "re-hidden when the WiFi link dropped");
        CHECK( TICK(g, true, 95000, false, false), "re-hidden when WiFi was switched off");
    }
    {   /* ... and the same once the grace period, not the association, opened it */
        qmx_prompt_gate_t g = {0};
        CHECK(!TICK(g, true, 1000, true, false), "shown at boot complete");
        CHECK( TICK(g, true, 1000 + QMX_PROMPT_WIFI_GRACE_MS, true, false), "grace did not open it");
        CHECK( TICK(g, true, 1000 + QMX_PROMPT_WIFI_GRACE_MS + 1, true, false), "did not latch");
    }

    /* 7. Wrap-safe across the 49.7-day uint32 millisecond roll-over. A
     *    (now > boot + grace) comparison gets this wrong and would hold the
     *    prompt off for another 49 days. */
    {
        qmx_prompt_gate_t g = {0};
        const uint32_t near_end = 0xFFFFF000u;          /* ~4 s before wrap */
        CHECK(!TICK(g, true, near_end, true, false), "shown at boot complete");
        CHECK(!TICK(g, true, (uint32_t)(near_end + QMX_PROMPT_WIFI_GRACE_MS - 1), true, false),
              "shown early across the wrap");
        CHECK( TICK(g, true, (uint32_t)(near_end + QMX_PROMPT_WIFI_GRACE_MS), true, false),
               "the millisecond counter wrapping held the prompt off");
    }

    /* 8. A null gate must refuse rather than fault. */
    CHECK(!qmx_prompt_gate_tick(0, true, 999999, false, false), "null gate did not refuse");

    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("all checks passed\n");
    return 0;
}
