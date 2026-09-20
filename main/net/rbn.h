#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Reverse Beacon Network as a second spot source, feeding the SAME store as
// POTA (see spots.h) so the spectrum lane never has to know where a spot came
// from.
//
// Why telnet and not HTTP: RBN has no public HTTP API. Its site does serve JSON
// at /spots.php, but the data call is gated on a per-deployment version hash and
// the client force-reloads on a mismatch - a private internal endpoint, brittle
// and impolite to point a fleet of radios at. The supported feed is the telnet
// stream, which is trivial to parse but means holding a persistent TCP socket
// open on this board's most fragile subsystem.
//
// That fragility is why the feature is OPT-IN and default OFF. RBN is a global
// firehose (tens of lines a second, continuously), and this board's esp_hosted
// WiFi link is the thing that every other network feature here has had to be
// taught to tiptoe around. Off by default means a user who never asks for it can
// never be hurt by it; the operator can switch it on and judge for themselves.

void rbn_init(void);

// Seconds since the last line arrived from the feed, or -1 if never connected.
// Distinguishes "connected and quiet" from "not connected".
int  rbn_age_s(void);

// Deduplicated spots currently held from the feed - what was last published.
int  rbn_spot_count(void);

// Verify the line parser against real feed lines. Logs PASS or the failures.
void rbn_selftest(void);

// Parse one feed line. True only for a usable spot; the login prompt, the
// banner, status chatter and truncated lines all return false. Exposed for the
// self-test - there is no other way to check a parser against a live socket.
bool rbn_parse_line(const char *line, char *call_out, size_t call_cap,
                    uint32_t *freq_hz_out, int *snr_out);

// Self-spotting (ui/spot_map_view.c): the SAME feed as above also spots our
// own CQ, exactly like any other station's, whenever a skimmer copies it -
// this is that half of it, kept separate from the CQ-activity table because
// it answers a different question ("who hears me", not "who is on the air").
// Position is resolved once at capture time via net/qrz_coords.h, falling
// back to util/geo_coords.h's country centroid.
typedef struct {
    char     skimmer[16];   // the station that heard us
    uint32_t freq_hz;
    int      snr_db;
    int64_t  heard_unix;
    float    lat, lon;
    bool     has_pos;
} rbn_self_spot_t;

// Copies up to max entries, in no particular order. Returns the count copied.
int rbn_self_spots_get(rbn_self_spot_t *out, int max);

// Manual Flush - empties the ring buffer immediately (ui/spot_map_view.c's
// sidebar button). New self-spots keep arriving afterward as normal.
void rbn_self_spots_clear(void);
