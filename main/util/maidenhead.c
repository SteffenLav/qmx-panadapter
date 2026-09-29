// Maidenhead grid math: grid -> lat/lon, great-circle distance and bearing.
//
// References:
//   - Maidenhead Locator System: https://en.wikipedia.org/wiki/Maidenhead_Locator_System
//   - Haversine formula: https://en.wikipedia.org/wiki/Haversine_formula

#include "maidenhead.h"
#include <math.h>
#include <stddef.h>
#include <ctype.h>

#define DEG2RAD(d) ((d) * 3.14159265358979323846 / 180.0)
#define RAD2DEG(r) ((r) * 180.0 / 3.14159265358979323846)
#define EARTH_RADIUS_KM 6371.0

bool maidenhead_to_latlon(const char *grid, double *lat_out, double *lon_out)
{
    if (!grid || !lat_out || !lon_out) return false;
    size_t len = 0;
    while (grid[len]) len++;
    // 8-char "extended precision" grids (e.g. PSK Reporter/WSPR reporters
    // sometimes publish "IO51uu43") used to be rejected outright here -
    // field-reported 2026-09-11 (EI4HQ's own report carried exactly this
    // shape, net/pskr_self.c's diagnostic pinned the value). Accepted and
    // resolved to a finer centre point below rather than truncated to 6
    // chars and thrown away, since the extra pair is cheap to use correctly.
    if (len != 4 && len != 6 && len != 8) return false;

    int A = toupper((unsigned char)grid[0]) - 'A';   // 0..17 (lon field, 20 deg wide)
    int B = toupper((unsigned char)grid[1]) - 'A';   // 0..17 (lat field, 10 deg tall)
    int C = grid[2] - '0';                           // 0..9  (lon square, 2 deg wide)
    int D = grid[3] - '0';                           // 0..9  (lat square, 1 deg tall)
    if (A < 0 || A > 17 || B < 0 || B > 17) return false;
    if (C < 0 || C > 9  || D < 0 || D > 9) return false;

    double lon = -180.0 + A * 20.0 + C * 2.0;
    double lat =  -90.0 + B * 10.0 + D * 1.0;
    double cell_lon = 2.0, cell_lat = 1.0;   // size of the finest field resolved so far

    if (len >= 6) {
        int E = toupper((unsigned char)grid[4]) - 'A';   // 0..23 (lon subsq, 5 min = 0.0833 deg)
        int F = toupper((unsigned char)grid[5]) - 'A';   // 0..23 (lat subsq, 2.5 min = 0.0417 deg)
        if (E < 0 || E > 23 || F < 0 || F > 23) return false;
        cell_lon /= 24.0;
        cell_lat /= 24.0;
        lon += E * cell_lon;
        lat += F * cell_lat;

        if (len == 8) {
            int G = grid[6] - '0';   // 0..9 (further lon precision, 1/10 subsquare)
            int H = grid[7] - '0';   // 0..9 (further lat precision, 1/10 subsquare)
            if (G < 0 || G > 9 || H < 0 || H > 9) return false;
            cell_lon /= 10.0;
            cell_lat /= 10.0;
            lon += G * cell_lon;
            lat += H * cell_lat;
        }
    }
    // Centre of whichever cell size was actually resolved (2x1 deg for a
    // 4-char grid, subsquare for 6, extended sub-subsquare for 8).
    lon += cell_lon / 2.0;
    lat += cell_lat / 2.0;

    *lon_out = lon;
    *lat_out = lat;
    return true;
}

double haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = DEG2RAD(lat2 - lat1);
    double dlon = DEG2RAD(lon2 - lon1);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0)
             + cos(DEG2RAD(lat1)) * cos(DEG2RAD(lat2))
               * sin(dlon / 2.0) * sin(dlon / 2.0);
    double cc = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return EARTH_RADIUS_KM * cc;
}

double bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double phi1 = DEG2RAD(lat1);
    double phi2 = DEG2RAD(lat2);
    double dlon = DEG2RAD(lon2 - lon1);
    double y = sin(dlon) * cos(phi2);
    double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlon);
    double brg = RAD2DEG(atan2(y, x));
    if (brg < 0.0) brg += 360.0;
    return brg;
}
