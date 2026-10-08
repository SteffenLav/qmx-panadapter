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
    bool     has_pos;   // latitude/longitude fields were present and in range
    double   lat_deg;   // signed degrees, + north (RMC fields 3/4)
    double   lon_deg;   // signed degrees, + east  (RMC fields 5/6)
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

/* ---- GGA and GSV: what a STATUS DISPLAY needs, and nothing the clock uses --
 *
 * The clock path reads RMC only, and deliberately: its status byte is the lock
 * bit and its date is the full civil date. These two add the numbers an
 * operator needs when a receiver is NOT locking - how many satellites it can
 * see, how strong they are, and what kind of fix it thinks it has. Added
 * 2026-10-08, when an afternoon was spent on a receiver that turned out to be
 * wired to nothing, with no instrument to say so.
 *
 * ⛔ Nothing here feeds time_sync. A GGA carries a time of day with NO DATE,
 * and letting it near the clock is how a receiver silently sets the wrong day.
 */

typedef struct {
    int   hour, min, sec;   // time of day only - GGA carries NO date
    int   quality;          // 0 invalid, 1 GPS, 2 DGPS, 4 RTK fix, 5 RTK float, 6 dead reckoning
    int   sats_used;        // satellites in the position solution
    float hdop;             // horizontal dilution of precision; <0 when absent
    float alt_m;            // antenna altitude above mean sea level
    bool  has_alt;
} nmea_gga_t;

// Parse one $xxGGA. Same contract as nmea_parse_rmc(): a line that is not a
// well-formed GGA with a good checksum returns false and leaves *out alone.
// quality 0 (no fix) is a SUCCESSFUL parse - the receiver is reporting that it
// has nothing, which is exactly what a status window must be able to show.
bool nmea_parse_gga(const char *line, nmea_gga_t *out);

typedef struct {
    int   fix_type;   /* 1 = no fix, 2 = 2D, 3 = 3D - the radio's "Fix" row */
    int   n_used;     /* PRNs listed as used in the solution (0..12) */
    int   used[12];   /* those PRNs. The sky plot marks them differently from
                       * the merely-in-view ones, so the list has to be kept,
                       * not just counted. */
    float pdop;       /* <0 when absent */
    float hdop;
    float vdop;
} nmea_gsa_t;

/* Parse one $xxGSA. This is the ONLY source of the 2D/3D distinction: GGA's
 * quality field says fix or no fix and nothing about dimensionality, and the
 * QMX's own GPS viewer prints "3D" on that row. */
bool nmea_parse_gsa(const char *line, nmea_gsa_t *out);

#define NMEA_GSV_SATS_PER_MSG 4

typedef struct {
    char talker[3];   // "GP" GPS, "GL" GLONASS, "GA" Galileo, "GB"/"BD" BeiDou, "GQ" QZSS
    int  msg_num;     // 1-based index of this sentence
    int  msg_total;   // sentences in this constellation's burst
    int  in_view;     // satellites in view FOR THIS TALKER - not a running total
    int  n_sats;      // how many of the four slots below this sentence filled
    struct {
        int  prn;
        int  elev_deg;   // -1 when the field was blank
        int  azim_deg;   // -1 when the field was blank
        int  snr_db;     // 0..99; -1 when blank, which means "not tracked"
    } sat[NMEA_GSV_SATS_PER_MSG];
} nmea_gsv_t;

/* Parse one $xxGSV.
 *
 * ⚠ in_view is PER TALKER. A receiver doing GPS + GLONASS + Galileo sends
 * three separate bursts, each with its own in_view, and adding the latest of
 * each is the only way to a total. Summing every GSV as it arrives counts the
 * same constellation several times over - once per sentence in its burst. */
bool nmea_parse_gsv(const char *line, nmea_gsv_t *out);
