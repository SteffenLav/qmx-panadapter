#pragma once
#include "spots.h"

// Which award programme a spot's reference belongs to - the string that goes in
// the ADIF SIG field of a logged chase.
//
// Deliberately in its own dependency-free file (like adif/lotw_tq8.c): it is
// decided by string shape, it decides what ends up in somebody's log, and being
// portable means test/spot_sig_harness.c compiles THIS EXACT CODE rather than a
// hand-copied mirror of it that can silently drift.
//
// `ref` may be NULL or empty; the result is then the source's own programme.
// Never returns NULL.
const char *spot_sig_for(spot_source_t src, const char *ref);

// The same decision made from the REFERENCE ALONE, for a chase that never came
// from a spot at all - the web log editor lets an operator type a park/summit
// reference against an already-logged QSO to mark it Park-to-Park, and there is
// no source to ask. Exported rather than duplicated in the web handler so there
// stays exactly ONE rule deciding what ends up in somebody's SIG field.
const char *spot_sig_for_ref(const char *ref);
