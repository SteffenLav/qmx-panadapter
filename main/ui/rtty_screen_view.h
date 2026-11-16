#pragma once
/* RTTY page - R006 (TODO-RTTY.md, docs/rtty-feasibility.md "R006 design").
 *
 * Same grid as the CW page: the panadapter spectrum and waterfall stay live
 * underneath, a 320 px pane on the left (CW_PANE_W - the spectrum mapping in
 * ui.c already narrows to it for any page with a left pane), and a 6-row
 * decoded-text area over the bottom of the waterfall, above the band-plan
 * strip. The pane carries the TX buttons (R010).
 *
 * Showing the page turns the decoder (rtty_rx) on; hiding it turns it off. */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

void      rtty_screen_view_init(lv_obj_t *parent);
void      rtty_screen_view_show(void);
void      rtty_screen_view_hide(void);
lv_obj_t *rtty_screen_view_get_container(void);
/* R030: screen y of the decoded-text box top. The log dialog stops its scrim
 * there, so the text stays tappable while it is open. */
int rtty_screen_view_text_top_y(void);
int rtty_screen_view_text_bottom_y(void);   /* ...and its bottom */

/* The waterfall area left visible by the pane and the text box (2 rects), for
 * ui.c's carpet shift; false while the page is not shown. See the CW page's
 * cw_screen_view_carpet_visible() for why. */
bool      rtty_screen_view_carpet_visible(lv_area_t a[2]);

/* A tap (Tab5) or click (web) on the spectrum/waterfall while this page is
 * up: the DIAL moves so the tapped frequency sits at RTTY_TAP_AUDIO_HZ, and
 * the decoder's tone centre goes on it. Any task (the web calls it). */
#define RTTY_TAP_AUDIO_HZ 1500
#define RTTY_TUNE_MIN_HZ  100000   /* below this a tap/keypad value is refused */
void      rtty_screen_view_tap_hz(uint32_t tapped_hz);

/* ⛔ ON THE RTTY PAGE "Freq:" IS THE MARK FREQUENCY, not the dial. Operator
 * 2026-11-14: "I want the dial/Freq to show what two stations would agree to
 * meet and have a qso on. So when they start the qso then the orange lines
 * match to two tones". RTTY frequencies (spots, logs, FSK radios) are quoted
 * as MARK. The QMX itself shows the dial, which is the USB carrier, 1.5 kHz
 * below a tapped signal.
 *
 * mark offset = decoder centre (+ AFC once a station has been decoded,
 * afc_held - the same rule rtty_tx uses for where it transmits) + shift/2,
 * - shift/2 when reversed. Any task. */
int       rtty_screen_view_mark_offset_hz(void);

/* The dial moved (knob, keypad, band, spot - any path). On the RTTY page the
 * decoder restarts, so AFC and acquisition search afresh round the lines, the
 * same as after a tap. No-op on other pages and while keyed. Any task. */
void      rtty_screen_view_dial_moved(void);

/* Tune so the MARK tone is at mark_hz (keypad, spot tap, web keypad on the
 * RTTY page): the centre goes to mark -/+ shift/2 by rtty_screen_view_tap_hz. */
void      rtty_screen_view_tune_mark_hz(uint32_t mark_hz);
