#pragma once
/* FT8/FT4 receive band hopping (Mark N8MH, 2026-11-25).
 *
 * While the FT8 page is idle, step through the bands the operator ticked:
 * 2 minutes per band (8 FT8 slots, 16 FT4 slots), hop on the even UTC minute.
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

/* ---- per-slot dial latch -------------------------------------------------
 * Latched by ft8_task at each slot boundary. ft8_slot_dial() returns 0 for a
 * slot it does not know (older than the ring); callers then fall back to the
 * current dial, which is what they did before the latch existed. */
void     ft8_slot_dial_latch(int64_t slot_sec, uint32_t dial_hz);
uint32_t ft8_slot_dial(int64_t slot_sec);

/* ---- the hop ----------------------------------------------------------------
 * ft8_hop_due(): the slot that ends at next_boundary_ms is the last of a 2 min
 * dwell AND hopping is on AND nothing blocks it. ft8_task asks this before the
 * capture so it can force the early cut (13.2 s FT8, 5.6 s FT4), which is what
 * makes a gap to retune in.
 *
 * ft8_hop_run(): retunes to the next ticked band, if it is still due and at
 * least FT8_HOP_MIN_LEAD_MS remain before next_boundary_ms. Call after the
 * capture has finished. Returns true when it retuned. */
bool ft8_hop_due(int64_t next_boundary_ms);
bool ft8_hop_run(int64_t next_boundary_ms);

/* True when the radio was retuned to dial_hz's band by a hop in the last few
 * seconds. The band-change handler in ui.c asks this so a hop keeps the decode
 * list (with a separator) instead of clearing it. */
bool ft8_hop_owns_band_change(uint32_t dial_hz);

/* For the page and /api/status. hopping = mask has two or more of the radio's
 * bands. blocked = why it is not hopping right now, or NULL. next_band and
 * secs_to_hop are valid when hopping and not blocked. */
typedef struct {
    bool        hopping;
    const char *blocked;
    char        bands[48];      /* "20 40 15" */
    char        next_band[6];   /* "40" */
    int         secs_to_hop;
} ft8_hop_state_t;
void ft8_hop_get_state(ft8_hop_state_t *out);
