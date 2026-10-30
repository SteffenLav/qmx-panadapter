#pragma once

/* The JS8 submode, in ONE place.
 *
 * ⛔ WHY THIS FILE EXISTS. The submode had three controls - the settings
 * drawer (ui.c), /api/cmd (webserver.c) and, from 2026-10-15, the JS8 screen's
 * own picker - and each carried its own copy of the value list and of the
 * "refused while busy" test. Three copies of a rule is how two surfaces end up
 * disagreeing about whether a change was allowed, which is the same class of
 * defect as the web Pause button that could only write and never read.
 *
 * ⛔ IT IS NOT A QUALITY KNOB. Each submode is a different physical layer -
 * slot period, symbol rate, tone spacing and sync arrays all change - so a
 * station on Fast cannot hear a station on Normal at all. It is persisted in
 * NVS, so a wrong one survives every reboot and power cycle; Ryan KK6DZB lost
 * JS8 entirely to that and the screen still read "MODE: JS8".
 *
 * ⚠ Fast, Turbo and Slow have never been exchanged with a real JS8Call
 * station. They round-trip through our own encoder and decoder
 * (test/js8_submode_harness.c) and nothing more than that. Normal is what
 * everyone runs.
 */

#include <stdbool.h>
#include <stdint.h>

/* The options, in the order every picker shows them.
 *
 * ⛔ THE INDEX IS NOT THE VALUE. JS8Call's own NSUBMODE numbering is not
 * dense - 0 Normal, 1 Fast, 2 Turbo, 4 Slow; 3 does not exist and 8 is Ultra,
 * which this firmware does not implement - so the two are deliberately kept
 * apart rather than one being computed from the other. */
int         js8_submode_count(void);
uint8_t     js8_submode_value(int index);        /* 0 for an index out of range */
int         js8_submode_index_of(uint8_t value); /* 0 (Normal) when unknown     */

/* "Normal - 15 s" and so on. The cycle length is in the label because that is
 * the number an operator can act on. Also used for the drawer's single
 * newline-separated option string (js8_submode_options_lvgl). */
const char *js8_submode_label(int index);

/* The LVGL dropdown option string: every label, '\n' separated, built once. */
const char *js8_submode_options_lvgl(void);

/* ---- T132: the ALTERNATE sub-mode picker --------------------------------
 *
 * The same table, offered as "what ELSE to decode" - so "Off" first and
 * Normal left out, because Normal as an alternate under a Normal primary is
 * the thing we already decode. The value is still JS8Call's NSUBMODE, plus
 * 255 for off (JS8_ALT_OFF in js8_alt.h).
 *
 * ⛔ STILL ONE TABLE. Adding a sub-mode means adding a row in js8_submode.c
 * and nothing else; these four functions are built from it.
 *
 * ⚠ The alternate decode is RECEIVE ONLY and it is never refused - it does
 * not touch the physical layer we transmit on, so there is no busy rule here
 * to share with js8_submode_set(). See js8_alt.h. */
int         js8_alt_count(void);
uint8_t     js8_alt_value(int index);        /* 255 (off) for index 0 or out of range */
int         js8_alt_index_of(uint8_t value); /* 0 (off) when unknown                  */
const char *js8_alt_options_lvgl(void);

/* Set it, with the one refusal rule.
 *
 * Refused while a QSO or a burst is live - ARMED counts, not just keyed,
 * because an armed request already carries tones built for the OLD submode.
 * Returns false and points *why at a short reason suitable for a toast or an
 * API error; *why is a string literal and is never NULL on refusal.
 *
 * On success the slot loop picks the change up through
 * ft8_op_mode_protocol() and rebuilds the monitor pool at the top of the next
 * slot, and the Tab5's MODE label is repainted - there is nothing else for a
 * caller to do. */
bool js8_submode_set(uint8_t value, const char **why);
