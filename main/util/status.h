#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the 1Hz status-bar task: reads battery + WiFi state and pushes
// a formatted string into the bottom-bar label via ui_set_fps_text().
void status_bar_start(void);

#ifdef __cplusplus
}
#endif