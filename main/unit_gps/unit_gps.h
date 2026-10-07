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
 * The pins are shared with the remote power-cycle relay. The Port mode
 * setting decides which module drives them; this module does not (see
 * util/gpio_relay.c and settings_set_port_a_mode()). The mode starts
 * this module only when the cable is a GPS. RX is GPIO54, with the
 * pull-up ON. TX is not connected to a pin (UART_PIN_NO_CHANGE). An
 * idle TX line stays HIGH. GPIO53 is one of the relay pins, and an
 * active-high relay treats HIGH as its active level. So a relay
 * harness left plugged in while in GPS mode would hold the radio in
 * power-cycle. Version 1 never transmits, and the pin stays at hi-Z.
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
/* Start on PORT.A (GPIO54) - the M5Stack Unit GPS v1.1, ericmoritz' original
 * target. Equivalent to unit_gps_start_on(UNIT_GPS_PORTA_RX_GPIO). */
bool unit_gps_start(void);

/* ⭐ Start on an arbitrary RX pin, for a receiver that is not on PORT.A.
 *
 * Steffen's Module GPS v2.1 (M5Stack M003-V21) is the same AT6668 silicon
 * behind an ATGM336H-6N can, same NMEA 0183 4.1 at 115200 8N1, so the parser
 * and this driver are unchanged - only the pin differs. It mounts on the 30-pin
 * M-Bus on the back rather than PORT.A, so it does NOT contend with the relay
 * at all and port_a_mode does not apply to it: relay and GPS can both run.
 *
 * ⛔ ONE RECEIVER AT A TIME. There is a single UART_NUM_1 and a single parser
 * state machine, so a second start is refused rather than quietly rebinding the
 * pin and leaving the caller thinking it won.
 */
bool unit_gps_start_on(int rx_gpio);

/* RX pin for a Module GPS v2.1 on the M-Bus with its TXD DIP switch at
 * position 5.
 *
 * ⚠ DERIVED, NOT DOCUMENTED. The module's silkscreen tabulates CORE, CORE2,
 * CORES3 and HP135 - there is no Tab5 column. The switches select M-BUS PIN
 * POSITIONS, so the Core column inverts onto the Tab5's own bus table:
 * TXD switch 5 is Core G0, which is M-Bus position 21, which on the Tab5 is
 * GPIO2. Position 21 was chosen over the alternatives because GPIO2 is a plain
 * GPIO used nowhere in this firmware or the BSP, and it avoids both the
 * strapping pins (switch 1 -> G37) and the pins the Tab5 names PC_TX/PC_RX as
 * if it intends to drive them (switch 2 -> G6).
 *
 * The RXD switches should all be left OFF: this driver is receive-only and
 * passes UART_PIN_NO_CHANGE for TX, so the Tab5 never talks to the receiver.
 */
#define UNIT_GPS_PORTA_RX_GPIO   54
#define UNIT_GPS_MBUS_RX_GPIO     2

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
