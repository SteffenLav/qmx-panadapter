#pragma once
#include "lvgl.h"
#include <stdint.h>

/* The WSPR page: a sibling screen to Panadapter and FT8, shown while
 * ui_mode == UI_MODE_WSPR. Design and its reasoning are in
 * docs/wspr-ui-design.md - in particular why this looks different from the FT8
 * screen rather than the same, which is the protocol's doing and not a
 * preference:
 *
 *   - the rhythm is TWO MINUTES, so the countdown orients rather than urges
 *   - it is a one-way beacon, so there is no QSO furniture at all
 *   - the list is a LOG grouped by cycle, not a live "who is on frequency now"
 *   - there is no MESSAGE column, because there is no message
 */

void      wspr_screen_view_init(lv_obj_t *parent);
void      wspr_screen_view_show(void);
void      wspr_screen_view_hide(void);
lv_obj_t *wspr_screen_view_get_container(void);

// Called from the 1 Hz UI tick while the page is up: refreshes the countdown,
// the status line and (only when it has changed) the spot list.
void      wspr_screen_view_tick(void);

/* ---- THE STANDARD WSPR DIALS, AND WHICH OF THEM THIS RADIO HAS ------------
 *
 * The dial frequencies are a property of the PROTOCOL - every band has exactly
 * one canonical WSPR sub-band and anything else is simply not where anyone is
 * listening. Which bands are REACHABLE is a property of this particular radio:
 * a QMX is built with a fixed set of band-pass filters, and the one on this
 * bench reports 60/40/30/20/17/15 while a QMX+ covers 160-6.
 *
 * So the table is fixed and the AVAILABILITY is asked of the radio
 * (cat_get_band_list). Offering a band the hardware cannot filter would be
 * offering a dial that receives nothing.
 */
typedef struct {
    const char *name;      /* "20" - matches what the QMX reports over CAT */
    const char *label;     /* "20 m  14.095600" - what the picker shows */
    uint32_t    dial_hz;
} wspr_band_t;

const wspr_band_t *wspr_bands(int *out_count);

/* "20", "30"... for a dial frequency, or NULL if it matches no WSPR band.
 * NULL rather than a guess: a spot recorded before dial_hz existed has 0 here,
 * and a blank is honest where "160" would be invented (Roy KI0ER, 2026-08-31 -
 * with band hopping on there was no way to tell which band a spot came from). */
const char *wspr_band_name_for_dial(uint32_t dial_hz);

/* Indices into wspr_bands() that THIS radio can actually reach, in table order.
 * Returns the count. With no CAT band list yet (radio off, or not polled) it
 * returns every band - a picker that is empty because the radio has not
 * answered yet is worse than one that offers too much. */
int wspr_bands_available(uint8_t *out, int max);

/* Open the band-hop picker. Exported because the control that opens it now
 * lives in the settings drawer rather than on this page; the modal parents to
 * lv_layer_top(), so it does not care whether this page is visible. */
void wspr_screen_view_open_hop_picker(void);

/* The legal WSPR duty-cycle values, shared with the settings drawer so the two
 * places that offer them cannot drift apart. WSPR asks "what fraction of cycles
 * may I transmit", and these are the answers - see docs/wspr-ui-design.md. */
extern const uint8_t kDuty[];
#define WSPR_N_DUTY 5
