#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise INA226 on the given I2C bus at the given address (Tab5: 0x41).
// Configures 16 averages, 1.1ms conv times, continuous shunt+bus mode.
esp_err_t ina226_init(i2c_master_bus_handle_t bus, uint8_t addr);

// Bus voltage in millivolts. Unsigned 16-bit raw, 1.25 mV/LSB.
esp_err_t ina226_read_bus_mv(uint32_t *mv);

// Shunt current in milliamps (signed). On Tab5: negative = charging.
// Computed in software from raw shunt voltage / 0.005 ohm shunt.
esp_err_t ina226_read_shunt_ma(int32_t *ma);

#ifdef __cplusplus
}
#endif