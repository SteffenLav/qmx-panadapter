#pragma once

#include <stdbool.h>

// Looks up real station coordinates for RBN/DX-cluster spotted callsigns via
// QRZ.com's XML Callbook service (session-key auth from username/password --
// QRZ has no static API key for this lookup service, only for the unrelated
// Logbook/QSO-sync API used by adif/qrz_upload.c). RBN and the DX cluster
// carry no locator at all in their spots, so net/spots.c otherwise falls back
// to util/geo_coords.h's country-centroid guess; this, once a callsign's
// lookup has completed, supplies a real per-station position instead.
//
// Ported from the sibling rbn_monitor project's qrz_client.h/.cpp (same
// board family, same problem). Credentials come from the settings drawer
// (storage/settings.h's qrz_lookup_user/qrz_lookup_pass, NOT qrz_api_key) --
// the background task polls them itself, so nothing needs to notify this
// module when they change.
//
// All lookups happen in a background task -- qrz_coords_lookup_cached() is a
// non-blocking cache check the spot producers can call straight from their
// own context. A cache miss enqueues a background lookup for next time
// rather than blocking on the network round-trip. The cache is a plain
// session cache in PSRAM: it does not survive a reboot (by design -- see the
// spot-map plan), and an empty/unconfigured username means every lookup
// simply misses forever, so callers should always keep using
// geo_coords_for_call()'s fallback regardless.

// Call once at boot, before any producer uses qrz_coords_lookup_cached().
void qrz_coords_start(void);

// Non-blocking. Returns true and fills *lat_out/*lon_out if this callsign's
// coordinates are already cached from a prior successful QRZ query. Returns
// false on any miss (not yet looked up, still pending, or QRZ has no
// coordinates for it) -- callers should fall back to
// geo_coords_for_call() in that case. A miss also enqueues a background
// lookup for next time (deduped against what's already pending; silently
// dropped if the queue is full or QRZ isn't configured).
bool qrz_coords_lookup_cached(const char *call, float *lat_out, float *lon_out);

// For a Settings status line: true once a login with the current credentials
// has succeeded at least once.
bool qrz_coords_is_logged_in(void);

// For a Settings status line: true once QRZ has explicitly rejected the
// current credentials (wrong username/password, or an active temporary
// block) -- the client stops retrying entirely until the stored credentials
// change.
bool qrz_coords_credentials_rejected(void);
