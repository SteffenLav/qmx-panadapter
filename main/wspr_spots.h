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
    /* ⭐ THE DIAL THIS SPOT WAS HEARD ON. freq_hz above is only the audio offset
     * inside the 200 Hz window, so with band hopping on there was no way at all
     * to tell which band a spot came from - Roy KI0ER, 2026-08-31: "there's no
     * good way to know what band a particular spot came from."
     *
     * Not a band LABEL: the dial is the honest record and a label derives from
     * it, whereas "20m" throws away which WSPR sub-band it was and cannot be
     * re-derived. 0 means unknown - spots already in the ring when this landed
     * have no dial, and a blank is honest where a guess is not. */
    uint32_t dial_hz;
    int32_t  km;            /* -1 when the grid gives no answer */
    int16_t  bearing_deg;   /* -1 when unknown */
    /* Set once this spot has been accepted by wsprnet. Lives in the ring
     * rather than in the uploader because eligibility is a property of the
     * RING: a spot becomes publishable when its callsign is heard a SECOND
     * time, which can happen cycles after the spot itself arrived. A simple
     * "everything newer than X" cursor cannot express that. */
    uint8_t  sent;
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

/* Spots ever ADDED, monotonic. ⛔ Use this, not wspr_spots_count(), to decide
 * whether a view needs repainting: the count saturates at WSPR_SPOT_RING and
 * then never changes again, which froze the Tab5 decode list for five hours of
 * an overnight run while the receiver was working perfectly. */
uint32_t wspr_spots_seq(void);

/* Distinct callsigns currently held. This is the number an operator actually
 * reads as "how am I doing" - 40 spots from 6 stations is a different night
 * from 40 spots from 40 stations. */
int wspr_spots_unique_calls(void);

/* The furthest station held, by km. Returns 0 if nothing has a distance yet.
 *
 * ⛔ AN ACCESSOR, NOT A SNAPSHOT, ON PURPOSE. The ring is 256 spots of 40
 * bytes; a caller on taskLVGL that copied it to find one maximum would put
 * 10 KB on a stack CLAUDE.md keeps a list of crashes for (the v0.20.1 pounce
 * crash was an 11 KB array on exactly that task). This walks under the mutex
 * and returns one struct. Same reasoning as wspr_spots_unique_calls(). */
int wspr_spots_best_dx(wspr_spot_t *out);

/* Distinct callsigns heard MORE THAN ONCE. A real station transmits again and a
 * false decode does not, so this is the population that is safe to publish -
 * see the repeat-test argument in docs/wspr-phase3-sensitivity.md and the
 * wsprnet rule that no upload ships without it. */
int wspr_spots_repeat_calls(void);

void wspr_spots_clear(void);

/* ---- wsprnet upload support ------------------------------------------
 *
 * Spots that are PUBLISHABLE and not yet sent, newest first. The rule, agreed
 * before any of this was written: a callsign must have been heard MORE THAN
 * ONCE. A real station transmits again; a false decode does not. WSPR has no
 * CRC, so repetition is the only confirmation available that needs neither a
 * second decoder nor the internet - and wsprnet is a scientific dataset that
 * other people draw propagation conclusions from, so publishing an unconfirmed
 * decode is not a private mistake.
 *
 * Does NOT mark anything: a spot is only marked once the server has accepted
 * it, so a failed upload is retried rather than silently dropped. */
int wspr_spots_pending_upload(wspr_spot_t *out, int max);

/* Mark one spot sent, identified the way the uploader knows it. */
void wspr_spots_mark_sent(int64_t cycle_utc, const char *call);

