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
