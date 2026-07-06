#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the 1Hz status-bar task: reads battery + WiFi state and pushes
// formatted strings into the bottom-bar's per-zone labels.
void status_bar_start(void);

#ifdef __cplusplus
}
#endif