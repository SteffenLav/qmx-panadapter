#pragma once
#include "lvgl.h"

/*
 * Diagrams for the on-device manual (TODO #96).
 *
 * WHY THIS EXISTS. The manual's pictures used to be drawn with UTF-8 box
 * characters. The Reader folds UTF-8 to ASCII before drawing, and some folds
 * CHANGE THE LENGTH of the line - an arrow becomes "(right)", one character
 * turning into seven - so a line the author had aligned overran its box and
 * wrapped. No font fixes that; monospace was tried and reverted. Beyond the
 * alignment, four of the drawings were found to be describing behaviour the
 * firmware no longer has, because nobody could read them well enough to notice.
 *
 * WHY NOT IMAGES. This Reader has no image decoder, and a bitmap would be
 * fixed-size, blind to the theme, and 20-45 KB each on top of a 223 KB manual.
 *
 * SO: a fenced ```qmxdiagram block carries a short SEMANTIC spec - what the
 * parts are, not where they go - and this module draws it with ordinary LVGL
 * widgets in the app's own colours. tools/pack_manual.py renders the same spec
 * to SVG for the website, so the two surfaces cannot drift apart.
 *
 * THE RULE THIS MODULE EXISTS TO ENFORCE: the renderer does the arithmetic.
 * Every placement bug in the mock-up phase (a diagram at half scale, a label
 * on top of its neighbour, a clock running into the text beside it) came from
 * a human placing things by hand in a fixed coordinate space. Flow and stack
 * diagrams are therefore laid out by LVGL's flex engine, which cannot overlap;
 * the timeline is the one type with computed coordinates, and it derives every
 * x from (time / span) rather than from a number anybody typed.
 */

/* Render `spec` into `parent` (the Reader's body column). Returns the created
 * container, or NULL if the spec was unusable - in which case the caller should
 * fall back to showing the raw text, so a malformed diagram degrades to
 * something readable instead of to nothing. */
lv_obj_t *reader_diagram_add(lv_obj_t *parent, const char *spec, lv_coord_t width);
