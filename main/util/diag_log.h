// Diagnostic logger: an always-on in-RAM ring buffer that captures all
// ESP_LOG output (and per-line CAT TX/RX traffic) so that whenever something
// goes wrong, the log is already there to hand back — no need to have flipped
// a switch beforehand.
//
// Three extraction paths:
//   - USB serial: everything still streams to the console as normal, so the
//     existing tools/capture_serial_log.ps1 keeps working (works with no WiFi).
//   - WiFi: the web UI exposes GET /api/log which downloads the ring buffer
//     as qmx-log.txt.
//   - microSD: storage/sd_archive mirrors the ring to /sdcard as a rotating
//     file (the persistence path — the PSRAM ring is wiped on reboot, the SD
//     copy survives the reboot that follows a crash). It reads the ring
//     incrementally via diag_log_total() / diag_log_read_from().
//
// The capture hook is installed once at boot (diag_log_init) and capture is
// on from that point — no enable flag, no opt-in.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install the esp_log vprintf hook and allocate the ring buffer (PSRAM
// preferred). Capture is on from the moment this returns. Call once, first
// thing in app_main, before other subsystems init so their boot logs are
// captured. Safe to call again (no-op once allocated).
void diag_log_init(void);

// Write the self-identifying session header (firmware version, MAC, chip,
// reset reason, operator, WiFi/QMX state) into the log. Call once early in
// app_main *after* settings_init() so the operator/version fields are valid
// (diag_log_init runs too early for that). Marks the start of each boot's
// log clearly for crash analysis.
void diag_log_write_session_header(void);

// Whether capture is active. Always true after diag_log_init(); retained so
// callers that gate verbose CAT TX/RX logging on it keep compiling.
bool diag_log_enabled(void);

// Number of bytes currently held in the ring (<= capacity).
size_t diag_log_size(void);

// Copy the oldest..newest captured bytes into dst (up to cap). Returns the
// number of bytes written. Thread-safe against the capture hook.
size_t diag_log_snapshot(char *dst, size_t cap);

// Discard everything currently in the ring (resets it to empty). Capture
// stays enabled. Called after a successful web download so each download
// hands back only what's new since the last one. Does NOT reset the
// monotonic total used by diag_log_read_from() — the SD mirror's cursor
// stays valid across a web clear. Thread-safe.
void diag_log_clear(void);

// Total number of bytes ever appended to the ring this boot (monotonic,
// never wraps). The cursor space for diag_log_read_from(). Thread-safe.
uint64_t diag_log_total(void);

// Path of the rotated (older) generation of the flash-persisted log,
// /spiffs/diag.0.log - may not exist until the first rotation.
const char *diag_log_persist_path_rotated(void);

// Count of USB enumeration failures seen in the log stream since boot
// (ENUM CHECK_SHORT/FULL_DEV_DESC FAILED lines - the stale-QMX wedge
// signature, TODO #74). Read by usb_replug.c's stale-QMX detector.
uint32_t diag_log_usb_enum_failures(void);

// Incremental tail read for the SD mirror. Copies bytes in [from, total)
// still retained in the ring into dst (up to cap), returns the count, and
// sets *out_next to the new cursor. If `from` has fallen off the back of the
// ring (mirror fell behind by more than the ring size — not expected), the
// lost bytes are skipped and *out_next jumps to the oldest retained position.
// Thread-safe.
size_t diag_log_read_from(uint64_t from, char *dst, size_t cap, uint64_t *out_next);

// Start the background task that persists the log to a small rolling file on
// internal SPIFFS flash (256 KB), so it survives power-off even with no SD
// card inserted (the PSRAM ring alone is wiped on reboot). Call once from
// app_main AFTER SPIFFS is mounted (i.e. after adif_log_init()).
void diag_log_persist_start(void);

// Path to the flash-persisted log file (served by the web UI's /api/log/saved).
const char *diag_log_persist_path(void);

#ifdef __cplusplus
}
#endif
