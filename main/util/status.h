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
/* The battery percentage AS SHOWN on the Tab5 bar and in /api/status.
 *
 * Capped at the user's charge limit when battery care is on: with a limit of
 * 80 % the pack really settles around 85 % (the fixed IR-drop compensation
 * over-corrects as the charge current tapers, so the cutoff fires late), and a
 * bar reading "85 % (limit)" after you asked for 80 % is indefensible to a
 * user. min(), not a clamp - the true value returns once the pack drains below
 * the limit, and the cutoff logic still uses the raw battery_get_level().
 *
 * ⛔ This HIDES the overshoot, it does not fix it. See the long note at the
 * definition in status.c for the measurement and the real fix. */
int status_battery_display_level(void);

bool status_charge_limit_active(void);


#ifdef __cplusplus
}
#endif