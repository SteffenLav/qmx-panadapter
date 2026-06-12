#include "battery.h"
#include "ina226.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "battery";

// Tab5 hardware: 2S LiPo, INA226 at I2C 0x41
// SoC: linear voltage curve, 3.3V/cell = 0%, 4.15V/cell = 100%
//   pack 6.6V = 0%, pack 8.3V = 100%, range 1700 mV
// Charging: INA226 shunt current sign (NOT the CHG_STAT pin which lingers).
//   Tab5 polarity is opposite M5Unified docstring: negative current = charging.
//   30 mA threshold filters topping-up noise.
// Reference: N6HAN qrp_companion battery_indicator.txt

#define BATTERY_MIN_MV          6600
#define BATTERY_MAX_MV          8300
#define CHARGING_THRESHOLD_MA   (-30)

static bool s_initialised = false;

esp_err_t battery_init(i2c_master_bus_handle_t bus)
{
    esp_err_t err = ina226_init(bus, 0x41);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ina226_init failed: %d", err);
        return err;
    }
    s_initialised = true;
    ESP_LOGI(TAG, "INA226 ready");
    return ESP_OK;
}

int battery_get_level(void)
{
    if (!s_initialised) return -1;

    uint32_t mv;
    if (ina226_read_bus_mv(&mv) != ESP_OK) return -1;

    if (mv <= BATTERY_MIN_MV) return 0;
    if (mv >= BATTERY_MAX_MV) return 100;
    return (int)(((mv - BATTERY_MIN_MV) * 100) / (BATTERY_MAX_MV - BATTERY_MIN_MV));
}

int battery_get_mv(void)
{
    if (!s_initialised) return -1;

    uint32_t mv;
    if (ina226_read_bus_mv(&mv) != ESP_OK) return -1;
    return (int)mv;
}

bool battery_is_charging(void)
{
    if (!s_initialised) return false;

    int32_t ma;
    if (ina226_read_shunt_ma(&ma) != ESP_OK) return false;
    return ma < CHARGING_THRESHOLD_MA;
}