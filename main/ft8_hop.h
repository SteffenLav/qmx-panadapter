#pragma once
/* FT8/FT4 receive band hopping (Mark N8MH, 2026-11-25).
 *
 * While the FT8 page is idle, step through the bands the operator ticked.
 * Stay on each band for the set minutes (default 2 = 8 FT8 / 16 FT4 slots);
 * the hop is on a UTC multiple of that stay.
 * The behaviour follows WSJT-X improved; the controls follow our WSPR page.
 *
 * The hop runs on ft8_task, after a slot's capture has ended and before the
 * next boundary - the only gap there is. See ft8_hop_run().
 *
 * ft8_slot_dial*() is the per-slot dial latch. Every consumer that turns a
 * decode into a frequency (PSK Reporter, worked-before by band) must read the
 * dial THE SLOT WAS CAPTURED ON, not cat_get_frequency() at decode time: the
 * decode of the last slot before a hop finishes after the hop. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The band table the mask indexes: 160 80 60 40 30 20 17 15 12 10 6, the same
 * order as WSPR's, so the two masks read the same way. */
int         ft8_hop_band_count(void);
const char *ft8_hop_band_name(int i);           /* "40", NULL past the end */

/* Bit mask -> "20 40 15" (radio's bands only); "" when none. */
void ft8_hop_mask_text(uint16_t mask, char *buf, size_t len);

/* Allowed stay lengths in minutes, ascending (1 2 4 6 10 15 30). */
int  ft8_hop_dwell_choices(const uint8_t **out);
bool ft8_hop_dwell_valid(int min);

/* ---- per-slot dial latch -------------------------------------------------
 * Latched by ft8_task at each slot boundary. ft8_slot_dial() returns 0 for a
 * slot it does not know (older than the ring); callers then fall back to the
 * current dial, which is what they did before the latch existed. */
void     ft8_slot_dial_latch(int64_t slot_sec, uint32_t dial_hz);
/* The dial now: the hop's target for a few seconds after a hop (the FA poll
 * has not confirmed it yet), else cat_get_frequency(). For the latch. */
uint32_t ft8_hop_dial_now(void);

/* Before a key-down: if a hop is pending, wait until the radio's FA answer
 * names the hop's band, up to timeout_ms. True = no hop pending, or
 * confirmed; false = the radio did not confirm (do not key). Blocks. */
bool ft8_hop_wait_confirmed(int timeout_ms);
uint32_t ft8_slot_dial(int64_t slot_sec);

/* ---- the hop ----------------------------------------------------------------
 * ft8_hop_due(): the stay on this band is used up by next_boundary_ms AND
 * hopping is on AND nothing blocks it (a QSO, an armed non-CQ TX, ...). A hop
 * that was held stays due on every later boundary until it can run. ft8_task asks this before the
 * capture so it can force the early cut (13.2 s FT8, 5.6 s FT4), which is what
 * makes a gap to retune in.
 *
 * ft8_hop_run(): retunes to the next ticked band, if it is still due and at
 * least FT8_HOP_MIN_LEAD_MS remain before next_boundary_ms. Call after the
 * capture has finished. Returns true when it retuned. */
bool ft8_hop_due(int64_t next_boundary_ms);
bool ft8_hop_run(int64_t next_boundary_ms);

/* The hop at a boundary, before that slot's TX check and dial latch. Runs
 * only when the hop in the gap before this boundary did not (a CQ held for
 * the previous slot's decode starts up to 2.3 s late and ends too close to
 * the boundary). Without it that slot's CQ went out on the OLD band and the
 * stay grew by a slot (Sim 2026-11-26). The slot loses the retune time. */
bool ft8_hop_run_at_boundary(int64_t boundary_ms);

/* True when the radio was retuned to dial_hz's band by a hop in the last few
 * seconds. The band-change handler in ui.c asks this so a hop keeps the decode
 * list (with a separator) instead of clearing it. */
bool ft8_hop_owns_band_change(uint32_t dial_hz);

/* False when slot_sec was captured on another band than the one the radio is
 * on now: the last slot before a hop is decoded after the hop. Its decodes
 * must not start or advance a QSO, or the reply goes out on the wrong band.
 * True when the slot's dial is unknown. */
bool ft8_hop_slot_on_current_band(int64_t slot_sec);

/* The CQ split's plan for the next CQ. Returns -1: no say (split off or not
 * hopping; the TXCQ setting decides). 0: arm now with *parity (0 EVEN in the
 * 1st half of the stay, 1 ODD in the 2nd). 1: WAIT - the next slot of
 * *parity is in the wrong half; do not arm, ask again next slot. */
int ft8_hop_cq_plan(int *parity);

/* The period the split wants now, for display: -1, 0 EVEN, 1 ODD. */
int ft8_hop_cq_parity(void);

/* For the page and /api/status. hopping = mask has two or more of the radio's
 * bands. blocked = why it is not hopping right now, or NULL. next_band and
 * secs_to_hop are valid when hopping and not blocked. */
typedef struct {
    bool        hopping;
    const char *blocked;
    char        bands[48];      /* "20 40 15" */
    char        next_band[6];   /* "40" */
    int         secs_to_hop;
    int         dwell_min;      /* stay per band in force */
    int         cq_period;      /* ft8_hop_cq_parity(): -1, 0 EVEN, 1 ODD */
} ft8_hop_state_t;
void ft8_hop_get_state(ft8_hop_state_t *out);
