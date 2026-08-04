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
} spot_t;

#define SPOTS_MAX 96

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

// Seconds since the last successful fetch, or -1 if none yet. Drives the
// "spots are stale" state in the UI.
int  spots_age_s(void);

// Ask for a refresh now (e.g. the operator just enabled the feature or
// changed band). Coalesced with the periodic cycle.
void spots_request_refresh(void);
