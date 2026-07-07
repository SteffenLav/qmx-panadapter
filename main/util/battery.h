#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time initialisation of the INA226 battery monitor.
// Must be called once at boot, after PI4IO init, before status_bar_start().
esp_err_t battery_init(i2c_master_bus_handle_t bus);

// Battery state of charge, 0-100. Returns -1 if unknown / uninitialised.
int battery_get_level(void);

// Pure mV->percent conversion (same linear map battery_get_level() uses),
// exposed so callers can apply it to an ADJUSTED voltage - see
// util/status.c's charge-limit IR-drop compensation for why.
int battery_mv_to_level(int mv);

// Battery pack voltage in millivolts. Returns -1 if unknown / uninitialised.
int battery_get_mv(void);

// True when current is flowing into the battery (charging).
bool battery_is_charging(void);

// False only once a missing pack has been positively detected (latched after a
// few seconds of erratic rail voltage). True otherwise, including the first
// seconds before a verdict. Drives the "no battery" icon in the status bar.
bool battery_present(void);

#ifdef __cplusplus
}
#endif