// Background microSD auto-archive.
//
// If a FAT-formatted microSD card is present in the Tab5 slot, this module
// keeps an up-to-date mirror of the device's important files under
// /sdcard/qmx-panadapter/:
//   - qmx-log.txt   : the always-on diagnostic log (rotated at 5 MB to
//                     qmx-log.1.txt) — the off-chip persistence path, so a
//                     crash's log survives the reboot that follows it.
//   - qso.adi       : the ADIF QSO log (re-copied after each logged QSO)
//   - qmx-config.txt: the full config/settings export (re-written on change)
//
// The Tab5 routes no card-detect line to the SoC, so card presence is
// discovered by periodically attempting a mount; removal is detected when a
// file write starts failing. All work happens on a low-priority background
// task so it never competes with audio/FFT/FT8.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SD-ARCHIVE SOFT-DISABLE (2026-07-09): the mounted card's SDMMC/SPI traffic
// shares one physical peripheral with the C6 WiFi co-processor's SDIO link
// (see CLAUDE.md "SD/WiFi shared-SDMMC wedge" - SPI mode was already a
// mitigation, not a cure; the residual wedge mechanism was never fully
// root-caused across several dedicated debugging sessions). Reproduced AGAIN
// 2026-07-09 during ordinary FT8 use, twice in one session, each time
// requiring a physical power-cycle to recover (no in-firmware recovery
// exists once it hits) - not an acceptable standing landmine, so this single
// flag is the entire on/off switch: sd_archive_init() early-returns and the
// background task is never spawned, so the card is never even mounted (same
// effect as not having a card in the slot at all). ADIF/config/diag-log data
// is unaffected - it all already lives in SPIFFS/NVS regardless; only the
// redundant SD-card copy is skipped. No code was deleted - flip this back to
// 0 to fully re-enable once the actual root cause is found and fixed. See
// memory project_sd_wedge_adif_repro_2026_07_09 for the specific repro this
// time (settings flush -> SD mirror -> wedge within ~12s, no SD-card-side
// error visible before the wedge signature appears).
#define SD_ARCHIVE_DISABLED 1

// Spawn the background archive task. Call once from app_main after settings,
// ADIF, and config storage are initialised. Cheap; the task does the probing.
void sd_archive_init(void);

// Whether a card is currently mounted and being mirrored to. Drives the
// bottom-bar "SD" indicator. Safe from any task.
bool sd_archive_is_mounted(void);

// Mark a source file dirty so the background task re-mirrors it on its next
// pass. Cheap and safe from any task, even before sd_archive_init().
void sd_archive_mark_adif_dirty(void);
void sd_archive_mark_config_dirty(void);

// Full path to the mirrored diagnostic log on the card (valid only while
// mounted). The web server reads this under sd_archive_lock()/_unlock() so its
// read never races the archive task's writes (FatFs has no internal locking).
const char *sd_archive_log_path(void);
bool sd_archive_lock(uint32_t timeout_ms);
void sd_archive_unlock(void);

// Free/total bytes on the mounted card. Returns false if no card is mounted
// (out params left untouched) or the archive task's lock couldn't be
// acquired quickly - never blocks. Safe from any task.
bool sd_archive_get_free_bytes(uint64_t *out_free, uint64_t *out_total);

#ifdef __cplusplus
}
#endif
