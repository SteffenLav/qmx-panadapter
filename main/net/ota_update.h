// ota_update — install a firmware release from the device itself (#218).
//
// This is the "one tap" half. update_check.c already finds that a newer release
// exists; this downloads and applies it. It sits ALONGSIDE the USB flasher and
// replaces nothing: the flasher stays the recovery path, the way to move
// between arbitrary versions, and the only route on a unit with no WiFi.
//
// ⛔ IT IS NOT AUTOMATIC, AND MUST NOT BECOME AUTOMATIC.
// Applying an update reboots the Tab5, and a Tab5 warm reset with the radio
// attached is the documented #74 trigger - the QMX then fails to re-enumerate
// and needs a power cycle. An unattended update would therefore kill the radio
// of an operator who was not watching, and it would be reported as a QMX fault
// (that inversion has already happened twice). This device also keys a
// transmitter. So the operator asks for it, every time.
//
// Safety, in the order it matters:
//   1. Refused outright while transmitting or mid-QSO - see ota_update_start().
//   2. Written to the inactive OTA slot; the running image is never touched.
//   3. esp_https_ota verifies the image header before the boot slot is switched.
//   4. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: the new image boots PENDING_VERIFY
//      and this firmware marks it valid only once it is properly up. A crash or
//      hang before that reverts to the previous image by itself.
//   5. `factory` still holds whatever was last flashed by cable, so erasing
//      otadata is a final escape hatch even if both OTA slots are bad.

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OTA_IDLE = 0,
    OTA_RUNNING,      // downloading / writing
    OTA_DONE,         // written and verified; reboot pending
    OTA_FAILED,
} ota_state_t;

// Confirm the running image so the bootloader stops treating it as on trial.
// Call once from app_main after the UI and network are up. No-op unless this
// boot is PENDING_VERIFY, so it is harmless on a cable-flashed image.
void ota_update_mark_valid(void);

// Begin an update to `url` (a raw .bin). Returns false and fills `err` if the
// device is in no state to be interrupted - transmitting, mid-QSO, no WiFi, or
// an update already running. Spawns one PSRAM-stack task; progress via
// ota_update_get_state().
bool ota_update_start(const char *url, char *err, size_t err_len);

// Snapshot for /api/status. `pct` is 0-100 while RUNNING, `msg` carries the
// failure reason when FAILED.
ota_state_t ota_update_get_state(int *pct, char *msg, size_t msg_len);

// DEV ONLY (POST /api/cmd {"action":"verify_test","quiet":true|false}). Runs
// only the image verify that esp_https_ota_finish() performs, on the partition
// already written, so a 2-second operation can be tested without a 6-minute
// download in front of it. Logs the duration and result.
void ota_update_verify_test(bool quiet);

// DEV ONLY. `yield_ms` stretches the download so a slow link can be simulated
// on a fast one - the failure being chased only appears on long downloads.
// `pause_feeds` A/Bs the net_quiet hold. Both default to the shipping values.
void ota_update_set_test_params(int yield_ms, bool pause_feeds);

// DEV ONLY (POST /api/cmd {"action":"ota_reset"}). Forget a staged update so
// another download can be started, WITHOUT a reboot - a reflash to clear this
// is a warm reset with the radio attached, which is the #74 trigger. Does not
// touch the written image or the boot partition.
void ota_update_reset_state(void);

// The version being installed, from the incoming image's own descriptor, or ""
// before the header has been read. Both screens name it while the download runs.
void ota_update_get_target_version(char *out, size_t out_sz);

// Bumped on every failure, never reset. A fast failure (bad hostname, DNS
// fails in under 100ms - measured) can complete entirely between two 1 Hz
// status polls with no observable OTA_RUNNING tick in between, so
// ota_state_t alone cannot tell status.c's bottom-bar pulse "this is a NEW
// failure, reset your 3-tick counter" from "still the same one as last
// tick". Compare this against a remembered value instead.
uint32_t ota_update_get_fail_seq(void);

// True once an update has been written and the device is only waiting to be
// restarted into it.
bool ota_update_reboot_pending(void);
