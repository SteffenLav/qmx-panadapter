#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the 1Hz status-bar task: reads battery + WiFi state and pushes
// formatted strings into the bottom-bar's per-zone labels.
void status_bar_start(void);

// "Later" was pressed on the update window while an update was sitting ready.
// Gives the bottom bar back - battery, clock, WiFi - and leaves only the small
// "vX.Y.Z ready - tap" reminder in the version slot. Without this the takeover
// was permanent: declining an update meant never seeing your own bar again
// until you restarted (operator, 2026-08-23). Re-armed automatically the next
// time the OTA state is anything other than "ready", so a fresh download gets
// the operator's attention again.
void status_ota_banner_dismiss(void);

#ifdef __cplusplus
}
#endif