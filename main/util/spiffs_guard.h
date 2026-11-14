#pragma once
// Write admission for the `storage` SPIFFS partition.
//
// ⛔ SPIFFS CANNOT FAIL A WRITE SAFELY ON A FULL PARTITION. Measured on the
// IDF v5.4.4 SPIFFS code at this partition's geometry (host replay,
// tools/spiffs_host, 2026-11-13): an append that fails part way on a
// full partition can leave the file's index pointing at free pages, after
// which every read past that point fails (-10014/-10015). It happened in
// 8 of 24 replay scenarios for v1.26.2 itself, every one with records over
// one page (251 B of data) - a QSO with name/QTH/comment or activation
// fields. Truncating or retrying after the failure does not undo it.
//
// So the rule is: never START a write that might not finish. Before writing
// `need` bytes, ask SPIFFS to guarantee need + SPIFFS_GUARD_MARGIN bytes:
// esp_spiffs_gc() returns OK only when that much is free with spare blocks
// left, collecting first if it has to, and it decides before any byte of the
// caller's is written. If it cannot, the caller refuses and the file is not
// touched. In the replay this held for 32 of 32 scenarios (records 150-600 B,
// diag traffic 0-12 KB per 30 s): 0 corruptions, 0 writes failing after a
// passed check.
//
// The check and the write must not be separated by another task's write, so
// both happen under spiffs_guard_take(). Every SPIFFS writer takes it except
// settings.c's pcal park, which runs under the settings mutex while the adif
// delete paths take the settings mutex under this one - taking it there would
// be a lock-order deadlock. pcal writes 2.5 KB, inside the margin.
//
// Recursive: adif_log_import_update() holds it across delete + import.
#include <stdbool.h>
#include <stddef.h>

#define SPIFFS_GUARD_MARGIN (8 * 1024)

// Create the mutex. Call once before any SPIFFS writer starts (adif_log_init).
void spiffs_guard_init(void);

// Serialize SPIFFS writes. Blocks until free.
void spiffs_guard_take(void);
void spiffs_guard_give(void);

// Caller holds the guard. True when `need` bytes (plus the margin) can be
// written without SPIFFS running out part way. May garbage-collect.
bool spiffs_guard_reserve(size_t need);
