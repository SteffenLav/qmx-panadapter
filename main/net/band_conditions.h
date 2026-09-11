// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// HF band (day/night) propagation conditions from N0NBH's public hamqsl.com
// feed - no auth, plain HTTPS GET: https://www.hamqsl.com/solarxml.php
// A well-established, widely-used (by other ham radio software) free XML
// feed of NOAA solar/geomagnetic data plus N0NBH's own derived "Good"/
// "Fair"/"Poor" band-condition estimates. Ported from the sibling
// rbn_monitor project's band_conditions_client (see ui/spot_map_view.c's
// CONDITIONS tab).

#define BAND_COND_GROUP_COUNT 4

// The 4 band groupings hamqsl.com reports conditions for (its own
// grouping, not this project's per-band table) - index order matches
// band_conditions_t::day/night below.
extern const char *const BAND_COND_GROUP_NAMES[BAND_COND_GROUP_COUNT];

#define BAND_COND_RATING_LEN 8
#define BAND_COND_GEOMAG_LEN 16
#define BAND_COND_NOISE_LEN  16

typedef struct {
    int solar_flux;
    int a_index;
    int k_index;
    int sunspots;
    char geomag_field[BAND_COND_GEOMAG_LEN]; // "QUIET"/"UNSETTLED"/"ACTIVE"/"MINOR STORM"/...
    char signal_noise[BAND_COND_NOISE_LEN];  // e.g. "S1-S2"
    char day[BAND_COND_GROUP_COUNT][BAND_COND_RATING_LEN];   // "Good"/"Fair"/"Poor"
    char night[BAND_COND_GROUP_COUNT][BAND_COND_RATING_LEN];
    int64_t fetched_ms;
} band_conditions_t;

// Call once at boot - no configuration needed. Polls every 60 minutes
// (hamqsl.com's own data updates roughly hourly); retries every 1 minute
// until the first fetch succeeds.
void band_conditions_start(void);

// Thread-safe snapshot copy of the last successful fetch. Returns false
// (leaves *out untouched) if nothing has been fetched yet.
bool band_conditions_get(band_conditions_t *out);
