// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Live self-spotting via PSK Reporter's public MQTT broker
// (mqtt.pskreporter.info), filtered SERVER-SIDE on tx_call = our own
// callsign - so only the handful of reception reports of US ever reach this
// device, not PSK Reporter's whole firehose (~450x CW's volume on that
// network). Real-time (seconds), unlike net/psk_rx.c's periodic HTTP/XML
// query, which is rate-limited by PSK Reporter itself to once per 5 minutes
// and exists for a different purpose (the web UI's "Who is hearing me"
// on-demand report) - that module is untouched by this one.
//
// Ported from the sibling rbn_monitor project's pskr_self_client.h/.cpp (same
// board family, same problem, same broker). Topic shape, per that project's
// own comment on the general (non-self) client:
//   pskr/filter/v2/{band}/{mode}/{tx_call}/{rx_call}/{tx_grid}/{rx_grid}/{tx_dxcc}/{rx_dxcc}
// Self-spotting fixes tx_call to our own callsign and wildcards everything
// else - band and mode included, since a CW/RTTY-only skimmer network
// equivalent does not exist for Digi modes and we want all of them.
//
// See net/rbn.c's self-spot capture for the CW equivalent (RBN has no
// server-side filtering at all, so that one taps the existing full feed
// instead of opening a second connection).

// Call once at boot. Connects once WiFi is up; re-subscribes automatically
// if the configured callsign (storage/settings.h's my_callsign) changes
// without a reboot.
void pskr_self_init(void);

typedef struct {
    char     call[16];    // the station that heard us
    char     mode[8];     // "FT8"/"FT4"/"JS8"/... as PSK Reporter reports it
    uint32_t freq_hz;
    int      snr_db;
    int64_t  heard_unix;
    char     grid[7];     // the reporter's OWN grid as they sent it, "" if absent
    float    lat, lon;    // resolved from the receiver's own reported grid
    bool     has_pos;
} pskr_self_spot_t;

// Copies up to max entries, in no particular order. Returns the count copied.
int pskr_self_spots_get(pskr_self_spot_t *out, int max);

// For a status line: true once the MQTT connection is up.
bool pskr_self_is_connected(void);

// Manual Flush - empties the ring buffer immediately (ui/spot_map_view.c's
// sidebar button). New self-spots keep arriving afterward as normal.
void pskr_self_clear(void);
