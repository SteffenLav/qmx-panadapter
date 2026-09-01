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

// ⚠ A SLIDING WINDOW ALONE IS DEFEATED BY A SLOW TOGGLE (#194, Randy N4OPI).
// He runs from USB-C with no NP-F550 fitted and the readout "toggles between
// 100% (8.4v) and 0% (4.2v)". That report is itself the evidence for how:
// if the rail alternated every sample, every window would span 4200 mV, the
// latch would hold, and he would see the struck-through no-battery icon. He sees
// PERCENTAGES, so each plateau must outlast the 5-sample window - so nearly every
// window looks perfectly stable, only the few spanning a transition trip the
// spread test, and BATT_DET_STABLE_REQ then flipped it back to "present" ~15 s
// later. The detector was oscillating in step with the rail.
//
// Two observations a sliding window cannot make, both added below:
//   * a real pack cannot STEP - between two 1 s samples it moves by IR drop at
//     most (CHARGE_IR_DROP_MV is ~200 mV), never volts. This catches the jump
//     however long each plateau lasts.
//   * the device cannot be RUNNING off a 2S pack reading 4.2 V. Below the floor
//     is not a flat battery, it is no battery.
#define BATT_DET_STEP_MV      1000   // impossible move between consecutive samples
#define BATT_DET_FLOOR_MV     6000   // below this it cannot be a live 2S pack
                                      // (BATTERY_MIN_MV 6600 with margin)

// Recovery must outlast the toggle, or the latch just flaps in sympathy with it.
// Raised 15 -> 60 and every sample in the run must now be plausible AND
// step-free, so a rail that keeps returning to an impossible value can never
// accumulate a full run. Cost: a pack genuinely inserted at runtime is trusted
// after ~60 s instead of ~15 s, which is a fair price for not flickering.
#define BATT_DET_STABLE_REQ   60

static uint32_t s_det_win[BATT_DET_WINDOW];
static int      s_det_count = 0;
static int      s_det_head  = 0;
static int      s_present   = -1;    // -1 unknown, 0 absent, 1 present
static int      s_stable_n  = 0;     // consecutive good samples while absent
static uint32_t s_last_mv   = 0;     // 0 = no previous sample yet

static void battery_track(uint32_t mv)
{
    // --- per-sample tests, independent of the window ---
    uint32_t step = 0;
    if (s_last_mv) step = (mv > s_last_mv) ? (mv - s_last_mv) : (s_last_mv - mv);
    bool impossible = (step > BATT_DET_STEP_MV) || (mv < BATT_DET_FLOOR_MV);
    s_last_mv = mv;

    s_det_win[s_det_head] = mv;
    s_det_head = (s_det_head + 1) % BATT_DET_WINDOW;
    if (s_det_count < BATT_DET_WINDOW) s_det_count++;
    if (s_det_count < 3) return;     // need a few samples before judging

    uint32_t lo = s_det_win[0], hi = s_det_win[0];
    for (int i = 1; i < s_det_count; i++) {
        if (s_det_win[i] < lo) lo = s_det_win[i];
        if (s_det_win[i] > hi) hi = s_det_win[i];
    }
    bool erratic = (hi - lo) > BATT_DET_SPREAD_MV;

    if (erratic || impossible) {
        s_stable_n = 0;
        if (s_present != 0) {
            s_present = 0;           // erratic/impossible rail -> no pack
            if (impossible)
                ESP_LOGW(TAG, "no battery detected (%lu mV, step %lu mV - a pack cannot do this)",
                         (unsigned long)mv, (unsigned long)step);
            else
                ESP_LOGW(TAG, "no battery detected (rail swing %lu mV over window)",
                         (unsigned long)(hi - lo));
        }
        return;
    }

    if (s_det_count < BATT_DET_WINDOW) return;
    if (s_present == 1) { s_stable_n = 0; return; }   // already present
    if (s_present == -1) {
        s_present = 1;               // stable across the full window at startup
        ESP_LOGI(TAG, "battery present (rail stable, swing %lu mV)",
                 (unsigned long)(hi - lo));
        return;
    }
    // Was absent. Every sample in this run has been plausible and step-free.
    if (++s_stable_n >= BATT_DET_STABLE_REQ) {
        s_present = 1;
        s_stable_n = 0;
        ESP_LOGI(TAG, "battery present (rail stable for %ds, swing %lu mV)",
                 BATT_DET_STABLE_REQ, (unsigned long)(hi - lo));
    }
}

// false only once an absent pack has been positively detected; "unknown"
// (first few seconds) counts as present so we don't flash the no-battery
// icon on a unit that does have one. Re-evaluated continuously so a battery
// plugged in at runtime is picked up once the rail settles.
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

int battery_mv_to_level(int mv)
{
    if (mv <= BATTERY_MIN_MV) return 0;
    if (mv >= BATTERY_MAX_MV) return 100;
    return (int)(((mv - BATTERY_MIN_MV) * 100) / (BATTERY_MAX_MV - BATTERY_MIN_MV));
}

// Compensates a real, confirmed-on-hardware artifact: the INA226 reads the
// pack's TERMINAL voltage, which while charge current is actually flowing
// sits ~200 mV (~10 percentage points on this pack's linear map) above the
// true resting voltage, due to the pack's own internal resistance
// (V_terminal = V_true + I_charge * R_internal). Left uncompensated this
// makes every consumer - the status-bar %/icon, the web API, and (most
// importantly) util/status.c's charge-limit cutoff decision - see a reading
// that jumps ~10 points the instant charging starts and drops back the
// instant it stops, rather than the battery's true, slowly-changing SoC.
// Reported and reproduced on real hardware (2026-07-07): a charge-limit set
// close to the true resting level got stuck permanently just below it (the
// inflated reading crossed the limit before any real charging happened, then
// relaxed back below limit-hysteresis and never resumed); a limit further
// away oscillated rapidly instead of charging smoothly up to it. Applying
// this compensation centrally here (not just in the charge-limit decision)
// also fixes the visual jump the user sees on the displayed %/voltage.
#define CHARGE_IR_DROP_MV 200

int battery_get_level(void)
{
    if (!s_initialised) return -1;

    int mv = battery_get_mv();
    if (mv < 0) return -1;
    return battery_mv_to_level(mv);
}

int battery_get_mv(void)
{
    if (!s_initialised) return -1;

    uint32_t mv;
    if (ina226_read_bus_mv(&mv) != ESP_OK) return -1;
    battery_track(mv);   // feed the no-battery detector with the RAW reading (called every status poll)

    if (battery_is_charging()) {
        int32_t compensated = (int32_t)mv - CHARGE_IR_DROP_MV;
        return compensated > 0 ? compensated : 0;
    }
    return (int)mv;
}

bool battery_is_charging(void)
{
    if (!s_initialised) return false;

    int32_t ma;
    if (ina226_read_shunt_ma(&ma) != ESP_OK) return false;
    return ma < CHARGING_THRESHOLD_MA;
}