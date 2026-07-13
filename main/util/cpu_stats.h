#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tier 0 resource diagnostics: periodic per-task, per-core CPU usage logging.
//
// Every CPU_STATS_PERIOD_MS a background task snapshots the FreeRTOS run-time
// counters (uxTaskGetSystemState) and logs the DELTA since the previous
// snapshot as a percentage of wall time — i.e. what each task actually
// consumed during the last window, not since boot. Two compact lines per
// window (core0 / core1, busiest first), so they land in the always-on diag
// ring and are downloadable via /api/log or a serial capture. Each core's
// line sums to ~100% including its IDLE task.
//
// Motivation: two core-0 CPU hogs (the priority-6 CW-audio ghost task, the
// FT8-mode panadapter render waste) each cost multiple releases to find
// because there was no per-task CPU accounting on the device. This makes the
// next one a one-glance diagnosis in any field log.
//
// Requires CONFIG_FREERTOS_USE_TRACE_FACILITY, _GENERATE_RUN_TIME_STATS and
// _VTASKLIST_INCLUDE_COREID (all in sdkconfig.defaults). Run-time counters
// are 32-bit µs and wrap every ~71 min; unsigned delta arithmetic makes that
// harmless as long as the sample period stays well under the wrap interval.
esp_err_t cpu_stats_init(void);

#ifdef __cplusplus
}
#endif
