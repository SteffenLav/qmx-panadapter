#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Propagation feedback: WHO IS HEARING ME.
//
// We already SEND reception reports to PSK Reporter (net/pskreporter.c). This
// is the other direction - asking the collector which receivers have copied
// OUR callsign lately. It answers the question no amount of local DSP can:
// "am I actually getting out, and where to?"
//
// The reciprocal case is the one worth having. Stations you can hear that
// cannot hear you is a transmit-side problem - antenna, power, a bad
// connector - and it looks identical to a dead band from the receive side
// alone. Nothing else on this device can tell those two apart.
//
// Read-only, opt-in, and independent of the sending side: turning report
// SENDING off does not turn this off, and vice versa. Inert until a callsign
// is set, since the query is by callsign.

typedef struct {
    char     rx_call[12];   // the receiver that heard us
    char     rx_grid[8];    // their Maidenhead grid ("" if not supplied)
    char     rx_dxcc[24];   // their country name as PSK Reporter gives it
    char     mode[8];       // mode they decoded us on
    uint32_t freq_hz;
    int16_t  snr_db;        // signal-to-noise THEY measured on us
    int64_t  heard_unix;
    int32_t  distance_km;   // -1 when either grid is missing
    int16_t  bearing_deg;   // -1 when either grid is missing
} psk_rx_report_t;

// How far back the panel looks: a day, because a POTA operator wants the
// morning, not the minute. In the header rather than the .c so the web UI can
// state the window instead of leaving the reader to guess it.
//
// ⛔ THE COLLECTOR DOES NOT SERVE A DAY. Measured 2026-11-26: a query with
// flowStartSeconds=-86400 returns about the last HOUR (KB1EFS, two queries
// 6 min apart: 1021 and 1055 reports, both spanning ~3500 s, the oldest moving
// forward with the clock - a time cap, not a row cap). N4OPI, last on the air
// 04:29 UTC, got 0 reports at 06:46 and at 16:05 while pskreporter.info's map
// showed 396. pskdev.html says "no more than 6 hours in any event"; the
// measured cap is shorter. So the day is built HERE, by keeping what each
// query returns - see PSK_RX_API_WINDOW_S and psk_rx_covered_from().
#define PSK_RX_WINDOW_S 86400

// What one query actually returns, as measured above. A gap between two good
// queries longer than this is a hole in what the device holds.
#define PSK_RX_API_WINDOW_S 3600

// The store keeps the NEWEST report per receiver, band and mode - "who has
// heard me", not a logbook. N4OPI had 396 reports in a day; a busy FT8 station
// had 1055 in one hour. 384 entries cost ~30 KB of PSRAM; psk_rx_is_truncated()
// says when the oldest had to make room.
#define PSK_RX_MAX 384

void psk_rx_init(void);

// Copy up to max reports, newest first. Returns the count written.
int  psk_rx_get(psk_rx_report_t *out, int max);

// Number of reports held, and how many DISTINCT receivers they came from.
// The distinct count is the honest headline: ten reports from one receiver
// says far less about propagation than one report each from ten.
int  psk_rx_count(void);
int  psk_rx_unique_receivers(void);

// Seconds since the last successful query, or -1 if none has succeeded.
int  psk_rx_age_s(void);

// Unix time from which the store is complete: reports heard since then are all
// here. 0 = no successful query yet (or no wall clock). Later than now - 24 h
// until the device has queried without a gap for a day - the panel must say
// so, because the collector itself serves only the last hour.
int64_t psk_rx_covered_from(void);

// Why the last pass did NOT query, or NULL when nothing held it. Shown by the
// web modal so "no query ran" never reads as "nobody heard you" (N4OPI).
const char *psk_rx_hold_reason(void);
// HTTP status of the last query when it failed (-1 = no answer), else 0.
int  psk_rx_last_fail_status(void);

// Furthest report in the current set, in km; -1 when no report carried a grid.
int  psk_rx_max_distance_km(void);

// True when the set is not the whole picture: the response buffer was cut, or
// the store dropped its oldest entries to make room.
// (History: first written for one answer that was CUT SHORT - the collector had
// more for the window than the response buffer or the report cap could hold.) The set on
// display is then the part that fit, not the whole picture, and the UI has to
// say so: a partial answer and a quiet band are otherwise indistinguishable,
// which is exactly how "no one has heard me in 24 hrs" was reported while
// pskreporter.info's own page showed dozens (Randy N4OPI).
bool psk_rx_is_truncated(void);

// Ask for a query now (e.g. the operator just opened the view). Coalesced with
// the periodic cycle and still rate-limited - see PSK_RX_MIN_INTERVAL_S.
void psk_rx_request_refresh(void);

// Parse one <receptionReport .../> element into a report. Exposed for the
// boot self-test: the field extraction is the part that silently rots when
// the collector changes its attribute set, and it is pure string work, so it
// can be checked against captured real responses with no network.
bool psk_rx_parse_report(const char *elem, psk_rx_report_t *out);

// Boot self-test over captured real responses. Returns true if all pass.
bool psk_rx_selftest(void);
