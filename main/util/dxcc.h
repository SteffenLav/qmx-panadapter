#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Look up the DXCC entity name for a callsign by prefix match.
// Returns a pointer to a static string (the entity name) or NULL
// when no prefix in the built-in table matches. The returned
// pointer is valid for the lifetime of the program.
//
// Limitations:
//   - Built-in table covers ~190 of the ~340 DXCC entities, focused on
//     active HF prefixes. Rare entities will miss and return NULL.
//   - Portable suffixes (/P, /M, /MM, /AM) are stripped from the head
//     of the callsign before lookup. A leading-prefix slash
//     ("OZ1LAV/W3") is honoured: the part after the slash is used
//     for lookup.
//   - No date-range exceptions (e.g. former entities).
const char *dxcc_lookup(const char *callsign);

#ifdef __cplusplus
}
#endif
