#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// Thin driver for the Epson RX8130CE RTC on the M5Stack Tab5 SYS I2C bus.
// I2C address 0x32, supercap-backed (~30-40 h retention).
// Registers 0x10-0x16: SEC, MIN, HOUR, WEEKDAY, DAY, MONTH, YEAR — all BCD.
// Register 0x1D bit 0x80: VBLF (power failure / time invalid).

esp_err_t rtc_init(i2c_master_bus_handle_t bus);

// True if the RTC has valid time (VBLF flag clear and supply was maintained).
bool rtc_is_valid(void);

// Read the current RTC time into *out (UTC, all fields populated).
// Returns false on I2C error or if rtc_init() was not called.
bool rtc_get_time(struct tm *out);

// Write tm to the RTC. Does not check rtc_is_valid() first.
bool rtc_set_time(const struct tm *tm);

// Convenience: read RTC and call settimeofday(). Returns false if invalid or error.
bool rtc_apply_to_system(void);
