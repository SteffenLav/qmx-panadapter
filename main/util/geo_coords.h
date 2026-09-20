// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
#pragma once

#include <stdbool.h>

// Approximate country/DXCC-entity centroid for a callsign, by prefix
// (longest-prefix-match). Ported from the sibling rbn_monitor project's
// geo_coords.h/.cpp (same table, same method) -- see that project for the
// original C++ source. NOT station-accurate: a whole country collapses to
// one point. Used as the last-resort fallback for a spot with no other way
// to place it on the map (see net/qrz_coords.h for the real-position path
// this backstops).
//
// Returns false (leaves *lat_out/*lon_out untouched) if no prefix matched.
// On success, *lat_out/*lon_out are in degrees (-90..90 / -180..180).
bool geo_coords_for_call(const char *call, float *lat_out, float *lon_out);

// Same longest-prefix-match lookup as geo_coords_for_call(), but returns the
// ISO 3166-1 alpha-3 country code for the matched DXCC entity instead of a
// centroid - independent of how (or whether) the caller resolved a real
// position, since the ISO code is a property of the callsign's nationality
// prefix alone. Returns NULL if no prefix matched, OR if the matched entity
// has no reasonable ISO mapping (a disputed territory, a diplomatic special
// entity - see tools/dxcc_iso3.py). The returned pointer is a string literal
// with static storage duration - never free it.
const char *geo_coords_iso_for_call(const char *call);

/* The entity NAME for a callsign's prefix - "Canary Is.", "European Russia".
 * Added with Uwe DL8UG's 2026-09-18 table, which carries names as well as
 * codes; before it, a caller wanting a name had to go to dxcc.c and this table
 * could only ever back it up with a 3-letter code. NULL if unknown. */
const char *geo_coords_name_for_call(const char *call);
