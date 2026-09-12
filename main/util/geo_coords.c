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
    {"DE", 51.0f, 10.0f}, {"DF", 51.0f, 10.0f}, {"DG", 51.0f, 10.0f}, {"DH", 51.0f, 10.0f},
    {"DI", 51.0f, 10.0f}, {"DJ", 51.0f, 10.0f}, {"DK", 51.0f, 10.0f}, {"DL", 51.0f, 10.0f},
    {"DM", 51.0f, 10.0f}, {"DN", 51.0f, 10.0f}, {"DO", 51.0f, 10.0f}, {"DP", 51.0f, 10.0f},
    {"DQ", 51.0f, 10.0f}, {"DR", 51.0f, 10.0f}, // Germany (DA-DR is ONE ITU block - DE/DI/DN
                                                 // were live-observed missing via scratchpad/geo_audit.py,
                                                 // 2026-09-11: DN9CA alone was one of the single
                                                 // largest unresolved-prefix groups in a live sample
                                                 // of PSK Reporter traffic, 23 distinct callsigns in ~90 min)
    {"G", 54.0f, -2.0f}, {"M", 54.0f, -2.0f}, {"2E", 54.0f, -2.0f},   // UK
    {"2M", 56.5f, -4.0f},                                              // UK - Scotland (2M0, live-observed
                                                                       // 2x, scratchpad/geo_audit.py, 2026-09-11)
    {"2I", 54.6f, -6.7f},                                              // UK - Northern Ireland (2I0, live-observed)
    {"EI", 53.3f, -8.0f}, {"EJ", 53.3f, -8.0f},                       // Ireland
    {"F", 46.5f, 2.5f}, {"TM", 46.5f, 2.5f},                          // France
    {"FO", -17.7f, -149.4f}, {"FK", -21.3f, 165.5f}, {"FP", 46.8f, -56.2f}, // French overseas
    {"FS", 18.1f, -63.1f}, {"FW", -13.3f, -176.2f}, {"FY", 4.0f, -53.0f},
    {"FR", -21.1f, 55.5f}, {"FM", 14.6f, -61.0f}, {"FG", 16.2f, -61.6f},
    {"I", 42.5f, 12.5f}, {"IK", 42.5f, 12.5f},                        // Italy
    {"IH9", 36.8f, 12.0f}, {"IG9", 35.5f, 12.6f},                     // Pantelleria/Lampedusa -> Africa-ish
    {"EA", 40.0f, -4.0f}, {"EB", 40.0f, -4.0f}, {"EC", 40.0f, -4.0f},
    {"ED", 40.0f, -4.0f}, {"EF", 40.0f, -4.0f}, {"EH", 40.0f, -4.0f}, // Spain (EB/EC live-observed
                                                                       // missing, scratchpad/geo_audit.py, 2026-09-11)
    {"EA8", 28.3f, -16.6f}, {"EA9", 35.9f, -5.3f},                    // Canary Islands, Ceuta/Melilla
    {"EG8", 28.3f, -16.6f}, {"EG9", 35.9f, -5.3f},
    {"EE", 40.0f, -4.0f}, {"AM", 40.0f, -4.0f}, {"AO", 40.0f, -4.0f}, // Spain special-event prefixes
                                                                       // (EE3/EE5/AM4/AO5/AO7 live-observed,
                                                                       // scratchpad/geo_audit.py, 2026-09-11/12)
    {"CT", 39.5f, -8.0f}, {"CQ", 39.5f, -8.0f}, {"CR", 39.5f, -8.0f}, {"CS", 39.5f, -8.0f},
    {"CU", 37.7f, -25.7f},                                            // Portugal (CU = Azores)
    {"CT3", 32.7f, -17.0f}, {"CS3", 32.7f, -17.0f}, {"CT9", 32.7f, -17.0f},
    {"CR3", 32.7f, -17.0f}, {"CQ9", 32.7f, -17.0f},                   // Madeira
    {"HB", 46.8f, 8.2f}, {"HE", 46.8f, 8.2f}, {"HB0", 47.1f, 9.5f},   // Switzerland/Liechtenstein
    {"OE", 47.5f, 14.5f},                                             // Austria
    {"PA", 52.2f, 5.5f}, {"PB", 52.2f, 5.5f}, {"PC", 52.2f, 5.5f}, {"PD", 52.2f, 5.5f},
    {"PE", 52.2f, 5.5f}, {"PF", 52.2f, 5.5f}, {"PG", 52.2f, 5.5f}, {"PH", 52.2f, 5.5f},
    {"PI", 52.2f, 5.5f},                                              // Netherlands (PB-PI were the
                                                                       // single biggest gap found by
                                                                       // scratchpad/geo_audit.py, 2026-09-11 -
                                                                       // PE1/PD0/PD1 alone were >100 distinct
                                                                       // callsigns in under 90 minutes)
    {"ON", 50.6f, 4.5f}, {"OO", 50.6f, 4.5f}, {"OP", 50.6f, 4.5f}, {"OQ", 50.6f, 4.5f},
    {"OR", 50.6f, 4.5f}, {"OS", 50.6f, 4.5f}, {"OT", 50.6f, 4.5f},   // Belgium (ON-OT is one ITU block -
                                                                       // OO/OS/OT live-observed missing,
                                                                       // scratchpad/geo_audit.py, 2026-09-11)
    {"LX", 49.8f, 6.1f},                                              // Luxembourg
    {"OZ", 56.0f, 10.0f}, {"5Q", 56.0f, 10.0f}, {"OU", 56.0f, 10.0f},
    {"OV", 56.0f, 10.0f}, {"OW", 56.0f, 10.0f}, {"5P", 56.0f, 10.0f},
    {"OY", 62.0f, -7.0f},                                              // Denmark (OY = Faroe Islands;
                                                                       // 5P live-observed missing,
                                                                       // scratchpad/geo_audit.py, 2026-09-11)
    {"LA", 61.0f, 9.0f}, {"LB", 61.0f, 9.0f}, {"LC", 61.0f, 9.0f}, {"LD", 61.0f, 9.0f},
    {"LE", 61.0f, 9.0f}, {"LF", 61.0f, 9.0f}, {"LG", 61.0f, 9.0f}, {"LH", 61.0f, 9.0f},
    {"LI", 61.0f, 9.0f}, {"LJ", 61.0f, 9.0f}, {"LK", 61.0f, 9.0f}, {"LL", 61.0f, 9.0f},
    {"LM", 61.0f, 9.0f}, {"LN", 61.0f, 9.0f},                         // Norway (LB-LN: LB8LH
                                                                       // live-observed, scratchpad/geo_audit.py)
    {"SM", 62.0f, 15.0f}, {"7S", 62.0f, 15.0f},
    {"SA", 62.0f, 15.0f}, {"SB", 62.0f, 15.0f}, {"SC", 62.0f, 15.0f}, {"SD", 62.0f, 15.0f},
    {"SE", 62.0f, 15.0f}, {"SF", 62.0f, 15.0f}, {"SG", 62.0f, 15.0f}, {"SH", 62.0f, 15.0f},
    {"SI", 62.0f, 15.0f}, {"SJ", 62.0f, 15.0f}, {"SK", 62.0f, 15.0f}, {"SL", 62.0f, 15.0f},
    {"8S", 62.0f, 15.0f},                                             // Sweden (8S is a special-event
                                                                       // prefix, same country as SA-SL -
                                                                       // (SA-SL: SA0/SA5/SA6/SA7 among the
                                                                       // largest live gaps, scratchpad/geo_audit.py)
    {"OH", 64.0f, 26.0f}, {"OI", 64.0f, 26.0f}, {"OJ", 64.0f, 26.0f},
    {"OF", 64.0f, 26.0f}, {"OG", 64.0f, 26.0f},                       // Finland
    {"SP", 52.0f, 19.0f}, {"3Z", 52.0f, 19.0f}, {"HF", 52.0f, 19.0f},
    {"SN", 52.0f, 19.0f}, {"SO", 52.0f, 19.0f}, {"SQ", 52.0f, 19.0f}, {"SR", 52.0f, 19.0f}, // Poland
                                                                       // (SQ was one of the single largest live
                                                                       // gaps of the whole audit, scratchpad/geo_audit.py)
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
    {"UR", 49.0f, 32.0f}, {"US", 49.0f, 32.0f}, {"UT", 49.0f, 32.0f}, {"UU", 49.0f, 32.0f},
    {"UV", 49.0f, 32.0f}, {"UW", 49.0f, 32.0f}, {"UX", 49.0f, 32.0f}, {"UY", 49.0f, 32.0f},
    {"UZ", 49.0f, 32.0f}, {"EM", 49.0f, 32.0f}, {"EN", 49.0f, 32.0f}, {"EO", 49.0f, 32.0f}, // Ukraine
                                                                       // (US/UW live-observed missing,
                                                                       // scratchpad/geo_audit.py, 2026-09-11)
    {"ER", 47.0f, 28.9f},                                             // Moldova
    {"R", 60.0f, 60.0f}, {"UA", 60.0f, 60.0f}, {"UB", 60.0f, 60.0f}, {"UC", 60.0f, 60.0f},
    {"UD", 60.0f, 60.0f}, {"UE", 60.0f, 60.0f}, {"UF", 60.0f, 60.0f}, {"UG", 60.0f, 60.0f},
    {"UH", 60.0f, 60.0f}, {"UI", 60.0f, 60.0f},
    {"U", 60.0f, 60.0f},                                              // Russia (rough, huge country).
                                                                       // Bare "U" catch-all for the "U+digit"
                                                                       // special-event scheme (U1-U7 = European
                                                                       // Russia, U8/U9 = Asian Russia, per
                                                                       // qsl.net/rw3ah's USSR prefix writeup) -
                                                                       // U49/U80/U88/U89/U95/U1B/U17/U64/U74 were
                                                                       // all live-observed, scratchpad/geo_audit.py.
                                                                       // Deliberately SHORTER than Ukraine's
                                                                       // UR-UZ/EM-EO entries above, so longest-
                                                                       // prefix-match still gives those priority -
                                                                       // this only catches what nothing else did.
    {"9H", 35.9f, 14.5f},                                             // Malta
    {"T7", 43.9f, 12.4f},                                             // San Marino
    {"HV", 41.9f, 12.45f},                                            // Vatican
    {"3A", 43.7f, 7.4f},                                              // Monaco
    {"C3", 42.5f, 1.5f},                                              // Andorra
    {"ZB", 36.1f, -5.3f},                                             // Gibraltar
    {"TF", 65.0f, -18.0f},                                            // Iceland
    {"JW", 78.2f, 15.6f},                                             // Svalbard (new entry, JW1 live-observed)
    {"TA", 39.0f, 35.0f}, {"TC", 39.0f, 35.0f}, {"YM", 39.0f, 35.0f}, // Turkey

    // -- North America --
    {"W", 39.8f, -98.6f}, {"K", 39.8f, -98.6f}, {"N", 39.8f, -98.6f}, {"AA", 39.8f, -98.6f},
    {"AB", 39.8f, -98.6f}, {"AC", 39.8f, -98.6f}, {"AD", 39.8f, -98.6f}, {"AE", 39.8f, -98.6f},
    {"AF", 39.8f, -98.6f}, {"AG", 39.8f, -98.6f}, {"AI", 39.8f, -98.6f}, {"AJ", 39.8f, -98.6f},
    {"AK", 39.8f, -98.6f}, {"AL", 39.8f, -98.6f},                     // USA (mainland-ish, rough)
    {"KH6", 20.5f, -157.0f}, {"NH6", 20.5f, -157.0f}, {"WH6", 20.5f, -157.0f}, {"AH6", 20.5f, -157.0f}, // Hawaii
    {"KH2", 13.5f, 144.8f}, {"NH2", 13.5f, 144.8f}, {"WH2", 13.5f, 144.8f}, {"AH2", 13.5f, 144.8f}, // Guam
    {"VE", 56.1f, -106.3f}, {"VA", 56.1f, -106.3f}, {"VO", 47.6f, -52.7f}, {"VY", 62.5f, -114.4f},
    {"CF", 56.1f, -106.3f}, {"CG", 56.1f, -106.3f}, {"CH", 56.1f, -106.3f}, {"CI", 56.1f, -106.3f},
    {"CJ", 56.1f, -106.3f}, {"CK", 56.1f, -106.3f}, {"VB", 56.1f, -106.3f}, {"VC", 56.1f, -106.3f},
    {"VD", 56.1f, -106.3f}, {"VF", 56.1f, -106.3f}, {"VG", 56.1f, -106.3f}, {"VX", 56.1f, -106.3f},
    {"XJ", 56.1f, -106.3f}, {"XK", 56.1f, -106.3f}, {"XL", 56.1f, -106.3f}, {"XM", 56.1f, -106.3f},
    {"XN", 56.1f, -106.3f}, {"XO", 56.1f, -106.3f},                   // Canada (CF-CK/VA-VG/VO/VX-VY/XJ-XO is
                                                                       // ONE ITU block - CK3/CK5/XM8/VX1/VB9/
                                                                       // CG7 were the single largest live gap
                                                                       // in the full audit run, scratchpad/
                                                                       // geo_audit.py, 2026-09-11/12, verified
                                                                       // against ac6v.com + Wikipedia ITU table)
    {"XE", 23.6f, -102.5f}, {"4A", 23.6f, -102.5f},
    {"XA", 23.6f, -102.5f}, {"XB", 23.6f, -102.5f}, {"XC", 23.6f, -102.5f}, {"XD", 23.6f, -102.5f},
    {"XF", 23.6f, -102.5f}, {"XG", 23.6f, -102.5f}, {"XH", 23.6f, -102.5f}, {"XI", 23.6f, -102.5f}, // Mexico
                                                                       // (XA-XI is one block, only XE was
                                                                       // covered before; XA5 live-observed,
                                                                       // scratchpad/geo_audit.py)
    {"CO", 21.5f, -79.5f}, {"T4", 21.5f, -79.5f}, {"CM", 21.5f, -79.5f}, // Cuba (CM8/CM6 live-observed)
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
    {"VP2M", 16.75f, -62.2f},                                         // Montserrat (VP2MAA live-observed -
                                                                       // VP2 is split by 3rd letter into three
                                                                       // separate DXCC entities, only the
                                                                       // actually-observed one is added)
    {"VP5", 21.75f, -71.6f},                                          // Turks & Caicos Islands
    {"P4", 12.5f, -70.0f},                                            // Aruba (new entry, P41 live-observed)

    // -- South America --
    {"PY", -10.0f, -55.0f}, {"PP", -10.0f, -55.0f}, {"PT", -10.0f, -55.0f}, {"PR", -10.0f, -55.0f},
    {"PS", -10.0f, -55.0f}, {"PW", -10.0f, -55.0f}, {"PU", -10.0f, -55.0f}, {"PV", -10.0f, -55.0f},
    {"PQ", -10.0f, -55.0f}, {"ZV", -10.0f, -55.0f}, {"ZW", -10.0f, -55.0f}, {"ZX", -10.0f, -55.0f},
    {"ZY", -10.0f, -55.0f}, {"ZZ", -10.0f, -55.0f}, {"PX", -10.0f, -55.0f}, // Brazil (PX9 live-observed,
                                                                       // scratchpad/geo_audit.py)
    {"LU", -34.0f, -64.0f}, {"LW", -34.0f, -64.0f}, {"LS", -34.0f, -64.0f}, {"AZ", -34.0f, -64.0f},
    {"LO", -34.0f, -64.0f}, {"LP", -34.0f, -64.0f}, {"LQ", -34.0f, -64.0f}, {"LR", -34.0f, -64.0f},
    {"LT", -34.0f, -64.0f}, {"LV", -34.0f, -64.0f},                   // Argentina (LO-LW is one ITU block -
                                                                       // LV1/LV9/LQ3 were live-observed,
                                                                       // scratchpad/geo_audit.py, 2026-09-11/12)
    {"CE", -35.0f, -71.0f}, {"CA", -35.0f, -71.0f}, {"CB", -35.0f, -71.0f},
    {"CC", -35.0f, -71.0f}, {"CD", -35.0f, -71.0f},
    {"XQ", -35.0f, -71.0f}, {"XR", -35.0f, -71.0f},                   // Chile (CA-CE is one block -
                                                                       // CD3 live-observed, scratchpad/geo_audit.py)
    {"VP8", -51.7f, -59.5f},                                          // Falkland Islands (VP8 also covers
                                                                       // South Georgia/S.Sandwich as a
                                                                       // SEPARATE DXCC entity at ~-54.3,-36.5 -
                                                                       // no way to tell them apart from the
                                                                       // prefix alone, Falklands used as the
                                                                       // more common default)
    {"OA", -9.2f, -75.0f},                                            // Peru
    {"HK", 4.6f, -74.1f},                                             // Colombia
    {"YV", 8.0f, -66.0f}, {"YW", 8.0f, -66.0f}, {"YX", 8.0f, -66.0f},
    {"YY", 8.0f, -66.0f},                                             // Venezuela (YV-YY is one block -
                                                                       // YY live-observed missing,
                                                                       // scratchpad/geo_audit.py, 2026-09-11)
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
    {"5T", 18.1f, -16.0f},                                            // Mauritania (new entry -
                                                                       // live-observed missing entirely,
                                                                       // e.g. 5T5T; scratchpad/geo_audit.py,
                                                                       // 2026-09-11)
    {"3V", 34.0f, 9.0f},                                              // Tunisia
    {"7X", 28.0f, 3.0f},                                              // Algeria
    {"V5", 22.0f, 17.0f},                                             // Namibia
    {"A2", -22.3f, 24.7f},                                            // Botswana
    {"3B8", -20.3f, 57.6f}, {"3B9", -19.7f, 63.4f},                   // Mauritius, Rodrigues
    {"5R", 19.0f, 47.0f},                                             // Madagascar
    {"ST", 15.6f, 30.2f},                                             // Sudan
    {"9Q", -4.0f, 21.8f},                                             // DR Congo
    {"TN", -1.0f, 15.8f},                                             // Republic of the Congo (new entry -
                                                                       // TN8GD live-observed, scratchpad/
                                                                       // geo_audit.py, 2026-09-11)
    {"TJ", 5.7f, 12.7f},                                              // Cameroon
    {"TL", 4.5f, 18.0f},                                              // Central African Republic (new
                                                                       // entry - live-observed missing
                                                                       // entirely, e.g. TL8GD;
                                                                       // scratchpad/geo_audit.py, 2026-09-11)
    {"TT", 15.0f, 19.0f},                                             // Chad (new entry, TT1 live-observed)
    {"TU", 7.5f, -5.5f},                                              // Ivory Coast
    {"5A", 26.3f, 17.2f},                                             // Libya
    {"D2", -11.2f, 17.9f},                                            // Angola
    {"7Q", -13.3f, 34.3f},                                            // Malawi
    {"C9", -18.7f, 35.5f},                                            // Mozambique
    {"D4", 16.0f, -24.0f},                                            // Cape Verde (new entry, D44 live-observed)
    {"5V", 8.6f, 1.0f},                                               // Togo (new entry, 5V7 live-observed)
    {"3X", 9.5f, -13.7f},                                             // Guinea (new entry, 3XO live-observed)
    {"ZD7", -15.9f, -5.7f},                                           // St Helena (new entry, ZD7 live-observed)
    {"ZD8", -7.9f, -14.4f},                                           // Ascension Island (new entry, ZD8
                                                                       // live-observed)
    {"S7", -4.6f, 55.5f},                                             // Seychelles (new entry, S79 live-observed)
    // Note: "FR" (Reunion) already listed under France overseas above,
    // duplicate prefix kept in the original table -- longest-prefix match
    // is unaffected since both entries share the same length and value.

    // -- Asia --
    {"JA", 36.2f, 138.0f}, {"JE", 36.2f, 138.0f}, {"JF", 36.2f, 138.0f}, {"JG", 36.2f, 138.0f},
    {"JH", 36.2f, 138.0f}, {"JI", 36.2f, 138.0f}, {"JJ", 36.2f, 138.0f}, {"JK", 36.2f, 138.0f},
    {"JL", 36.2f, 138.0f}, {"JM", 36.2f, 138.0f}, {"JN", 36.2f, 138.0f}, {"JO", 36.2f, 138.0f},
    {"JP", 36.2f, 138.0f}, {"JQ", 36.2f, 138.0f}, {"JR", 36.2f, 138.0f}, {"JS", 36.2f, 138.0f},
    {"7J", 36.2f, 138.0f}, {"7K", 36.2f, 138.0f}, {"7L", 36.2f, 138.0f}, {"7M", 36.2f, 138.0f},
    {"7N", 36.2f, 138.0f},
    {"8J", 36.2f, 138.0f}, {"8K", 36.2f, 138.0f}, {"8L", 36.2f, 138.0f}, {"8M", 36.2f, 138.0f},
    {"8N", 36.2f, 138.0f}, // Japan (JE-JS/7J-7M/8K-8N: the single largest source of
                           // live-observed gaps of the whole audit - JK1/JI1/JR1/JL1/
                           // JF1/JJ1/JM1/JE1/JG1 etc. together were hundreds of distinct
                           // callsigns in under 90 minutes, scratchpad/geo_audit.py, 2026-09-11)
    {"BY", 35.0f, 105.0f}, {"BG", 35.0f, 105.0f}, {"BD", 35.0f, 105.0f}, {"B", 35.0f, 105.0f}, // China
    {"VU", 21.0f, 78.0f},
    {"8V", 21.0f, 78.0f}, {"8W", 21.0f, 78.0f}, {"8X", 21.0f, 78.0f}, {"8Y", 21.0f, 78.0f}, // India
                                                                       // (8V-8Y is a supplementary ITU block -
                                                                       // 8V5 live-observed, scratchpad/geo_audit.py)
    {"HL", 36.5f, 128.0f}, {"DS", 36.5f, 128.0f},
    {"6K", 36.5f, 128.0f}, {"6L", 36.5f, 128.0f}, {"6M", 36.5f, 128.0f}, {"6N", 36.5f, 128.0f}, // South Korea
                                                                       // (6K-6N is a supplementary block -
                                                                       // 6K2/6K5 were the single largest
                                                                       // live gap of the whole audit run,
                                                                       // scratchpad/geo_audit.py, 2026-09-11/12)
    {"4X", 31.0f, 35.0f}, {"4Z", 31.0f, 35.0f},                       // Israel
    {"HS", 15.0f, 101.0f}, {"E2", 15.0f, 101.0f},                     // Thailand
    {"DU", 13.0f, 122.0f},
    {"DV", 13.0f, 122.0f}, {"DW", 13.0f, 122.0f}, {"DX", 13.0f, 122.0f}, {"DY", 13.0f, 122.0f},
    {"DZ", 13.0f, 122.0f}, {"4D", 13.0f, 122.0f}, {"4E", 13.0f, 122.0f}, {"4F", 13.0f, 122.0f},
    {"4G", 13.0f, 122.0f}, {"4H", 13.0f, 122.0f}, {"4I", 13.0f, 122.0f}, // Philippines (DU-DZ + 4D-4I is
                                                                       // one ITU block, only DU was covered
                                                                       // before - DY9/4F3/4I1 live-observed,
                                                                       // scratchpad/geo_audit.py, 2026-09-11/12)
    {"YB", -2.0f, 118.0f}, {"YC", -2.0f, 118.0f}, {"YD", -2.0f, 118.0f}, {"YE", -2.0f, 118.0f},
    {"YF", -2.0f, 118.0f}, {"YG", -2.0f, 118.0f}, {"YH", -2.0f, 118.0f},
    {"7E", -2.0f, 118.0f}, {"7I", -2.0f, 118.0f},                     // Indonesia (7E/7I are a supplementary
                                                                       // block - live-observed, scratchpad/geo_audit.py)
    {"9M2", 4.2f, 102.0f}, {"9W2", 4.2f, 102.0f},                     // Malaysia (West; 9W2 live-observed
                                                                       // missing, scratchpad/geo_audit.py, 2026-09-11)
    {"9M6", 4.0f, 114.0f}, {"9M8", 4.0f, 114.0f},                     // Malaysia (East - Sabah/Sarawak;
                                                                       // 9M8 live-observed, scratchpad/geo_audit.py)
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
    {"EP", 32.0f, 53.0f}, {"9D", 32.0f, 53.0f},                       // Iran (9D live-observed, e.g.
                                                                       // 9D1G - special/club prefix within
                                                                       // Iran's ITU 9B-9D block, verified
                                                                       // against ac6v.com + Wikipedia's ITU
                                                                       // prefix table, 2026-09-11)
    {"AP", 30.0f, 70.0f},                                             // Pakistan
    {"S2", 24.0f, 90.0f},                                             // Bangladesh
    {"4S", 7.9f, 80.8f},                                              // Sri Lanka
    {"UN", 48.0f, 68.0f},                                             // Kazakhstan
    {"UK", 41.4f, 64.6f},                                             // Uzbekistan
    {"EX", 41.2f, 74.7f},                                             // Kyrgyzstan (live-observed
                                                                       // missing entirely, scratchpad/geo_audit.py)
    {"JT", 46.9f, 103.8f},                                            // Mongolia
    {"BV", 23.7f, 121.0f},                                            // Taiwan
    {"VR2", 22.3f, 114.2f},                                           // Hong Kong
    {"4K", 40.1f, 47.6f}, {"4J", 40.1f, 47.6f},                       // Azerbaijan
    {"EK", 40.1f, 45.0f},                                             // Armenia
    {"4L", 42.0f, 43.5f},                                             // Georgia
    {"7O", 15.5f, 48.0f},                                             // Yemen

    // -- Oceania --
    {"VK", -25.0f, 134.0f}, {"VJ", -25.0f, 134.0f},
    {"VH", -25.0f, 134.0f}, {"VI", -25.0f, 134.0f}, {"VL", -25.0f, 134.0f},
    {"VM", -25.0f, 134.0f}, {"VN", -25.0f, 134.0f},                   // Australia (VH-VN is Australia's
                                                                       // full ITU block per Wikipedia's ITU
                                                                       // prefix table; VJ6/VI9 live-observed,
                                                                       // scratchpad/geo_audit.py, 2026-09-11/12)
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
