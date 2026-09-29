#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Shorten a country name to at most max_chars, writing into out.
 *
 * Parenthetical first, then a known short form, then generic abbreviations,
 * then a cut on a word boundary. NEVER returns a 3-letter code - see the long
 * comment in country_shorten.c for why that rule was reversed in v1.15.x.
 *
 * Portable: no ESP dependencies, host-tested by test/country_shorten_harness.c. */
void country_shorten(const char *full, int max_chars, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
