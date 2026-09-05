// Decoded CW text from the QMX's OWN decoder, delivered over CAT.
//
// The radio already decodes CW in its microcontroller and will hand the text to
// a CAT host on request, so this costs the Tab5 no DSP at all - one more slot in
// a poll rotation that is already running. That matters here specifically:
// core 0 is the wall on this board (taskLVGL alone is 73.9% of it), and every
// previous attempt at doing audio work for CW ourselves ran into it. Suggested
// by Uwe DL8UG, who had already built it for himself.
//
// ⭐ `TB` is in the 1_03 CAT manual as well as 1_04, so this needs NO
// cat_qmx_fw_at_least() gate - unlike AM and SWR Tune. It works on the firmware
// people are already running.
//
// ⛔ THE RADIO'S BUFFER IS 40 CHARACTERS AND IT IS NOT CIRCULAR. The CAT manual
// is explicit: "When it fills up, it simply discards any new incoming
// characters." So TB; has to be polled at a steady cadence rather than read on
// demand - at 20 WPM the radio fills it in roughly 24 seconds, and text lost
// there is lost silently, with nothing in the response to say it happened.
//
// We do NOT enable the decoder over CAT. Its "Enable Rx"/"Enable Tx" parameters
// default to YES in the radio's own Decoder menu, so it is normally already on,
// and writing an MM path we have not verified against a radio is exactly the
// guessing CLAUDE.md keeps warning about.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Longest text one TB; response can carry (the radio's buffer is 40).
#define CW_DECODE_MAX_CHUNK 64

// PORTABLE, no ESP dependencies, host-tested by test/cw_decode_harness.c.
//
// Parse a "TBtnns;" response. Writes the decoded characters to out (always
// NUL-terminated) and, if tx_pending is non-NULL, the 't' field: 0 means the
// radio is receiving, 1-9 means that many characters of a KY send remain.
//
// Returns the number of characters written, or -1 if resp is not a well-formed
// TB response. A well-formed response carrying no text returns 0 - that is the
// normal idle case, not an error.
int cw_decode_parse_tb(const char *resp, char *out, size_t out_sz, int *tx_pending);

// ---- the running text (ESP side) -------------------------------------------

// Allocate the scrollback ring. Safe to call more than once.
void cw_decode_init(void);

// Append one TB; response. Ignores a malformed one rather than polluting the
// text with it. Safe from the CAT poll task.
void cw_decode_feed(const char *resp);

// Copy the most recent characters into out (NUL-terminated) - what the
// panadapter's one-line strip shows. Returns the number of characters copied.
size_t cw_decode_tail(char *out, size_t out_sz);

// Copy the whole scrollback, oldest first - what the CW page shows.
size_t cw_decode_snapshot(char *out, size_t out_sz);

// Total characters ever decoded. A cheap change-detector for a UI that only
// wants to repaint when something arrived.
unsigned cw_decode_total(void);

// Forget everything (mode change, or the operator clearing the window).
void cw_decode_clear(void);

#ifdef __cplusplus
}
#endif
