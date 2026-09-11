#pragma once

// Is this token shaped like a real amateur callsign?
//
// WHY THIS EXISTS. The QMX's CW decoder emits a steady stream of garbage
// between real characters, and cw_qso.c's capture used to accept anything 3-10
// characters long that held a letter, a digit and nothing but [A-Z0-9/]. That
// admits plenty of things no callsign can be, and the "new station" card put
// them on screen as if they were stations (operator, 2026-09-11: the card
// "shows up immaturely and say callsigns that are far too long").
//
// Measured against 744 real TB; responses captured on 20 m the same day, the
// old rule accepted SC3Z75HSC, TEE5A and E9VA alongside the genuine OT4A,
// JH6KZP and DL7MKI. Those vectors are the harness's test set - real bytes off
// the air, not invented ones (test/cw_callsign_harness.c).
//
// ⚠ A SHAPE TEST CANNOT BE A TRUTH TEST. EV9C is structurally perfect and EV is
// a real Belarus prefix; nothing here can know whether anybody is on the air
// with it. This narrows the field; it does not certify a station. The capture
// layer adds corroboration on top (a call heard twice, or heard after DE/CQ),
// which is what real CW practice provides for free - operators send their call
// twice precisely because one pass may not survive the noise.
//
// Portable: no ESP-IDF, no LVGL. cw_call_structure_ok() is pure string logic;
// cw_call_is_plausible() additionally consults the DXCC prefix table (also
// portable), so both link into the host harness.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Structural test only - the ITU shape, no knowledge of which prefixes are
// actually allocated.
//
// A base call is  <prefix><area digit><suffix>  where
//   prefix  1-3 chars:  A | AA | #A | #AA        (a 3-letter prefix like "TEE"
//                                                 is NOT a callsign prefix)
//   digit   the LAST digit in the base
//   suffix  1-4 characters, all letters
//
// '/' affixes are allowed either side (EA2GM/P, PJ4/K1ABC, DL/G0ABC/QRP): at
// most three parts, exactly one of which is a base, the rest 1-4 alphanumerics.
bool cw_call_structure_ok(const char *t);

// Structure AND an allocated DXCC prefix. This is what the capture uses.
// Rejects E9VA (E9 is unallocated) while keeping OT4A, JH6KZP, 2E0ABC, 9A1A.
bool cw_call_is_plausible(const char *t);

#ifdef __cplusplus
}
#endif
