#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Convert a Maidenhead grid square to lat/lon in degrees. Accepts
// 4-char ("JO45") or 6-char ("JO45ab") grids; case-insensitive on
// subsquare letters. Returns false on malformed input. lat/lon are
// the *centre* of the indicated grid cell.
bool maidenhead_to_latlon(const char *grid, double *lat_out, double *lon_out);

// Great-circle distance in km between two lat/lon points (haversine).
double haversine_km(double lat1, double lon1, double lat2, double lon2);

// Initial bearing in degrees (0..360, 0 = north, 90 = east) from
// point 1 toward point 2 along the great circle.
double bearing_deg(double lat1, double lon1, double lat2, double lon2);

#ifdef __cplusplus
}
#endif
