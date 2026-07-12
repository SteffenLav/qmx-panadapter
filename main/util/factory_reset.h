#pragma once

#include <stdbool.h>

// Selective NVS reset — clears stuck persisted state without an esptool
// full-chip erase, so a bad stored value can be cleared in the field while
// keeping the QSO log (ADIF lives in SPIFFS, which is never touched here).
//
// A normal firmware flash only rewrites the app image (0x10000) and leaves the
// NVS partitions intact, so a stuck value survives every reflash — the reason a
// clean/erase flash was previously the only cure. These two resets replicate
// exactly what a clean flash does to NVS, but selectively:
//
//   settings  -> erases the "user_nvs" partition (all app settings + memory
//                channels). Keeps the ADIF QSO log and WiFi/system state.
//   network   -> erases the default "nvs" partition (WiFi credentials, PHY
//                calibration, esp_hosted state). Keeps all app settings.
//
// The erase is deferred to the *next* boot: a request sets a flag in RTC memory
// (retained across esp_restart) and reboots; factory_reset_apply_pending() then
// erases the partition(s) at the very top of app_main, before any NVS handle is
// opened. Erasing a partition out from under a live handle is unsafe, so this
// reboot-then-erase ordering is deliberate.

#ifdef __cplusplus
extern "C" {
#endif

// Erase any partitions flagged by a prior factory_reset_request(). Call this
// FIRST in app_main, before nvs_flash_init()/settings_init(). No-op on a normal
// boot (including a cold power-on: the RTC flag is magic-guarded).
void factory_reset_apply_pending(void);

// Flag the requested partition(s) for erase and reboot to apply them (after a
// short delay so an in-flight HTTP response can flush first). At least one of
// the two flags should be true; passing both replicates a clean flash's effect
// on NVS while preserving the ADIF log.
void factory_reset_request(bool reset_settings, bool reset_network);

#ifdef __cplusplus
}
#endif
