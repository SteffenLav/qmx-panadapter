#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tier 0 resource diagnostics: periodic per-core IDLE% logging (v2).
//
// Every CPU_STATS_PERIOD_MS a background task reads the two idle tasks'
// run-time counters (ulTaskGetIdleRunTimeCounterForCore — a single TCB field
// read, O(1)) and logs the per-core idle percentage of the last window into
// the always-on diag ring (one line: "cpu: idle0 X% idle1 Y%").
//
// v1 logged a full per-task table via uxTaskGetSystemState() — REMOVED
// (2026-07-13): that call byte-walks every task's stack for the watermark
// inside a kernel-lock critical section (several stacks are 64 KB, in
// PSRAM), a multi-ms interrupts-off window every period. It delayed the
// core-0 MIPI-DSI frame-restart ISR past the blanking window and blanked the
// panel for one frame (the FT4 "full-screen cyan flash") — and core-pinning
// does NOT contain it (hardware-verified: lock contention propagates the
// ints-off window to the other core). Idle% is the headline number anyway;
// the per-task breakdown remains available on-demand via the dev-only resmon
// (POST /api/cmd {resmon}), where a rare one-frame blink is acceptable.
//
// Motivation: two core-0 CPU hogs (the priority-6 CW-audio ghost task, the
// FT8-mode panadapter render waste) each cost multiple releases to find
// because there was no CPU accounting on the device. Requires
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS. Counters are 32-bit µs and wrap
// every ~71 min; unsigned delta arithmetic makes that harmless.
esp_err_t cpu_stats_init(void);

#ifdef __cplusplus
}
#endif
