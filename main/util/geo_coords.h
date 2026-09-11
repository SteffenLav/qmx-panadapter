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
