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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Longest text one TB; response can carry (the radio's buffer is 40).
#define CW_DECODE_MAX_CHUNK 64

// ---- noise squelch ---------------------------------------------------------
//
// With no signal the radio's decoder chews on noise and emits a stream of E and
// T - "TTTTTTTTETTTTTTT" - because those are the two SHORTEST Morse symbols (a
// single dit and a single dah), so a random threshold crossing is far more
// likely to land on one of them than on anything longer. It is worst just after
// switching to CW, while the decoder's own amplitude tracking is still settling.
//
// So: hold a run of E/T back rather than printing it, and decide once something
// else arrives. A SHORT run was real text and is released intact; a long one was
// noise and is dropped. Spaces neither start nor break a run - noise produces
// plenty of them - but any other character ends it.
//
// This cannot make a wrong decision permanently: at most CW_SQUELCH_RUN
// characters are ever held, and a genuine "EEEE" (unusual in real CW) costs
// nothing but those characters.
#define CW_SQUELCH_RUN 5      // E/T in a row before a run is called noise
#define CW_SQUELCH_HOLD 24    // characters held while deciding

typedef struct {
    char pend[CW_SQUELCH_HOLD];
    int  n_pend;   // held, waiting on a verdict
    int  run;      // consecutive E/T (may exceed CW_SQUELCH_RUN)
    int  emitted;  // anything released yet? - stops a leading space
} cw_squelch_t;

// Push one decoded character. Writes any characters that are now released to
// out and returns how many. PORTABLE; host-tested.
int cw_squelch_push(cw_squelch_t *st, char c, char *out, size_t out_sz);

// ---- speed estimate --------------------------------------------------------
//
// Words per minute from characters and elapsed time, on the PARIS convention of
// 5 characters to a word.
//
// ⚠ This is a THROUGHPUT figure, not the sender's keying speed. It counts the
// gaps between words and between overs, so it reads LOW during a real QSO and
// only approaches the true speed during continuous sending. A true keying speed
// needs element timing, and the radio hands us finished characters - the timing
// is gone by then. Returns 0 when there is not enough to say.
int cw_wpm_estimate(int chars, unsigned elapsed_ms);

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

// Total characters ever decoded (after squelch). A cheap change-detector for a
// UI that only wants to repaint when something arrived.
unsigned cw_decode_total(void);

// ---- the one-line display ---------------------------------------------------
//
// A fixed grid of columns that WRAPS and overwrites itself, the way the QMX's
// own scroll line does, rather than scrolling text leftwards. Nothing moves, so
// a callsign you are half-way through reading stays where it is; only the
// oldest end is replaced.
//
// ⭐ It lives HERE rather than in the UI because BOTH screens draw it - the
// Tab5's panadapter and the browser's. Two copies of "where does the next
// character go" would drift the moment one of them changed, and the operator
// asked for them to be identical.
//
// 71 columns is what the Tab5 has left after the green "CW ~08 wpm:" header at
// 15 px per character across 1280 px. The browser uses the same number so the
// two wrap in the same places.
#define CW_LINE_COLS 71

// Copy the line as it should be DRAWN: the grid, with two blank columns laid
// over it at the write position. After the line has wrapped that gap is the
// only thing separating what has just been decoded from what is about to be
// overwritten. Always NUL-terminated; returns the number of columns written.
size_t cw_decode_line(char *out, size_t out_sz);

// Estimated speed of what is being received, words per minute, over the last
// few tens of seconds. 0 means "not enough to say" - which is the honest answer
// on a quiet band and is what the UI should show rather than a stale number.
int cw_decode_wpm(void);

// Forget everything (mode change, or the operator clearing the window).
void cw_decode_clear(void);

// Hand over the decoded text waiting to be written to the microSD transcript
// (#323), removing what it returns. Already formatted - timestamped lines with
// CRLF - so the caller only appends bytes and never has to know how a line is
// shaped. Returns 0 when there is nothing pending, which is the normal case on
// a quiet band. Called by sd_archive.c, which owns every SD write.
size_t cw_decode_take_pending(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
