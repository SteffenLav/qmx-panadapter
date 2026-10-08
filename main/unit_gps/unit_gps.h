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

/* ⭐ TEMPORARY, 2026-10-08. Start on an arbitrary pin AND an arbitrary baud
 * rate, so the Module GPS v2.1 can be swept from /api/cmd without a flash per
 * attempt. 115200 here is inherited from the Unit GPS v1.1; the ATGM336H-6N on
 * the v2.1 defaults to 9600, and the pipeline state cannot tell a wrong baud
 * from a wrong pin - both sit at LISTENING. baud <= 0 means the default.
 * Fold this back into unit_gps_start_on() once the receiver is known good. */
bool unit_gps_start_on_baud(int rx_gpio, int baud);

/* Instrument readouts for /api/status, valid whatever the Port mode is. */
int      unit_gps_rx_gpio(void);
int      unit_gps_baud(void);
uint32_t unit_gps_rx_bytes(void);
uint32_t unit_gps_rx_lines(void);

/* RX pin for a Module GPS v2.1 on the M-Bus: GPIO38, the Tab5's M-Bus RXD0
 * (bus pin 13), with the module's DIP switch 6 ON.
 *
 * MEASURED 2026-10-08 on the dev bench: DEVICE within seconds, ~530 bytes/s,
 * well-formed NMEA parsing continuously.
 *
 * ⛔ THE SILKSCREEN GROUPS ARE NAMED FROM THE HOST'S SIDE, NOT THE MODULE'S.
 * The module's data OUTPUT is selected by the group the silkscreen labels
 * "RXD" - it drives the host's receive line. Switch 6 is the first of that
 * group. The "TXD" group drives the host's transmit line and is of no use to
 * a receive-only driver.
 *
 * This comment used to say the opposite: set TXD switch 5, leave every RXD
 * switch off. That guaranteed silence whatever else was tried, and it cost an
 * afternoon on the bench - every one of the sixteen signal GPIOs on the Tab5
 * M-Bus was swept and all were dead, because nothing was driving any of them.
 * The answer came from dmatking/m5stack-tab5-gps, which runs the same module
 * on the same board and wrote it down: "DIP switch 6 (not the TXD-labeled
 * group as the silkscreen suggests - verified empirically after the labeled
 * group produced nothing)".
 *
 * The earlier derivation also had an arithmetic error, recorded so it is not
 * repeated: it said "TXD switch 5 is Core G0, which is M-Bus position 21,
 * which on the Tab5 is GPIO2". Core G0 is position 24; position 21 is G12,
 * which is switch 4. The premise and the conclusion named different switches.
 *
 * The full position map, from M5Stack's published M-Bus tables for the Tab5
 * and the Basic (docs.m5stack.com /en/core/Tab5 and /en/core/Basic). The
 * module's switch rows carry the Core pin; the Core pin gives the bus
 * position; the position gives the Tab5 pin:
 *
 *   sw | group | Core pin | M-Bus pos | Tab5 pin
 *   ---+-------+----------+-----------+---------------------------
 *    1 |  TXD  | G1  TXD0 |    14     | G37  ⛔ the Tab5's OWN console TX
 *    2 |  TXD  | G17 TXD2 |    16     | G6
 *    3 |  TXD  | G15      |    23     | G47
 *    4 |  TXD  | G12      |    21     | G2
 *    5 |  TXD  | G0       |    24     | G35
 *    6 |  RXD  | G3  RXD0 |    13     | G38  <- this one, the module's output
 *    7 |  RXD  | G16 RXD2 |    15     | G7
 *    8 |  RXD  | G13      |    22     | G48
 *    9 |  RXD  | G34      |    26     | G51
 *   10 |  RXD  | G35      |     2     | G16
 *
 * ⛔ NEVER LISTEN ON G37. It is the Tab5's own UART0 TX, so the console
 * output comes back as a few hundred bytes a second WITH NEWLINES IN IT, and
 * during the bring-up it read exactly like a receiver had been found. The
 * byte sample gives it away: 5B 52 42 ... "[RB" is a misframed ANSI colour
 * escape from our own log, not NMEA.
 *
 * The baud is 115200 8N1, which is the module's documented default - the same
 * rate the Unit GPS v1.1 uses, so no change was needed there.
 *
 * The other RXD switches should stay OFF, and all five TXD switches too: this
 * driver is receive-only and passes UART_PIN_NO_CHANGE for TX, so the Tab5
 * never talks to the receiver.
 */
#define UNIT_GPS_PORTA_RX_GPIO   54
#define UNIT_GPS_MBUS_RX_GPIO    38

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
