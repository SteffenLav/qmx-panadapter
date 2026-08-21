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

// How far back each query looks: a day, because a POTA operator wants the
// morning, not the minute. In the header rather than the .c so the web UI can
// state the window instead of leaving the reader to guess it.
#define PSK_RX_WINDOW_S 86400

// A single query returns everything the collector has for the window, and a
// busy station on a good band can be heard by a lot of receivers. 64 was chosen
// to answer "where am I getting out" without pretending to be a logbook - but
// over a 24 h window an active operator passes it easily, and the overflow was
// discarded in silence. 128 costs ~10 KB of PSRAM and covers a real session;
// psk_rx_is_truncated() reports the case where even that is not enough.
#define PSK_RX_MAX 128

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

// Furthest report in the current set, in km; -1 when no report carried a grid.
int  psk_rx_max_distance_km(void);

// True when the last answer was CUT SHORT - the collector had more for the
// window than the response buffer or the report cap could hold. The set on
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
