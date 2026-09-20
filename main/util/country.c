#include "country.h"
#include "country_shorten.h"
#include "dxcc.h"
#include "geo_coords.h"
#include "maidenhead.h"

#include <string.h>

const char *country_display(const char *call, int max_chars)
{
    if (!call || !call[0] || max_chars <= 0) return NULL;

    /* ⛔ NO MORE 3-LETTER CODES. This returned dxcc_lookup_alpha3() whenever the
     * name did not fit, on the reasoning that "NLD" is the shorter TRUE answer
     * while "Netherlan" reads as a bug. Both halves of that are right, and the
     * conclusion was still wrong: a column reading "Sweden / Ireland / NLD /
     * Italy" makes the reader switch alphabets mid-column. Operator,
     * 2026-09-19, looking at exactly that: "I hate to see those 3 char
     * countries in the list - for me its apples and pears - please cut them off
     * intelligently". country_shorten() is that "intelligently", and it never
     * produces a bare stump - see its own file.
     *
     * ⚠ The result is a STATIC buffer, so it is valid until the next call. Every
     * caller today prints it immediately, which is why this is acceptable and
     * also why it is written down. */
    static char buf[64];

    const char *full = dxcc_lookup(call);
    if (!full) {
        /* Not in the DXCC table. Uwe DL8UG's table now carries names as well as
         * codes (2026-09-18), so this fallback finally answers with a NAME - it
         * used to be the one path that could only ever return a code. It is
         * DXCC-entity granular there too, covering ~130 entities dxcc.c never
         * knew. */
        full = geo_coords_name_for_call(call);
    }
    if (!full) return NULL;

    if ((int)strlen(full) <= max_chars) return full;
    country_shorten(full, max_chars, buf, sizeof buf);
    return buf[0] ? buf : NULL;
}

bool country_centroid_km(const char *call, double my_lat, double my_lon,
                         double *km_out)
{
    if (!call || !call[0] || !km_out) return false;
    float lat = 0.0f, lon = 0.0f;
    if (!geo_coords_for_call(call, &lat, &lon)) return false;
    *km_out = haversine_km(my_lat, my_lon, (double)lat, (double)lon);
    return true;
}
