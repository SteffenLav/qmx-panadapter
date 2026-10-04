#pragma once

/* NMEA 0183 RMC parsing for the Unit GPS (AT6668) - pure, dependency-free C.
 *
 * Split out of unit_gps.c so that test/unit_gps_nmea_harness.c can link it on
 * a host: the parse rules (checksum, status, field sanity, the 59->00 flip) are
 * the part of this feature that can be wrong WITHOUT the hardware noticing - a
 * checksum bug silently accepts garbage, a date bug silently accepts a wrong
 * DATE, and both only ever show up on a device that has a satellite fix.
 */

#include <stdbool.h>
#include <stdint.h>

// One accepted $xxRMC sentence, decoded. Fields are calendar/time of day as
// written on the wire: the date is the full civil date from ddmmyy, NOT a
// time-of-day waiting for a remembered anchor to supply the day.
typedef struct {
    int      year;      // 4-digit year (20xx from RMC's 2-digit yy)
    int      mon;       // 1..12
    int      mday;      // 1..31
    int      hour;      // 0..23
    int      min;       // 0..59
    int      sec;       // 0..59
    uint32_t frac_us;   // fractional seconds as microseconds (0 when the
                        // sentence carries none - many GPSes send hhmmss only)
    bool     valid;     // status field was 'A' (ACTIVE); false for 'V'
} nmea_rmc_t;

// Verify a whole line's "*HH" checksum. Returns false for a line with no
// checksum field, a non-hex one, or a mismatch - i.e. for anything that is not
// a complete NMEA sentence.
bool nmea_checksum_ok(const char *line);

// Parse exactly one line. Returns false (leaving *out untouched) unless the
// line is a well-formed $..RMC with a good checksum AND every field it carries
// is within its own range. A void fix ('V') parses as valid=false: the sentence
// is well-formed, the receiver simply has no lock yet.
//
// Anything else - other sentence types, a truncated line, a checksum failure,
// an impossible time or date - is a false, not a "partial success". The caller
// uses "did it parse at all" to decide the DEVICE/LISTENING distinction, so a
// sentence that parses with nonsense in it must not parse.
bool nmea_parse_rmc(const char *line, nmea_rmc_t *out);

// Second-boundary flip: true only for a forward step of exactly one second,
// 59 -> 00 included. A repeated second, a gap (packet loss at 1 Hz) or a jump
// backwards is not an edge - stamping a flip on those would phase-align the
// clock to a moment that never happened.
bool nmea_second_flipped(int prev_sec, int cur_sec);
