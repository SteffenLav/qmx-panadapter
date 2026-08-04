#pragma once
#include <stdbool.h>
#include <stddef.h>

// CW ESM ("Enter Sends Message") ladder + callsign capture — Tier 2 of the
// CW page (docs/cw-page-design.md). One big NEXT button runs a standard
// exchange, N1MM-style: what it sends is decided by this state machine from
// what has been sent and what the QMX's decoder heard.
//
//   CQ    NEXT sends memory M1 (the CQ). Stays here, listening.
//   EXCH  a station was adopted: NEXT sends "<CALL> " + memory M3 (report).
//   TU    NEXT sends "<CALL> " + memory M4 (TU 73) and LOGS the QSO.
//
// Adoption: automatic when the decoder hears our own call followed by
// "DE <their call>" shortly after our CQ; manual any time by tapping the
// station card (pounce). Long-press NEXT resets to CQ.
//
// Threading: cw_qso_feed() runs on whatever context delivers decoded text
// (the CDC-ACM RX callback via cw_screen_feed_decoded) and only does string
// work under a spinlock — no LVGL, no blocking. Everything else is called
// from the LVGL thread.

typedef enum {
    CW_ESM_CQ = 0,    // calling / listening, no station adopted
    CW_ESM_EXCH,      // station adopted, report not yet sent
    CW_ESM_TU,        // report sent, TU+log pending
} cw_esm_state_t;

void cw_qso_init(void);

// Cache our own callsign for the capture heuristics (call from the page's
// show(); reads settings there so feed() never has to take the settings
// mutex on the CDC context).
void cw_qso_set_my_call(const char *call);

// Decoded characters, any task context (string work under a spinlock only).
void cw_qso_feed(const char *s, int n);

// Every transmitted text (memory button or NEXT) is reported here so the
// capture knows a CQ went out (arms auto-adoption).
void cw_qso_note_sent(const char *text);

cw_esm_state_t cw_qso_get_state(void);

// The station adopted as the QSO target ("" when none).
void cw_qso_get_target(char *out, size_t out_sz);

// The most recently captured plausible callsign that is not us and not the
// target — what the station card shows before adoption. ts_ms_out (optional)
// = how long ago it was heard.
bool cw_qso_get_heard(char *out, size_t out_sz, int *age_s_out);

// RST captured from the target after adoption ("" if never heard one —
// the log then omits RST_RCVD; never fabricated).
void cw_qso_get_rst_rcvd(char *out, size_t out_sz);

// Adopt a station (station-card tap / auto). Enters EXCH.
void cw_qso_adopt(const char *call);

// Drop the target and go back to CQ (long-press NEXT, or card tap on the
// already-adopted station).
void cw_qso_reset(void);

// Forget everything learned from the decode stream: the heard call, the
// copilot hint, and the timing windows that pair "our call" with "DE <them>".
// Used when leaving simulation mode so a phantom QSO leaves no trace.
void cw_qso_clear_capture(void);

// What NEXT would send right now (macro-UNexpanded except <CALL>, which is
// substituted here). Returns false when it cannot compose (e.g. no target
// mid-ladder). label_out gets a short state word for the UI ("CQ",
// "report", "TU + log").
bool cw_qso_next_text(char *out, size_t out_sz,
                      const char *label_out[]);

// NEXT was actually sent: advance the ladder. In TU this WRITES THE ADIF
// RECORD (mode CW, RST sent 599 — genuinely keyed — RST rcvd only if
// captured; freq 0 with no radio, which marks sim records deletable) and
// resets to CQ. Returns true if a QSO was logged (UI toasts).
bool cw_qso_on_next_sent(void);

// Phantom practice stations (rides the existing FT8 Simulation Mode
// checkbox): call at ~500 ms from the page's LVGL timer while sim mode is
// on. Injects scripted replies to whatever we "sent" via the same
// cw_screen_feed_decoded() path a real decode takes.
void cw_qso_sim_tick(void);

// Arm a phantom reply after a [SIM] send. Call BEFORE cw_qso_on_next_sent()
// (the pre-advance ladder state picks the reply script).
void cw_qso_sim_arm(void);

// ---- Copilot hints -----------------------------------------------------------
// Plain-language interpretation of the decode stream, for an operator who
// cannot yet copy CW by ear (docs/cw-page-design.md Tier 3, "CW copilot").
// The ticker shows WHAT was heard; the hint says WHAT IT MEANT. Classified in
// feed context (spinlock, no allocation), rendered by the LVGL tick.
typedef enum {
    CW_HINT_NONE = 0,
    CW_HINT_CALLING_CQ,   // a CQ went by, sender not yet identified
    CW_HINT_CQ_FROM,      // "<subj> is calling CQ" - CQ then DE <call>
    CW_HINT_ANSWERED_YOU, // your call + "DE <call>" - they are answering you
    CW_HINT_YOUR_CALL,    // your call heard, but no DE <call> yet
    CW_HINT_REPORT,       // an RST went by (subj carries it, e.g. "599")
    CW_HINT_SIGNOFF,      // 73 / TU - they are finishing
    CW_HINT_REPEAT,       // AGN / PSE - they want a repeat
    CW_HINT_QRZ,          // QRZ - they are asking who called
    CW_HINT_CALL_HEARD,   // a plausible callsign appeared
} cw_hint_t;

// Latest hint, its subject (a callsign, or the report for CW_HINT_REPORT;
// "" when not applicable) and how long ago it landed. False when there has
// been no hint yet. Hints do NOT expire here - the UI decides how long to
// show one (age_s_out is what it uses).
bool cw_qso_get_hint(cw_hint_t *kind_out, char *subj_out, size_t subj_sz,
                     int *age_s_out);

// One line of plain English for a hint. Never NULL; returns buf.
const char *cw_qso_hint_text(cw_hint_t kind, const char *subj,
                             char *buf, size_t buf_sz);

// Boot self-test: drives the tokenizer with canned decode text and checks the
// hint classifier, since the real path needs a radio and a finger on the
// screen. Logs one PASS/FAIL line per case and restores all state. Safe to
// call once at startup before the CW page is used.
void cw_qso_hint_selftest(void);
