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

/* RAW pack voltage and charge current, straight off the INA226 with no IR
 * compensation applied.
 *
 * battery_get_mv() subtracts a FIXED CHARGE_IR_DROP_MV while charging, and that
 * constant is the problem being measured: at the 2026-10-09 cutoff the real
 * drop was about 105 mV against the 200 mV subtracted, so the limit overshot by
 * ~5 points. Deriving the right model needs V and I together and UNCOOKED -
 * R_internal = dV/dI across a charge on/off transition.
 *
 * Both return false if the read failed; *out is untouched then. ma is signed,
 * and NEGATIVE means current flowing INTO the pack (Tab5 polarity - see the
 * note at the top of battery.c). */
bool battery_get_raw_mv(int *out_mv);
bool battery_get_charge_ma(int *out_ma);

// False only once a missing pack has been positively detected (latched after a
// few seconds of erratic rail voltage). True otherwise, including the first
// seconds before a verdict. Drives the "no battery" icon in the status bar.
bool battery_present(void);

#ifdef __cplusplus
}
#endif