#pragma once

#include <stdbool.h>

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

// True while charging is deliberately capped at the operator's charge-limit
// setting (Don N2VGU: no way to tell "capped on purpose" from "not charging
// for some unknown reason"). Exposed so /api/status can carry it too - the
// same question is at least as useful checked remotely as on the Tab5 screen.
bool status_charge_limit_active(void);


#ifdef __cplusplus
}
#endif