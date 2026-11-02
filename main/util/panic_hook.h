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

/* ---- T138: the breadcrumb for a reset the panic handler never sees ---------
 *
 * THE PROBLEM, and it is the one the record above cannot cover. Michael
 * KZ4LY's v1.23.1 crash came back as reset_reason=other-watchdog
 * (ESP_RST_WDT) after a 99-minute clean run, with no panic, no backtrace and
 * no crash record at all. On the P4 that reason is not a generic "watchdog":
 * esp_system/port/soc/esp32p4/reset_reason.c maps a task-WDT or int-WDT reset
 * that goes THROUGH the panic handler to RESET_REASON_CORE_SW plus a stored
 * hint, which reads back as "task-watchdog" or "interrupt-watchdog".
 * ESP_RST_WDT is the fall-through for the hardware watchdogs with NO hint
 * written - the panic handler never ran.
 *
 * So __wrap_esp_panic_handler() never ran either, and the #117 record was
 * empty. Its absence is not a fault in it; it is the same evidence as the
 * reset reason. Both say the CPU never got a turn.
 *
 * The documented mechanism for that on this board is in main/net/ota_update.c:
 * a stretch with the cache or interrupts off for longer than INT_WDT stage 1
 * can tolerate, so stage 2 resets the chip in hardware.
 *
 * WHAT THIS DOES: the code about to enter such a stretch says so BEFORE it
 * starts, into RTC no-init RAM, and clears it on the way out. Nothing is
 * written by a handler, because no handler gets to run. If a crumb is still
 * there at the next boot, the reset happened INSIDE that stretch and the next
 * boot names it.
 *
 * ⚠ Three limitations, all deliberate:
 *  - RTC RAM does not survive a full power cycle, same trade as the record
 *    above. A watchdog reset is warm, which is the case this is for.
 *  - The OUTERMOST entrant owns the crumb, so with two unrelated stretches
 *    genuinely overlapping it names the first, not necessarily the one that
 *    hung. A depth counter stops an inner exit from clearing it early.
 *  - A site that is not marked cannot be reported. This is evidence about
 *    the sites it is put on, and silence means "not one of these", never
 *    "not a cache-off stretch". */
void panic_hook_crumb_enter(const char *site);
void panic_hook_crumb_exit(void);

// Report a crumb the PREVIOUS boot left behind, and consume it so it is
// reported exactly once. Call it beside panic_hook_report_previous(); the two
// answer different questions and are almost never both present.
void panic_hook_report_previous_crumb(void);

// True if the previous boot reset inside a marked stretch.
bool panic_hook_previous_left_crumb(void);

#ifdef __cplusplus
}
#endif
