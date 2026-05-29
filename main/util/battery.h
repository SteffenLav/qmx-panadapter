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

// True when current is flowing into the battery (charging).
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif