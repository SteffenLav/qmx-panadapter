#pragma once

// Remote GPIO relay pulse - Randy N4OPI's request (2026-09-04): most of his
// QMXs already carry a 2.5mm jack wired to PWR_ON/GND so a home-automation
// relay can power them on/off, and a remote Tab5 firmware upgrade needs a
// manual QMX power cycle afterward (see CLAUDE.md's #74) - which defeats the
// point of doing the upgrade remotely if there is nobody at the bench to flip
// the switch. This drives a Tab5 GPIO to trigger that same relay.
//
// ⛔ WHITELISTED TO EXACTLY TWO PINS, GPIO53/54 - not "any GPIO you ask for".
// Both are genuinely free: BSP_EXT_I2C_SDA/SCL in the m5stack_tab5 BSP header,
// which nothing on this board currently uses (the snap-on keyboard's I2C is
// GPIO0/1 - a DIFFERENT bus, see keyboard/tab5_keyboard.c - and
// BSP_POWER_AMP_IO, historically GPIO53, is disabled/GPIO_NUM_NC in the BSP
// as shipped). An arbitrary-pin API from the web would be a real hazard on a
// board this densely wired; these two are the only ones with nothing else on
// them.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, AFTER settings_init(). Configures both whitelisted pins
// as outputs resting on the INACTIVE side of the stored polarity - open-relay
// is the safe default, and a contact closure must require a deliberate pulse.
//
// ⛔ This used to rest both pins at a fixed LOW, which for an operator who had
// chosen active LOW meant the relay was held CLOSED from boot until the first
// pulse released it (Randy N4OPI, 2026-09-06). "Resting" is a statement about
// polarity, not about a voltage.
void gpio_relay_init(void);

// Re-apply the resting level after the stored polarity changes. Without this
// the boot fix only helps until the operator edits the setting, and the pin
// then sits on the wrong side until the next reboot - the same bug, deferred.
// Safe mid-pulse: it retargets where the pulse RETURNS to rather than cutting
// it short.
void gpio_relay_set_polarity(bool active_level);

// Drive `pin` (53 or 54 ONLY) to `level` for `ms`, then return it to the
// opposite level. Runs asynchronously via a one-shot timer - never blocks
// the caller, so it is safe to call directly from an HTTP handler.
//
// Refuses (returns false, fills `err`) and does nothing if: pin is not 53 or
// 54; ms is 0 or over GPIO_RELAY_MAX_MS (a request that could leave a relay
// stuck energised for an unbounded time is refused outright, not clamped -
// clamping silently does something other than what was asked); or a pulse on
// EITHER pin is already in flight (one at a time - a second request while
// the first is mid-pulse could only mean a stuck client retrying, and firing
// both together is exactly the "arbitrary GPIO drive" hazard this whole
// module exists to avoid).
bool gpio_relay_pulse(uint8_t pin, bool level, uint16_t ms, char *err, size_t errlen);

// True while a pulse is actively driving a pin - for the web UI to grey the
// button and for /api/status.
bool gpio_relay_busy(void);

#define GPIO_RELAY_MIN_MS   50
#define GPIO_RELAY_MAX_MS   5000   /* a QMX long-press is a few seconds, not more */

// Deterministic power-cycle sequence (Randy N4OPI, 2026-09-13). A single
// gpio_relay_pulse() on a PWR_ON line only TOGGLES the QMX - one click turns
// it off, the next turns it on, and there was no way to tell which, or
// whether it worked. This runs the whole sequence he asked for: pulse `pin`
// active for `off_ms` (the operator's own "how long to hold it" figure),
// wait 1 s, pulse 500 ms to press it back on, wait 2 s, then poll CAT for a
// response - up to a further ~8 s, since a fixed single check 2 s after power
// risked reporting a QMX that was still coming up as "failed".
//
// Runs asynchronously via esp_timer, same as gpio_relay_pulse - never blocks
// the caller. Refuses (returns false, fills `err`) on the same grounds as
// gpio_relay_pulse (bad pin, bad ms, a relay operation already running), and
// does not repeat those checks in its own callers.
bool gpio_relay_power_cycle_start(uint8_t pin, bool level, uint16_t off_ms,
                                   char *err, size_t errlen);

typedef enum {
    GPIO_PC_IDLE = 0,    // never run since boot, or the last run's result was read
    GPIO_PC_RUNNING,
    GPIO_PC_OK,          // QMX answered CAT after the on-pulse
    GPIO_PC_FAILED,      // on-pulse refused, or no CAT response within the window
} gpio_pc_status_t;

// Current phase of the power-cycle sequence - for /api/status. OK/FAILED are
// LATCHED, not auto-cleared: the sequence runs on its own timers, well
// outside the 1 Hz status poll's cadence, and a status good for exactly one
// read could be missed by whichever poll happens to land on it. It stays at
// OK/FAILED until the next gpio_relay_power_cycle_start() call moves it back
// to RUNNING - a client wanting "show this result once" diffs against the
// last value it saw, same as the rest of this device's live status fields.
gpio_pc_status_t gpio_relay_power_cycle_status(void);

#ifdef __cplusplus
}
#endif
