#pragma once
/* The band-hop picker: a full-screen window of finger-sized band checkboxes.
 * One implementation for the WSPR page and the FT8/FT4 page (moved out of
 * wspr_screen_view.c on 2026-11-25 when FT8 hopping arrived - a second copy
 * would have been the drift this project keeps recording).
 *
 * The caller passes the bands to OFFER (already filtered to the radio, built
 * when the window opens - CAT answers ~17 s after boot, so a list built at
 * init offered every band on every radio), the bit each band sets, the current
 * mask, and two callbacks. Parents to lv_layer_top(), so it opens from the
 * drawer with the page hidden. */
#include <stdint.h>

typedef struct {
    const char *name;   /* "40" */
    uint8_t     bit;    /* bit in the caller's mask */
} band_hop_choice_t;

typedef void (*band_hop_mask_cb_t)(uint16_t mask);

/* `hint` is the explanation under the title. `changed` runs on every tick
 * with the new mask; `closed` (may be NULL) runs when the window closes. */
void band_hop_picker_open(const char *title, const char *hint,
                          const band_hop_choice_t *choices, int n,
                          uint16_t mask,
                          band_hop_mask_cb_t changed, void (*closed)(void));
