#pragma once

// Station directory for the web-radio player: radio-browser.info, searched by
// distance from the operator's Maidenhead grid.
//
// ⭐ WHY THIS DIRECTORY AND NOT ANOTHER. It is the only free one that needs no
// API key and supports a geographic search. TuneIn/RadioTime needs a partner
// ID and its terms do not cover an embedded player; Shoutcast needs a key;
// the Icecast directory has no geo search and thin broadcaster coverage.
//
// ⭐ DAB STATIONS ARE IN IT. The Tab5 has no DAB tuner and never will, but the
// DAB-only channels simulcast on the internet and the directory lists them -
// measured 2026-11-01 for DK: 65 DR-family entries in the top 200, including
// P6 Beat and P8 Jazz, which have no FM transmitter at all.
//
// ⛔ MP3 ONLY. The decoder is Helix, which is an MP3 decoder; the directory
// also carries AAC (DR P3 has an AAC entry at 324 kbps). An AAC row in the
// list would be a station that cannot play, so they are filtered out here
// rather than failing at the stream.

#include <stdbool.h>
#include <stdint.h>

#define RB_NAME_LEN   48
#define RB_URL_LEN    192
#define RB_MAX_STATIONS 24     /* one pane, no paging - see the .c */

typedef struct {
    char     name[RB_NAME_LEN];
    char     url[RB_URL_LEN];
    uint16_t bitrate_kbps;
    uint16_t distance_km;
} rb_station_t;

#ifdef __cplusplus
extern "C" {
#endif

// Fetch up to `max` MP3 stations within `radius_km` of the operator's grid,
// most-played first and de-duplicated by name. Returns the count written, or
// a negative esp_err_t-style code: -1 no grid set, -2 no network/HTTP error,
// -3 the reply did not parse.
//
// Blocking and slow (a TLS fetch plus a JSON parse of a few hundred KB) - call
// it from a worker task, never from an LVGL callback.
int radio_browser_fetch(rb_station_t *out, int max, int radius_km);

// The last failure, for the operator rather than the log. "" when none.
const char *radio_browser_last_error(void);

#ifdef __cplusplus
}
#endif
