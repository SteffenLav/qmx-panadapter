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

/* ⚠ BYTES, NOT CHARACTERS. The name is UTF-8 now (see to_latin1_utf8 in the
 * .c), so every æ, ø, å or umlaut costs two. 64 keeps the same ~48 visible
 * characters a Nordic name actually needs. */
#define RB_NAME_LEN   64
#define RB_URL_LEN    192
/* 150 on the operator's ask (2026-11-01, raised from 50 the same day). The
 * pane scrolls and now has a search field, so the only real limits are the
 * directory's supply and where the array lives - at 260 B a row, 150 of them
 * is 39 KB, which is why webradio_modal.c keeps it in PSRAM and not in .bss
 * (#65: this board runs ~30 KB of internal free).
 *
 * ⚠ MEASURED SUPPLY, per radius, from his grid: 150 km gives 44 unique MP3
 * stations, 300 km gives 147, 450 km gives 189. 150 is NOT reachable without
 * widening - see WR_RADIUS_KM in webradio_modal.c. */
#define RB_MAX_STATIONS 150

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

// Sanitise free text from the internet into something font_ui_latin_* can
// actually draw: UTF-8 restricted to ASCII plus Latin-1 Supplement, anything
// else as '?'. Accepts UTF-8 OR raw ISO-8859-1 (the ICY spec never settled
// one, and European Icecast servers send both). Never cuts a character in
// half, so a truncated string is still valid UTF-8 - a stray continuation
// byte draws as a square, which is the fault this exists to remove.
//
// Shared because the station name and the ICY programme title need exactly
// the same treatment and got different ones for a day: names were folded to
// ASCII, titles were not touched at all and showed squares on the glass.
void rb_text_to_latin1(const char *in, char *out, int cap);

#ifdef __cplusplus
}
#endif
