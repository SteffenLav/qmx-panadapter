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
#include <stddef.h>   // size_t (sd_archive_read_adif)

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
//
// 2026-07-10: the WiFi *wedge* permanence was root-caused + fixed (the
// esp_hosted SDIO oversize-length livelock now self-heals — see
// tools/patches/apply_esp_hosted_sdio_recovery.ps1 and the SD-wedge section in
// CLAUDE.md), so the card was briefly re-enabled (flag=0). But a live test
// found a SEPARATE cost: with SD mounted, internal heap dropped to ~15 KB min /
// 16 KB largest-block (vs ~45 KB with SD off), and FT8 decode collapsed into
// zero-clusters on a busy band (cand=140, dec=0 — the internal-pinned decode
// FFT/buffers starve). So SD-on is a WiFi-safe now but FT8-hostile trade: kept
// DISABLED. Re-enabling would need the SD mount's internal-RAM cost reduced OR
// the mount gated off while in FT8 mode.
// 2026-07-19: RE-ENABLED (flag=0) for a fresh soak. The "SD mount starves FT8
// decode into cand=140/dec=0 zero-clusters" verdict above was reached BEFORE
// #51 was solved - and cand=140/dec=0 is the exact #51 signature (USB audio
// lost at the wire, NOT internal-heap starvation of the decode buffers). #51
// was root-caused + fixed 2026-07-19 (USB ISO pipeline depth 9 ms -> 320 ms),
// so the old blocker is very plausibly gone. Test: enable, mount a card, run
// FT8 on a busy band, read the existing per-slot log line - heap_i min/lblk
// (real, SD-independent cost) AND dec= (the #51 collapse tell). If yield holds
// (~16 uniq/slot) with SD mounted, SD is viable and this becomes a shipped
// feature; if it still collapses with healthy heap, the internal-RAM cost is a
// genuine separate issue (reduce mount footprint OR gate SD off in FT8 mode).
#define SD_ARCHIVE_DISABLED 0

// Spawn the background archive task. Call once from app_main after settings,
// ADIF, and config storage are initialised. Cheap; the task does the probing.
void sd_archive_init(void);

// Whether a card is currently mounted and being mirrored to. Drives the
// bottom-bar "SD" indicator. Safe from any task.
bool sd_archive_is_mounted(void);

// Block until the boot-window mount has settled, up to timeout_ms. Returns true
// if a card is mounted. Intended for one caller: app_main, so the Reader's staged
// offline manual can be flushed to the card BEFORE panadapter_wifi_start() - the
// only window in which SD writes are reliable on this board. Returns quickly on
// a normal boot (the mount either succeeds within ~1 s or the retries are done).
bool sd_archive_wait_mounted(uint32_t timeout_ms);

// Mark a source file dirty so the background task re-mirrors it on its next
// pass. Cheap and safe from any task, even before sd_archive_init().
void sd_archive_mark_adif_dirty(void);
void sd_archive_mark_config_dirty(void);
// Re-mirror the LoTW certificate + private key to the card (call after import).
void sd_archive_mark_lotw_dirty(void);

// Full path to the mirrored diagnostic log on the card (valid only while
// mounted). The web server reads this under sd_archive_lock()/_unlock() so its
// read never races the archive task's writes (FatFs has no internal locking).
const char *sd_archive_log_path(void);

// Read one of the card's two QSO logs into a PSRAM buffer, which the CALLER
// frees: qso.adi, or with previous=true the qso.prev.adi safety copy. Returns
// NULL if there is no card, no such file, or no memory; *out_len is only
// written on success. Pauses the spectrum stream for the duration, like every
// other bulk SD read here. See adif_log_import_from_sd().
char *sd_archive_read_adif_file(bool previous, size_t *out_len);

bool sd_archive_lock(uint32_t timeout_ms);
void sd_archive_unlock(void);

// Free/total bytes on the mounted card. Returns false if no card is mounted
// (out params left untouched) or the archive task's lock couldn't be
// acquired quickly - never blocks. Safe from any task.
bool sd_archive_get_free_bytes(uint64_t *out_free, uint64_t *out_total);


// ---------------------------------------------------------------------------
// TEMP INSTRUMENT (#282) - remove with the diagnosis.
//
// Counters kept in RTC no-init RAM so they survive every warm reset and can be
// read days later from /api/status ("sd_instr"), rather than needing a serial
// capture to be running at the moment the fault recurs. Cleared by a full power
// cycle only; boot_id says which boot a count belongs to.
//
// The two numbers being hunted are handle_no_mount and park_reentered - both
// are states this file's own logic says cannot occur, and both were implied by
// the 2026-08-28 capture. Any non-zero value is the answer.
typedef struct {
    uint32_t boot_id;
    uint32_t mount_enter;
    uint32_t mount_ok;
    uint32_t handle_no_mount;
    uint32_t unmount_calls;
    uint32_t park_set;
    uint32_t park_reentered;
    uint32_t first_anom_uptime_s;
    uint32_t first_anom_boot;
} sd_archive_instr_t;

void sd_archive_instr_get(sd_archive_instr_t *out);

#ifdef __cplusplus
}
#endif
