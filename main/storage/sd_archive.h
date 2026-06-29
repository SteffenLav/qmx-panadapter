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
//   - screenshots/  : timestamped BMPs saved on demand
//
// The Tab5 routes no card-detect line to the SoC, so card presence is
// discovered by periodically attempting a mount; removal is detected when a
// file write starts failing. All work happens on a low-priority background
// task so it never competes with audio/FFT/FT8.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

// Capture the current screen and save it as a timestamped BMP under
// /sdcard/qmx-panadapter/screenshots/. On success returns ESP_OK and, if
// out_path is non-NULL, copies the saved path into it. Returns an error if no
// card is mounted or the capture/write failed. Safe to call from the web task.
esp_err_t sd_archive_save_screenshot(char *out_path, size_t out_path_len);

#ifdef __cplusplus
}
#endif
