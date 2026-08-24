#pragma once
#include <stdint.h>
#include <stdbool.h>

/* WSPR spot store - the data layer behind the Phase 3 UI.
 *
 * Deliberately NOT modelled on ft8_screen.c's call table, and the difference is
 * the protocol's, not a preference:
 *
 *   FT8's table is keyed by CALLSIGN and merged, because its list answers "who
 *   is on frequency right now" and a station heard five times is still one row.
 *
 *   A WSPR store is a LOG. Spots arrive in a burst every two minutes and the
 *   same station heard in six consecutive cycles at six different SNRs is the
 *   entire point - that sequence IS the propagation measurement. Merging it
 *   away would delete the information the mode exists to produce.
 *
 * So this is a ring of individual spots, newest first, never merged.
 *
 * ⛔ The backing store lives in PSRAM and callers must NOT put a snapshot on
 * the stack - the same rule ft8_screen.h carries, for the same reason (see the
 * v0.20.1 pounce crash: an 11 KB stack array on an LVGL callback). Use
 * wspr_spots_count() when only the number is wanted, and copy at most a screen
 * full when rendering.
 */

#define WSPR_SPOT_CALL_MAX  11   /* WSPR type-1 callsigns, plus room for NUL */
#define WSPR_SPOT_GRID_MAX   5   /* 4-character Maidenhead field + NUL */
#define WSPR_SPOT_CTY_MAX    4   /* DXCC alpha-3, as the FT8 list uses */
#define WSPR_SNR_UNKNOWN     (-32768)  /* see snr_db below */
#define WSPR_DRIFT_UNKNOWN   (-32768)  /* likewise - 0 Hz is a REAL reading */
#define WSPR_SPOT_RING       256 /* ~8 cycles of a busy band, ~10 KB in PSRAM */

typedef struct {
    char     call[WSPR_SPOT_CALL_MAX];
    char     grid[WSPR_SPOT_GRID_MAX];
    char     cty[WSPR_SPOT_CTY_MAX];
    int64_t  cycle_utc;     /* start of the even minute this was heard in */
    float    freq_hz;       /* audio offset inside the 200 Hz window */
    /* WSPR_SNR_UNKNOWN until something actually measures it. NEVER a stand-in
     * value: this project deleted the ADIF "599" placeholder for exactly this
     * reason - an unmeasured number displayed as a measurement is a fabricated
     * one, and a missing field is honest where a wrong one is not. */
    int16_t  snr_db;
    /* WSPR_DRIFT_UNKNOWN until measured. Note 0 is a genuine and common value -
     * fifty stations reported drift 0 for our own transmission - so a default of
     * 0 would be indistinguishable from a real clean reading. That is precisely
     * why it needs a sentinel rather than a "harmless" zero. */
    int16_t  drift_hz;
    int16_t  power_dbm;     /* as REPORTED by that station - never inferred */
    int32_t  km;            /* -1 when the grid gives no answer */
    int16_t  bearing_deg;   /* -1 when unknown */
} wspr_spot_t;

void wspr_spots_init(void);

/* Add one spot. Called from the decode task, once per successful decode. */
void wspr_spots_add(const wspr_spot_t *spot);

/* Copy up to `max` spots, newest first, into `out`. Returns the number written.
 * Takes the mutex internally. */
int wspr_spots_get(wspr_spot_t *out, int max);

/* How many spots are held, without needing a snapshot buffer - so a caller on
 * taskLVGL that only wants a count never has to allocate one. */
int wspr_spots_count(void);

/* Distinct callsigns currently held. This is the number an operator actually
 * reads as "how am I doing" - 40 spots from 6 stations is a different night
 * from 40 spots from 40 stations. */
int wspr_spots_unique_calls(void);

void wspr_spots_clear(void);
