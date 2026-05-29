#include "battery.h"
#include <stdbool.h>

// TODO: replace with real INA226 reads.
// See N6HAN qrp_companion: 2S LiPo, SoC = clamp((vbus - 6.6) / 1.7 * 100, 0, 100);
// charging = (shunt_mA < -30).  INA226 at I2C 0x40.

int battery_get_level(void)
{
    return 87;  // stub
}

bool battery_is_charging(void)
{
    return false;  // stub
}