// hosted_watchdog - WHEN to power-cycle the ESP32-C6 because the hosted WiFi
// link has died.
//
// Bryan N0LUF, 2026-10-01, captured on the SD card WHILE his WiFi was dead (the
// web download cannot work once it is - Michael KZ4LY made that point and he was
// right):
//
//   W H_SDIO_DRV: SDIO RX oversize: len=19838 ... - draining to recover  x183/9 s
//   W rpc_core:   Timeout waiting for Resp for Req[0x126]                x61, forever
//
// The link does not merely desynchronise, it DIES, and the existing oversize
// drain cannot fix a dead link however long it runs. 0x126 is WifiStaGetApInfo -
// exactly what esp_wifi_sta_get_ap_info() issues - so the call that was timing
// out is also the cheapest probe for the condition. Recovery is a level up:
// power-cycle the C6 and bring the hosted transport back, which is what boot
// already does.
//
// ⛔ WHAT THIS FILE IS FOR IS THE BOUNDS, NOT THE RECOVERY. CLAUDE.md records the
// FT8 respawn watchdog firing ~390 times and degrading the device it was
// rescuing. A watchdog that is too eager is worse than no watchdog: it
// power-cycles the radio on a momentary RPC hiccup. The bounds are therefore
// SIX consecutive failures (~3 min on the 30 s loop) before acting, and at most
// THREE attempts in a session, after which it says so and stops for good.
//
// ⚠ The recovery itself has NEVER run against a real wedge - the condition
// exists only in Bryan's log and has not been reproduced on the bench. That
// cannot be fixed here. What CAN be fixed here is the half that decides whether
// to fire at all, which is pure counting and needs no radio: a bench unit never
// trips the probe, so a healthy session proves nothing about the thresholds.
//
// Portable: no ESP-IDF dependencies, so test/hosted_watchdog_harness.c links
// these very functions rather than a copy of them.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOSTED_WD_FAILS_BEFORE_RELINK 6   // x 30 s loop = ~3 min before acting
#define HOSTED_WD_MAX_RELINKS         3   // per session, then stop for good

typedef enum {
    HOSTED_WD_NOTHING = 0,  // carry on
    HOSTED_WD_RECOVERED,    // answered again after missed probes - say so
    HOSTED_WD_RELINK,       // power-cycle the C6 now
    HOSTED_WD_EXHAUSTED,    // out of attempts; only a reboot will do it
} hosted_wd_action_t;

typedef struct {
    int fail_streak;    // consecutive probe failures
    int relink_count;   // attempts used this session
    int last_missed;    // probes missed before a HOSTED_WD_RECOVERED
} hosted_wd_t;

// One pass of the 30 s loop.
//
// `watching` is false whenever the probe says nothing about the C6 - no
// association yet, or the operator switched WiFi off. The streak resets then, so
// a deliberate disconnection can never accumulate towards a power-cycle.
//
// `probe_ok` is the result of esp_wifi_sta_get_ap_info().
hosted_wd_action_t hosted_wd_tick(hosted_wd_t *w, bool watching, bool probe_ok);

#ifdef __cplusplus
}
#endif
