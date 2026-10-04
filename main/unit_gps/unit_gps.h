#pragma once

/* Unit GPS v1.1 (M5Stack, AT6668) on the Tab5's HY2.0-4P PORT.A.
 *
 * This module owns the UART and NOTHING ELSE about time: it receives NMEA,
 * decides what the receiver's pipeline is doing, and hands an accepted fix to
 * time_sync as one thin call. The state machine lives here because two
 * different readers need it - the bottom-bar chip and time_sync's liveness
 * test - and two freshness clocks that disagree would be a known failure
 * mode: both read THIS one.
 *
 * The pins (RX GPIO54, TX GPIO53) are shared with the remote power-cycle
 * relay. Whoever drives them is decided by the Port mode setting, not here -
 * see util/gpio_relay.c and settings_set_port_a_mode(). This module is only
 * ever started when the mode says the cable is a GPS.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nmea_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pipeline state. OFF when this module is not running; the rest are decided
// from the timestamps of what has actually arrived, so reading the state never
// waits for a timer and two readers at different moments cannot disagree.
typedef enum {
    UNIT_GPS_OFF = 0,     // Port mode is not unit_gps (never started / stopped)
    UNIT_GPS_LISTENING,   // running, no well-formed NMEA inside the freshness window
    UNIT_GPS_DEVICE,      // the receiver is talking (well-formed sentences) but has no fix
    UNIT_GPS_LOCKED,      // fresh status-A fix with a sane date and time
    UNIT_GPS_LOST,        // had a fix, it has gone stale (cable pulled, sky lost)
} unit_gps_state_t;

// ONE freshness window, used by the status chip, /api/status and
// time_sync's liveness test alike. 5000 ms: an RMC sentence every second, so
// anything past 5 s of silence means the receiver is gone, and a single
// constant means the chip and the clock authority can never disagree about it.
#define UNIT_GPS_FRESH_MS 5000u

// Bring the UART up (idempotent - a second call is a no-op) and start the
// receive task. Returns false only when the UART itself could not be
// configured, so the Port-mode sequencer can roll the pins back to the relay.
bool unit_gps_start(void);

// Release the UART and end the receive task (idempotent). Safe to call when
// never started; returns once the task has parked.
void unit_gps_stop(void);

// Is the receive path up?
bool unit_gps_running(void);

// Current pipeline state - computed from the timestamps, no polling needed.
unit_gps_state_t unit_gps_state(void);

// Milliseconds since the last ACCEPTED fix, or UINT32_MAX when this session
// has never had one. This is the age both the chip and time_sync read.
uint32_t unit_gps_age_ms(void);

// Liveness in one call: LOCKED and inside UNIT_GPS_FRESH_MS. This is the ONLY
// definition - time_sync_unit_gps_is_live() is a wrapper around it.
bool unit_gps_is_live(void);

// "OFF"/"LISTENING"/"DEVICE"/"LOCKED"/"LOST" - the wire spelling used by
// /api/status. Returns a string literal, never allocated.
const char *unit_gps_state_name(unit_gps_state_t state);

#ifdef __cplusplus
}
#endif
