#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Live spots on the spectrum: stations currently on the air, drawn at their
// frequency under the trace so you can see who is where without leaving the
// radio.
//
// Source-agnostic by design. POTA (api.pota.app, a plain JSON GET) is the
// first source; RBN is intended as a second one feeding the SAME store, so
// the display layer never has to know where a spot came from - and a
// misbehaving source can be dropped without losing the feature.
//
// Threading: one background fetch task (PSRAM stack, low priority) owns the
// network side and publishes into a mutex-protected store. The UI reads a
// snapshot from the LVGL thread and never blocks on the network.

typedef enum {
    SPOT_SRC_POTA = 0,
    SPOT_SRC_RBN,
    SPOT_SRC_CLUSTER,   // human DX-cluster spots - the only source of PHONE
} spot_source_t;

typedef enum {
    SPOT_MODE_OTHER = 0,
    SPOT_MODE_CW,
    SPOT_MODE_SSB,
    SPOT_MODE_DIGI,      // FT8/FT4/digital
} spot_mode_t;

typedef struct {
    char          call[12];   // the station on the air
    char          ref[10];    // POTA park reference ("US-1211"), "" if none
    uint32_t      freq_hz;
    spot_mode_t   mode;
    spot_source_t source;
    int64_t       heard_unix; // when it was spotted (UTC seconds)
    // Set on an ACTIVATION spot (POTA/SOTA) when the RBN independently heard
    // the same callsign on the same frequency. Two different things are true
    // of such a spot: it is not a duplicate to be drawn twice, and it is
    // corroborated - somebody's receiver actually copied it just now, rather
    // than it being a self-spot typed in an hour ago. Set by the read path,
    // never by a producer; see spots_get_in_range().
    bool          rbn_confirmed;
} spot_t;

// Headroom, not a measurement: the live POTA feed returned 94 spots one day and
// 96 the next, and the first cap here WAS 96 - i.e. it was already silently
// truncating on day two. RBN is meant to feed the same store later, so this has
// to hold both sources at once. At ~48 bytes per entry the whole table is under
// 10 KB of PSRAM, so there is no reason to run it tight.
#define SPOTS_MAX 200

// Start the background fetcher. Safe to call once at boot; does nothing until
// WiFi is up and the feature is enabled in settings.
void spots_init(void);

// Copy up to max spots into out, newest first. Returns the count written.
// Cheap and lock-bounded - safe from the LVGL thread.
int  spots_get(spot_t *out, int max);

// Copy only the spots whose frequency falls inside [lo_hz, hi_hz] - what the
// spectrum lane needs. Newest first, so a caller that runs out of label room
// keeps the freshest.
int  spots_get_in_range(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz);

// As spots_get_in_range, but bounded by wait_ms on the store lock and returning
// -1 (not 0) when the lock could not be taken. The LVGL thread uses this: a
// missed refresh should leave the previous picture on screen, whereas a 0 would
// blank the lane for a tick. Never wait long here - a stalled LVGL thread is a
// dropped display frame.
int  spots_get_in_range_wait(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz,
                             int wait_ms);

// Bumped once per successful store replacement. Lets the UI decide whether
// anything actually changed without copying the table first.
uint32_t spots_version(void);

// Replace every spot belonging to `src` with `list`, leaving the other sources'
// entries alone. This is how a second producer joins the store: POTA and RBN
// each own their slice and neither can wipe the other's. `list` must not point
// into the store itself.
void spots_publish(spot_source_t src, const spot_t *list, int n);

// Seconds since the last successful fetch, or -1 if none yet. Drives the
// "spots are stale" state in the UI.
int  spots_age_s(void);

// Look up the park/summit reference a station was spotted activating, so a
// completed QSO can be logged as a chase (ADIF SIG/SIG_INFO). Matches on
// callsign and, if freq_hz is non-zero, requires the spot to be within the
// same duplicate tolerance used for collapsing the lane - a station spotted
// on 40 m says nothing about a contact made on 20 m.
//
// Returns true and fills ref_out (+ sig_out with "POTA"/"SOTA") only for a
// source that actually carries a reference; an RBN spot never does. Deliberately
// a lookup at QSO time rather than something cached per decode: the reference
// has to be right in the log, and the spot may have aged out by the time the
// contact completes - so the caller resolves it while the contact is fresh.
bool spots_activation_for_call(const char *call, uint32_t freq_hz,
                               char *sig_out, size_t sig_sz,
                               char *ref_out, size_t ref_sz);

// True when ANY source (POTA, RBN, DX cluster) is enabled. The display gates on
// this rather than on spots_en, so no single source checkbox silently blanks
// the lane for the others.
bool spots_any_source_enabled(void);

// Ask for a refresh now (e.g. the operator just enabled the feature or
// changed band). Coalesced with the periodic cycle.
void spots_request_refresh(void);
