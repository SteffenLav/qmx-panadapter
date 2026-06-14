// Diagnostic logger: opt-in in-RAM ring buffer that captures all ESP_LOG
// output (and gated per-line CAT TX/RX traffic) so a remote user can flip
// one switch, reproduce a problem, and hand back a communication log.
//
// Two extraction paths:
//   - USB serial: everything still streams to the console as normal, so the
//     existing tools/capture_serial_log.ps1 keeps working (works with no WiFi).
//   - WiFi: the web UI exposes GET /api/log which downloads the ring buffer
//     as qmx-log.txt.
//
// The capture hook is installed once at boot (diag_log_init); the ring only
// fills while enabled. The enable flag is persisted in NVS so a boot-time
// problem can be captured across the reset that follows enabling it.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install the esp_log vprintf hook and allocate the ring buffer (PSRAM
// preferred). Call once, early in app_main, before other subsystems init so
// their boot logs are captured. Safe to call again (no-op once allocated).
void diag_log_init(void);

// Turn capture on/off. When turning on, writes a header line with the
// firmware version / build stamp so every captured log is self-identifying.
void diag_log_set_enabled(bool on);

// Whether capture is currently active. Cheap; safe from any task. Used to
// gate the verbose CAT TX/RX logging so it costs nothing when disabled.
bool diag_log_enabled(void);

// Number of bytes currently held in the ring (<= capacity).
size_t diag_log_size(void);

// Copy the oldest..newest captured bytes into dst (up to cap). Returns the
// number of bytes written. Thread-safe against the capture hook.
size_t diag_log_snapshot(char *dst, size_t cap);

#ifdef __cplusplus
}
#endif
