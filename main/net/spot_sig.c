// See spot_sig.h. No ESP dependencies - test/spot_sig_harness.c compiles this
// file directly.

#include "spot_sig.h"
#include <string.h>

const char *spot_sig_for(spot_source_t src, const char *ref)
{
    // A spot that came from a programme's OWN feed needs no guessing.
    if (src == SPOT_SRC_SOTA) return "SOTA";
    if (src == SPOT_SRC_POTA) return "POTA";

    // A DX cluster spot carries whatever the spotter typed, and
    // dxcluster.c's find_reference() deliberately accepts all three kinds, so
    // the reference's own shape has to settle it:
    //
    //   G/LD-049, DM/BW-193   a '/' BEFORE the dash - SOTA associations are the
    //                         only one of the three that is region-qualified
    //   DLFF-0123, ONFF-0259  "FF" immediately before the dash - WWFF
    //   ES-2081, US-1254      anything else - POTA
    //
    // The '/' test comes first because it is the stronger signal: a SOTA
    // reference always has one and neither of the others ever does.
    if (ref) {
        const char *dash = strchr(ref, '-');
        if (dash) {
            size_t before = (size_t)(dash - ref);
            if (memchr(ref, '/', before)) return "SOTA";
            if (before >= 2 && strncasecmp(dash - 2, "FF", 2) == 0) return "WWFF";
        }
    }
    return "POTA";
}
