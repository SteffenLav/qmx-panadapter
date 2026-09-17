#include "country.h"
#include "dxcc.h"
#include "geo_coords.h"
#include "maidenhead.h"

#include <string.h>

const char *country_display(const char *call, int max_chars)
{
    if (!call || !call[0] || max_chars <= 0) return NULL;

    const char *full = dxcc_lookup(call);
    if (full) {
        if ((int)strlen(full) <= max_chars) return full;
        const char *a3 = dxcc_lookup_alpha3(call);
        if (a3) return a3;
        /* An entity with a name too long and no code should be impossible -
         * dxcc.c's two tables are verified in step - but returning a clipped
         * name would be the one thing this function exists to prevent. */
        return NULL;
    }

    /* Not in the DXCC table at all. Uwe's table is country-granular, so this
     * is a coarser answer than the branch above - but it is a TRUE one, and
     * it covers Monaco, Malta, Andorra, Nepal, Aruba and about 130 more that
     * would otherwise print nothing. */
    return geo_coords_iso_for_call(call);
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
