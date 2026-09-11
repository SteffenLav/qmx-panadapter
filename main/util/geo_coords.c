// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
#include "geo_coords.h"

#include <string.h>
#include <stddef.h>

typedef struct {
    const char *prefix;
    float lat;
    float lon;
} prefix_coord_t;

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Approximate country/DXCC-entity centroids, keyed by callsign prefix.
// Deliberately coarse (whole country -> one point) -- see geo_coords.h.
// Matched by longest-prefix-wins, same as the rbn_monitor project's
// geo_filter.cpp callsign_continent(), so a more specific entry (e.g. "EA8"
// for the Canary Islands) correctly overrides a broader one ("EA" for Spain).
static const prefix_coord_t PREFIX_COORDS[] = {
    // -- Europe --
    {"DA", 51.0f, 10.0f}, {"DB", 51.0f, 10.0f}, {"DC", 51.0f, 10.0f}, {"DD", 51.0f, 10.0f},
    {"DF", 51.0f, 10.0f}, {"DG", 51.0f, 10.0f}, {"DH", 51.0f, 10.0f}, {"DJ", 51.0f, 10.0f},
    {"DK", 51.0f, 10.0f}, {"DL", 51.0f, 10.0f}, {"DM", 51.0f, 10.0f}, {"DO", 51.0f, 10.0f},
    {"DP", 51.0f, 10.0f}, {"DQ", 51.0f, 10.0f}, {"DR", 51.0f, 10.0f}, // Germany
    {"G", 54.0f, -2.0f}, {"M", 54.0f, -2.0f}, {"2E", 54.0f, -2.0f},   // UK
    {"EI", 53.3f, -8.0f}, {"EJ", 53.3f, -8.0f},                       // Ireland
    {"F", 46.5f, 2.5f}, {"TM", 46.5f, 2.5f},                          // France
    {"FO", -17.7f, -149.4f}, {"FK", -21.3f, 165.5f}, {"FP", 46.8f, -56.2f}, // French overseas
    {"FS", 18.1f, -63.1f}, {"FW", -13.3f, -176.2f}, {"FY", 4.0f, -53.0f},
    {"FR", -21.1f, 55.5f}, {"FM", 14.6f, -61.0f}, {"FG", 16.2f, -61.6f},
    {"I", 42.5f, 12.5f}, {"IK", 42.5f, 12.5f},                        // Italy
    {"IH9", 36.8f, 12.0f}, {"IG9", 35.5f, 12.6f},                     // Pantelleria/Lampedusa -> Africa-ish
    {"EA", 40.0f, -4.0f},                                             // Spain
    {"EA8", 28.3f, -16.6f}, {"EA9", 35.9f, -5.3f},                    // Canary Islands, Ceuta/Melilla
    {"EG8", 28.3f, -16.6f}, {"EG9", 35.9f, -5.3f},
    {"CT", 39.5f, -8.0f}, {"CQ", 39.5f, -8.0f}, {"CR", 39.5f, -8.0f}, {"CS", 39.5f, -8.0f},
    {"CU", 37.7f, -25.7f},                                            // Portugal (CU = Azores)
    {"CT3", 32.7f, -17.0f}, {"CS3", 32.7f, -17.0f}, {"CT9", 32.7f, -17.0f},
    {"CR3", 32.7f, -17.0f}, {"CQ9", 32.7f, -17.0f},                   // Madeira
    {"HB", 46.8f, 8.2f}, {"HE", 46.8f, 8.2f}, {"HB0", 47.1f, 9.5f},   // Switzerland/Liechtenstein
    {"OE", 47.5f, 14.5f},                                             // Austria
    {"PA", 52.2f, 5.5f},                                              // Netherlands
    {"ON", 50.6f, 4.5f},                                              // Belgium
    {"LX", 49.8f, 6.1f},                                              // Luxembourg
    {"OZ", 56.0f, 10.0f}, {"5Q", 56.0f, 10.0f}, {"OU", 56.0f, 10.0f},
    {"OV", 56.0f, 10.0f}, {"OW", 56.0f, 10.0f}, {"OY", 62.0f, -7.0f}, // Denmark (OY = Faroe Islands)
    {"LA", 61.0f, 9.0f},                                              // Norway
    {"SM", 62.0f, 15.0f}, {"7S", 62.0f, 15.0f},                       // Sweden
    {"OH", 64.0f, 26.0f}, {"OI", 64.0f, 26.0f}, {"OJ", 64.0f, 26.0f},
    {"OF", 64.0f, 26.0f}, {"OG", 64.0f, 26.0f},                       // Finland
    {"SP", 52.0f, 19.0f}, {"3Z", 52.0f, 19.0f}, {"HF", 52.0f, 19.0f}, // Poland
    {"OK", 49.8f, 15.5f}, {"OL", 49.8f, 15.5f},                       // Czechia
    {"OM", 48.7f, 19.5f},                                             // Slovakia
    {"HA", 47.0f, 19.5f}, {"HG", 47.0f, 19.5f},                       // Hungary
    {"YO", 46.0f, 25.0f}, {"YP", 46.0f, 25.0f}, {"YQ", 46.0f, 25.0f}, {"YR", 46.0f, 25.0f}, // Romania
    {"LZ", 42.7f, 25.5f},                                             // Bulgaria
    {"YU", 44.0f, 21.0f}, {"YT", 44.0f, 21.0f},                       // Serbia
    {"9A", 45.5f, 16.0f},                                             // Croatia
    {"S5", 46.0f, 15.0f},                                             // Slovenia
    {"E7", 44.0f, 18.0f},                                             // Bosnia
    {"Z3", 41.6f, 21.7f},                                             // North Macedonia
    {"4O", 42.7f, 19.3f},                                             // Montenegro
    {"ZA", 41.0f, 20.0f},                                             // Albania
    {"Z6", 42.6f, 20.9f},                                             // Kosovo
    {"SV", 39.0f, 22.0f}, {"SW", 39.0f, 22.0f}, {"SX", 39.0f, 22.0f},
    {"SY", 39.0f, 22.0f}, {"SZ", 39.0f, 22.0f}, {"J4", 39.0f, 22.0f}, // Greece
    {"5B", 35.1f, 33.4f}, {"C4", 35.1f, 33.4f}, {"P3", 35.1f, 33.4f}, {"H2", 35.1f, 33.4f}, // Cyprus
    {"LY", 55.3f, 24.0f},                                             // Lithuania
    {"YL", 56.9f, 24.6f},                                             // Latvia
    {"ES", 58.6f, 25.0f},                                             // Estonia
    {"EU", 53.5f, 28.0f}, {"EV", 53.5f, 28.0f}, {"EW", 53.5f, 28.0f}, // Belarus
    {"UR", 49.0f, 32.0f}, {"UT", 49.0f, 32.0f}, {"UX", 49.0f, 32.0f}, {"UY", 49.0f, 32.0f},
    {"UZ", 49.0f, 32.0f}, {"EM", 49.0f, 32.0f}, {"EN", 49.0f, 32.0f}, {"EO", 49.0f, 32.0f}, // Ukraine
    {"ER", 47.0f, 28.9f},                                             // Moldova
    {"R", 60.0f, 60.0f}, {"UA", 60.0f, 60.0f}, {"UB", 60.0f, 60.0f}, {"UC", 60.0f, 60.0f},
    {"UD", 60.0f, 60.0f}, {"UE", 60.0f, 60.0f}, {"UF", 60.0f, 60.0f}, {"UG", 60.0f, 60.0f},
    {"UH", 60.0f, 60.0f}, {"UI", 60.0f, 60.0f},                       // Russia (rough, huge country)
    {"9H", 35.9f, 14.5f},                                             // Malta
    {"T7", 43.9f, 12.4f},                                             // San Marino
    {"HV", 41.9f, 12.45f},                                            // Vatican
    {"3A", 43.7f, 7.4f},                                              // Monaco
    {"C3", 42.5f, 1.5f},                                              // Andorra
    {"ZB", 36.1f, -5.3f},                                             // Gibraltar
    {"TF", 65.0f, -18.0f},                                            // Iceland
    {"TA", 39.0f, 35.0f}, {"TC", 39.0f, 35.0f}, {"YM", 39.0f, 35.0f}, // Turkey

    // -- North America --
    {"W", 39.8f, -98.6f}, {"K", 39.8f, -98.6f}, {"N", 39.8f, -98.6f}, {"AA", 39.8f, -98.6f},
    {"AB", 39.8f, -98.6f}, {"AC", 39.8f, -98.6f}, {"AD", 39.8f, -98.6f}, {"AE", 39.8f, -98.6f},
    {"AF", 39.8f, -98.6f}, {"AG", 39.8f, -98.6f}, {"AI", 39.8f, -98.6f}, {"AJ", 39.8f, -98.6f},
    {"AK", 39.8f, -98.6f}, {"AL", 39.8f, -98.6f},                     // USA (mainland-ish, rough)
    {"KH6", 20.5f, -157.0f}, {"NH6", 20.5f, -157.0f}, {"WH6", 20.5f, -157.0f}, {"AH6", 20.5f, -157.0f}, // Hawaii
    {"KH2", 13.5f, 144.8f}, {"NH2", 13.5f, 144.8f}, {"WH2", 13.5f, 144.8f}, {"AH2", 13.5f, 144.8f}, // Guam
    {"VE", 56.1f, -106.3f}, {"VA", 56.1f, -106.3f}, {"VO", 47.6f, -52.7f}, {"VY", 62.5f, -114.4f}, // Canada
    {"XE", 23.6f, -102.5f}, {"4A", 23.6f, -102.5f},                   // Mexico
    {"CO", 21.5f, -79.5f}, {"T4", 21.5f, -79.5f},                     // Cuba
    {"HI", 18.7f, -70.2f},                                            // Dominican Republic
    {"KP4", 18.2f, -66.6f},                                           // Puerto Rico
    {"6Y", 18.1f, -77.3f},                                            // Jamaica
    {"C6", 24.25f, -76.0f},                                           // Bahamas
    {"HH", 18.9f, -72.3f},                                            // Haiti
    {"TI", 9.7f, -83.8f},                                             // Costa Rica
    {"TG", 15.8f, -90.2f},                                            // Guatemala
    {"HR", 15.2f, -86.2f},                                            // Honduras
    {"YS", 13.8f, -88.9f},                                            // El Salvador
    {"YN", 12.9f, -85.2f},                                            // Nicaragua
    {"HP", 8.5f, -80.8f},                                             // Panama
    {"V3", 17.2f, -88.5f},                                            // Belize
    {"VP9", 32.3f, -64.75f},                                          // Bermuda
    {"ZF", 19.3f, -81.25f},                                           // Cayman
    {"8P", 13.2f, -59.5f},                                            // Barbados
    {"9Y", 10.7f, -61.2f}, {"9Z", 10.7f, -61.2f},                     // Trinidad & Tobago
    {"J3", 12.1f, -61.7f}, {"J6", 13.9f, -60.9f}, {"J7", 15.4f, -61.4f}, {"J8", 13.2f, -61.2f}, // Grenada/StLucia/Dominica/StVincent
    {"V4", 17.3f, -62.7f}, {"V2", 17.1f, -61.8f},                     // St Kitts, Antigua

    // -- South America --
    {"PY", -10.0f, -55.0f}, {"PP", -10.0f, -55.0f}, {"PT", -10.0f, -55.0f}, {"PR", -10.0f, -55.0f},
    {"PS", -10.0f, -55.0f}, {"PW", -10.0f, -55.0f}, {"PU", -10.0f, -55.0f}, {"PV", -10.0f, -55.0f},
    {"PQ", -10.0f, -55.0f}, {"ZV", -10.0f, -55.0f}, {"ZW", -10.0f, -55.0f}, {"ZX", -10.0f, -55.0f},
    {"ZY", -10.0f, -55.0f}, {"ZZ", -10.0f, -55.0f},                   // Brazil
    {"LU", -34.0f, -64.0f}, {"LW", -34.0f, -64.0f}, {"LS", -34.0f, -64.0f}, {"AZ", -34.0f, -64.0f}, // Argentina
    {"CE", -35.0f, -71.0f}, {"CA", -35.0f, -71.0f}, {"CB", -35.0f, -71.0f},
    {"XQ", -35.0f, -71.0f}, {"XR", -35.0f, -71.0f},                   // Chile
    {"OA", -9.2f, -75.0f},                                            // Peru
    {"HK", 4.6f, -74.1f},                                             // Colombia
    {"YV", 8.0f, -66.0f},                                             // Venezuela
    {"HC", -1.8f, -78.2f}, {"HD", -1.8f, -78.2f},                     // Ecuador
    {"CP", -16.5f, -65.0f},                                           // Bolivia
    {"ZP", -23.4f, -58.4f},                                           // Paraguay
    {"CX", -32.5f, -56.0f},                                           // Uruguay
    {"8R", 4.9f, -58.9f},                                             // Guyana
    {"PZ", 4.1f, -56.0f},                                             // Suriname

    // -- Africa --
    {"ZS", -29.0f, 24.0f}, {"ZR", -29.0f, 24.0f},                     // South Africa
    {"SU", 26.0f, 30.0f},                                             // Egypt
    {"CN", 32.0f, -6.0f},                                             // Morocco
    {"5N", 9.1f, 8.7f},                                               // Nigeria
    {"5Z", 0.0f, 38.0f},                                              // Kenya
    {"5H", -6.4f, 35.0f},                                             // Tanzania
    {"Z2", -19.0f, 30.0f},                                            // Zimbabwe
    {"9J", -13.1f, 27.8f},                                            // Zambia
    {"9G", 7.9f, -1.0f},                                              // Ghana
    {"5X", 1.4f, 32.3f},                                              // Uganda
    {"ET", 9.1f, 40.5f},                                              // Ethiopia
    {"6W", 14.5f, -14.5f},                                            // Senegal
    {"3V", 34.0f, 9.0f},                                              // Tunisia
    {"7X", 28.0f, 3.0f},                                              // Algeria
    {"V5", 22.0f, 17.0f},                                             // Namibia
    {"A2", -22.3f, 24.7f},                                            // Botswana
    {"3B8", -20.3f, 57.6f}, {"3B9", -19.7f, 63.4f},                   // Mauritius, Rodrigues
    {"5R", 19.0f, 47.0f},                                             // Madagascar
    {"ST", 15.6f, 30.2f},                                             // Sudan
    {"9Q", -4.0f, 21.8f},                                             // DR Congo
    {"TJ", 5.7f, 12.7f},                                              // Cameroon
    {"TU", 7.5f, -5.5f},                                              // Ivory Coast
    {"5A", 26.3f, 17.2f},                                             // Libya
    {"D2", -11.2f, 17.9f},                                            // Angola
    {"7Q", -13.3f, 34.3f},                                            // Malawi
    {"C9", -18.7f, 35.5f},                                            // Mozambique
    // Note: "FR" (Reunion) already listed under France overseas above,
    // duplicate prefix kept in the original table -- longest-prefix match
    // is unaffected since both entries share the same length and value.

    // -- Asia --
    {"JA", 36.2f, 138.0f}, {"JH", 36.2f, 138.0f}, {"8J", 36.2f, 138.0f}, {"7N", 36.2f, 138.0f}, // Japan
    {"BY", 35.0f, 105.0f}, {"BG", 35.0f, 105.0f}, {"BD", 35.0f, 105.0f}, {"B", 35.0f, 105.0f}, // China
    {"VU", 21.0f, 78.0f},                                             // India
    {"HL", 36.5f, 128.0f}, {"DS", 36.5f, 128.0f},                     // South Korea
    {"4X", 31.0f, 35.0f}, {"4Z", 31.0f, 35.0f},                       // Israel
    {"HS", 15.0f, 101.0f}, {"E2", 15.0f, 101.0f},                     // Thailand
    {"DU", 13.0f, 122.0f},                                            // Philippines
    {"YB", -2.0f, 118.0f}, {"YC", -2.0f, 118.0f}, {"YD", -2.0f, 118.0f}, {"YE", -2.0f, 118.0f},
    {"YF", -2.0f, 118.0f}, {"YG", -2.0f, 118.0f}, {"YH", -2.0f, 118.0f}, // Indonesia
    {"9M2", 4.2f, 102.0f},                                            // Malaysia
    {"9V", 1.35f, 103.8f},                                            // Singapore
    {"XV", 16.0f, 108.0f}, {"3W", 16.0f, 108.0f},                     // Vietnam
    {"HZ", 24.0f, 45.0f}, {"7Z", 24.0f, 45.0f},                       // Saudi Arabia
    {"A6", 24.0f, 54.0f},                                             // UAE
    {"9K", 29.3f, 47.5f},                                             // Kuwait
    {"A7", 25.3f, 51.2f},                                             // Qatar
    {"A4", 21.0f, 57.0f},                                             // Oman
    {"A9", 26.0f, 50.5f},                                             // Bahrain
    {"JY", 31.2f, 36.5f},                                             // Jordan
    {"OD", 33.9f, 35.5f},                                             // Lebanon
    {"YI", 33.0f, 44.0f},                                             // Iraq
    {"EP", 32.0f, 53.0f},                                             // Iran
    {"AP", 30.0f, 70.0f},                                             // Pakistan
    {"S2", 24.0f, 90.0f},                                             // Bangladesh
    {"4S", 7.9f, 80.8f},                                              // Sri Lanka
    {"UN", 48.0f, 68.0f},                                             // Kazakhstan
    {"UK", 41.4f, 64.6f},                                             // Uzbekistan
    {"JT", 46.9f, 103.8f},                                            // Mongolia
    {"BV", 23.7f, 121.0f},                                            // Taiwan
    {"VR2", 22.3f, 114.2f},                                           // Hong Kong
    {"4K", 40.1f, 47.6f}, {"4J", 40.1f, 47.6f},                       // Azerbaijan
    {"EK", 40.1f, 45.0f},                                             // Armenia
    {"4L", 42.0f, 43.5f},                                             // Georgia
    {"7O", 15.5f, 48.0f},                                             // Yemen

    // -- Oceania --
    {"VK", -25.0f, 134.0f},                                           // Australia
    {"ZL", -41.0f, 174.0f}, {"ZM", -41.0f, 174.0f},                   // New Zealand
    {"P2", -6.3f, 143.9f},                                            // Papua New Guinea
    {"3D2", -18.0f, 178.0f},                                          // Fiji
    {"V6", 6.9f, 158.2f},                                             // Micronesia
    {"T8", 7.5f, 134.6f},                                             // Palau
    {"T30", 1.9f, -157.4f}, {"T31", 3.1f, 172.9f}, {"T32", 1.9f, -157.4f}, // Kiribati
};
#define PREFIX_COORDS_LEN (sizeof(PREFIX_COORDS) / sizeof(PREFIX_COORDS[0]))

bool geo_coords_for_call(const char *call, float *lat_out, float *lon_out)
{
    char prefix[16];
    const char *slash = strchr(call, '/');
    size_t len = slash ? (size_t)(slash - call) : strlen(call);
    if (len >= sizeof(prefix)) {
        len = sizeof(prefix) - 1;
    }
    memcpy(prefix, call, len);
    prefix[len] = '\0';

    int best = -1;
    size_t best_len = 0;
    for (size_t i = 0; i < PREFIX_COORDS_LEN; i++) {
        size_t plen = strlen(PREFIX_COORDS[i].prefix);
        if (plen > best_len && starts_with(prefix, PREFIX_COORDS[i].prefix)) {
            best = (int)i;
            best_len = plen;
        }
    }
    if (best < 0) {
        return false;
    }
    *lat_out = PREFIX_COORDS[best].lat;
    *lon_out = PREFIX_COORDS[best].lon;
    return true;
}
