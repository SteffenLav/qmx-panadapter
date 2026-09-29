// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Who is hearing OUR OWN WSPR transmissions - see ui/spot_map_view.c's MAP
// tab (third self-spotting source alongside net/rbn.c's CW and
// net/pskr_self.c's Digi). Unlike those two, WSPR has no live push feed to
// subscribe to (no RBN-style socket, no MQTT broker) - wsprnet.org's own
// upload path is one-way (net/wsprnet.c only ever POSTs), so the only way to
// find out who heard us is a periodic query against wsprnet.org's public
// "olddb" lookup, filtered server-side on our own callsign.

#define WSPR_SELF_MAX 100   // ring buffer, same cap/shape as RBN_SELF_MAX / PSKR_SELF_MAX

typedef struct {
    char     call[16];    // the station that heard us (wsprnet's "Reporter")
    uint32_t freq_hz;
    int      snr_db;
    int64_t  heard_unix;
    char     grid[7];     // the reporter's OWN grid as they sent it, "" if absent
    float    lat, lon;    // resolved from the reporter's own reported grid
    bool     has_pos;
} wspr_self_spot_t;

// Starts the background poll task. Queries wsprnet.org on a fixed interval
// regardless of whether WSPR TX is currently on - a wsprnet.org report from
// an earlier transmission is still worth showing, and the query itself is
// cheap. See wspr_self.c's wspr_self_task() for the exact gating (WiFi +
// net_quiet only).
void wspr_self_init(void);

// Copies up to max entries, in no particular order. Returns the count copied.
int wspr_self_spots_get(wspr_self_spot_t *out, int max);

// Manual Flush (ui/spot_map_view.c's sidebar button) - empties the ring
// buffer immediately. The next scheduled poll refills it as normal.
void wspr_self_spots_clear(void);
