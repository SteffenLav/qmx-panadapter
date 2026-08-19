// panic_hook — carry a crash across the reboot so the diag log can report it.
//
// THE PROBLEM (#117). Panics print straight to UART through the ROM/panic path
// and never reach diag_log's vprintf hook, so the assert text, the register
// dump and the backtrace exist ONLY if a serial capture happened to be running
// at that second. Verified across four sources on 2026-08-14: the serial
// capture held both overnight asserts and both register dumps, while
// /api/log/saved, SD qmx-log.txt and a 5 MB SD qmx-log.1.txt going back to
// 26 July held ZERO asserts and ZERO register dumps between them.
//
// So a field crash is currently undiagnosable. The operator is asked for a
// diagnostic download and it contains everything except the one thing needed.
// All that survives is the next boot's reset_reason=panic/exception, which
// CLAUDE.md already records as ambiguous - a forced power-off and every
// idf.py flash reset read identically, so it cannot even be counted as a crash.
//
// WHY NOT FLUSH TO FLASH FROM THE HANDLER, which is the obvious design: a panic
// handler runs with interrupts off and the other core halted, and the cache may
// be disabled. A SPIFFS write there can HANG instead of rebooting, which turns
// a diagnosable crash into a dead device - strictly worse than the bug. It also
// cannot take diag_log's spinlock or run diag_log_persist's 30 s task.
//
// WHAT THIS DOES INSTEAD: the handler only memcpy's a small record into RTC
// no-init RAM, which survives a warm reset (and every panic reset is warm).
// The NEXT boot finds it and logs it through the ordinary path, so it lands in
// the ring, in /api/log, and in /spiffs/diag.log via the normal persist task -
// no flash access from panic context at all.
//
// A bonus that matters: a valid magic here is POSITIVE proof the last reset was
// a genuine crash, which reset_reason alone can never be. No magic after a
// panic/exception reset means a forced power-off or a flash.
//
// ⚠ Limitation, deliberate: RTC RAM does not survive a full power cycle, so a
// crash followed by pulling the battery is still lost. Nothing short of writing
// to flash from panic context can cover that, and that is the trade above.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Log the previous boot's crash record, if there is one, and consume it so it
// is reported exactly once. Safe to call when there is none.
//
// Call AFTER diag_log_init() (so the lines are captured) and as early as
// possible otherwise, so a crash report cannot be lost to a second crash
// during start-up.
void panic_hook_report_previous(void);

// True if the last reset was a genuine panic with a record to prove it.
// Distinct from esp_reset_reason() == ESP_RST_PANIC, which is ambiguous here.
bool panic_hook_previous_was_crash(void);

#ifdef __cplusplus
}
#endif
