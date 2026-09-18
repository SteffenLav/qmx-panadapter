#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One answer to "where is this station" for every screen that shows it.
//
// Two tables sit underneath, and they are NOT interchangeable:
//   util/dxcc.c        ~580 prefixes, DXCC-entity granular, carries the
//                      spelled-out name. Hawaii, Alaska and Sardinia keep
//                      their own identity here.
//   util/geo_coords.c  ~4,100 prefixes (Uwe DL8UG's, generated from cty.dat),
//                      ISO 3166-1 country level only, plus a centroid.
//
// Measured 2026-09-17 against cty.dat: dxcc.c answers nothing for 973 of its
// 6,310 prefixes. geo_coords covers most of those, so it backstops the name -
// but at country granularity, which is why it is the FALLBACK and not the
// source.

// The name to print in a column `max_chars` wide.
//
// Spelled out when it fits, SHORTENED when it does not - never a 3-letter code.
// See util/country_shorten.c: a parenthetical goes first, then a known short
// form, then abbreviations, then a cut that marks itself with a full stop.
//
// ⛔ REVERSED 2026-09-19. This returned the ISO code for anything too long -
// "NLD", not "Netherlan" - because a clipped name reads as a bug. True, but the
// worse fault was mixing the two: "Sweden / Ireland / NLD / Italy" asks the
// reader to switch alphabets mid-column. Operator: "apples and pears".
//
// ⚠ The shortened result lives in a STATIC buffer, valid until the next call.
// Every caller prints it immediately. Do not store the pointer.
//
// Returns NULL when neither table knows the callsign.
const char *country_display(const char *call, int max_chars);

// Distance fallback for a station whose GRID we never decoded - a report or
// an RR73 carries none, which is why the KM column is so often blank.
//
// ⛔ THIS IS A COUNTRY CENTROID, NOT A POSITION. A whole country collapses to
// one point: for the USA that point is ~2,000 km from either coast. It is
// strictly WORSE than a Maidenhead square (~70-150 km), so it must only ever
// be used when there is no grid at all, and the caller MUST mark the result
// as approximate - the house rule is that a missing field is honest and a
// wrong one is not, and an unmarked centroid distance is a measurement we
// did not make.
//
// Returns false and leaves *km_out alone when the prefix is unknown.
bool country_centroid_km(const char *call, double my_lat, double my_lon,
                         double *km_out);

#ifdef __cplusplus
}
#endif
