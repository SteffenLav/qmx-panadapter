#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the 1Hz status-bar task: reads battery + WiFi state and pushes
// formatted strings into the bottom-bar's per-zone labels.
void status_bar_start(void);

// "Later" was pressed on the update window. The ready line keeps saying what it
// says, but stops breathing - the operator has seen it. Re-armed automatically
// the next time the OTA state is not "ready".
void status_ota_ready_ack(void);


#ifdef __cplusplus
}
#endif