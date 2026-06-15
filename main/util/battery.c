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

// --- no-battery detection ---------------------------------------------------
// On the battery-less Tab5 (cheaper SKU, run from USB), the INA226 reads an
// erratic rail voltage that swings across the whole range, so the icon flickers
// empty<->full. A real 2S pack is stable to a few tens of mV over seconds, even
// while charging, so we watch the spread of recent readings (over a short
// sliding window, NOT since boot — a real pack legitimately drifts >1V over a
// full charge) and LATCH "absent" once it swings more than any pack ever could.
#define BATT_DET_WINDOW       5      // samples (~5 s at the 1 Hz status poll)
#define BATT_DET_SPREAD_MV    1000   // window swing a real pack never shows

static uint32_t s_det_win[BATT_DET_WINDOW];
static int      s_det_count = 0;
static int      s_det_head  = 0;
static int      s_present   = -1;    // -1 unknown, 0 absent (latched), 1 present

static void battery_track(uint32_t mv)
{
    if (s_present == 0) return;      // absent is latched (no runtime hot-plug)
    s_det_win[s_det_head] = mv;
    s_det_head = (s_det_head + 1) % BATT_DET_WINDOW;
    if (s_det_count < BATT_DET_WINDOW) s_det_count++;
    if (s_det_count < 3) return;     // need a few samples before judging

    uint32_t lo = s_det_win[0], hi = s_det_win[0];
    for (int i = 1; i < s_det_count; i++) {
        if (s_det_win[i] < lo) lo = s_det_win[i];
        if (s_det_win[i] > hi) hi = s_det_win[i];
    }
    if (hi - lo > BATT_DET_SPREAD_MV) {
        s_present = 0;               // erratic rail -> no pack; latch
        ESP_LOGW(TAG, "no battery detected (rail swing %lu mV over window)",
                 (unsigned long)(hi - lo));
    } else if (s_det_count >= BATT_DET_WINDOW) {
        s_present = 1;               // stable across the full window -> pack present
    }
}

// false only once an absent pack has been positively detected (latched);
// "unknown" (first few seconds) counts as present so we don't flash the
// no-battery icon on a unit that does have one.
bool battery_present(void)
{
    return s_present != 0;
}

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
    battery_track(mv);   // feed the no-battery detector (called every status poll)
    return (int)mv;
}

bool battery_is_charging(void)
{
    if (!s_initialised) return false;

    int32_t ma;
    if (ina226_read_shunt_ma(&ma) != ESP_OK) return false;
    return ma < CHARGING_THRESHOLD_MA;
}