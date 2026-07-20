// v0.13.0: FT8 QSO state machine - auto search-and-pounce + CQ-run.
//
// Two roles, one machine. The third field of a standard FT8 message decides
// the flow:
//
//   POUNCE (we answered their CQ)               CQ-RUN (they answered our CQ)
//   ------------------------------------        --------------------------------
//   TX1 <them> <me> <my_grid>                   CQ  CQ <me> <my_grid>
//   RX  <me> <them> <report>                    RX  <me> <them> <their_grid|rpt>
//   TX2 <them> <me> R<report>                   TX  <them> <me> <report>
//   RX  <me> <them> RR73|73                     RX  <me> <them> R<report>
//   TX3 <them> <me> 73                          TX  <them> <me> RR73
//
// Patience: the "current outgoing message" (s_cur_req) is re-armed every TX
// slot - by ft8_qso_on_tx_complete() right after each burst - until either the
// expected reply is heard (progress: swap s_cur_req, reset the miss counter) or
// QSO_TIMEOUT_SLOTS consecutive RX slots pass with no progress. So we keep
// pushing the same call for several cycles rather than going silent after one
// transmission. On timeout a CQ-originated QSO falls back to calling CQ again;
// a pounce QSO goes to TIMEOUT (sticky).
//
// Arming is deferred: ft8_qso_advance() runs in the decode task ~4 s into the
// *next* slot, which is usually while our re-armed burst is already ACTIVE on
// air - so advance() only updates state + s_cur_req, and on_tx_complete() (or
// the idle fallback) does the actual ft8_tx_arm(). This avoids the
// "arm refused, burst already in progress" race that previously left CQ stuck.

#include "ft8_qso.h"
#include "ft8_pileup.h"
#include "ft8_tx.h"
#include "ft8_test.h"   // ft8_op_mode_get() - FT8/FT4 sub-mode, for ADIF MODE
#include "ft8_status.h"
#include "ui/ft8_screen.h"
#include "storage/settings.h"
#include "adif/adif_log.h"
#include "cat/cat.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_qso";

// How many consecutive RX slots with no expected reply before we give up on
// the station we're working. ~6 cycles of patience (the user can wander off
// and come back, fading, QRM, etc.). Bumped from 4 after a logged near-miss
// (2026-06-26): an OH5KNL RR73 landed just 29s after a 4-slot timeout fired -
// they needed one more cycle to successfully copy our report and reply.
#define QSO_TIMEOUT_SLOTS  6

// Clamp the SNR we report to a sane FT8 range.
#define RPT_MIN_DB  (-24)
#define RPT_MAX_DB  (+15)

// ARRL Field Day exchange text, max len "32F WCF" + "R " prefix + NUL.
#define FT8_FD_EXCH_LEN 20

static SemaphoreHandle_t  s_lock;
static ft8_qso_state_t    s_state          = FT8_QSO_IDLE;
static char               s_target[FT8_CALL_MAX_LEN];   // their callsign
static char               s_my_call[FT8_CALL_MAX_LEN];  // our callsign (uppercased)
static int                s_freq_hz;                    // partner AF tone for our replies
static int64_t            s_min_scan_utc;               // pounce: don't scan before TX1 fires
static int                s_missed_slots;
static bool               s_from_cq;                    // session started as CQ-run
static ft8_tx_request_t   s_cur_req;                    // message we're currently sending
static bool               s_have_cur;                   // s_cur_req valid
static ft8_tx_request_t   s_cq_saved;                   // original CQ, to resume after a dropped QSO
static bool               s_have_cq_saved;
// Signal reports captured during the exchange for ADIF logging.
// Pounce: rst_rcvd = what they told us; rst_sent = our own locally-measured SNR
//   of them (the protocol never has us transmit a numeric report of them - TX2
//   just rogers their report back - so this is synthesized like WSJT-X does).
// CQ-run: rst_sent = our report of their signal; rst_rcvd = the value in their
//   numeric roger "R<rpt>" (or a direct report answer). The R-report is their
//   own measurement of OUR signal, NOT an echo of the report we sent - proven
//   on air 2026-07-15 (we sent -08, OS4K rogered R-06). "599" is only the
//   fallback if the exchange completes without us ever hearing a numeric value
//   (e.g. they jump straight to RR73 after a grid answer).
static char               s_rst_sent[8];
static char               s_rst_rcvd[8];
// ARRL Field Day: their class+section, captured when their roger/exchange
// message is recognized (WAIT_RPT pounce path, WAIT_ROGER cqrun path). Empty
// when the QSO isn't a Field Day exchange.
static char               s_fd_their_exch[FT8_FD_EXCH_LEN];

// ---------------------------------------------------------------------------

// Classify an already-truncated-to-whole-seconds slot_sec (e.g. a heard
// station's ft8_call_t.last_utc). FT4's 7.5 s grid needs a different formula
// from FT8's plain "/15" - see the long derivation at ft8_tx.c's own
// slot_is_even() (the two are intentionally identical; kept as separate
// static copies rather than shared across translation units, same as before
// this fix). Briefly: floor(k*7.5) mod 15 is always exactly 0 (k even) or 7
// (k odd), so the truncation loses no parity information.
static inline bool slot_is_even(int64_t sec, ftx_protocol_t proto)
{
    if (proto == FTX_PROTOCOL_FT4) return (sec % 15) == 0;
    return ((sec / 15) % 2) == 0;
}

// UTC slot_sec (matching exactly what ft8_test.c's slot loop will itself
// produce as boundary_ms/1000) of the next slot boundary strictly in the
// future, optionally constrained to a parity. Works in milliseconds
// internally using proto's real period (15000 FT8 / 7500 FT4) so the result
// is always a boundary the engine actually produces - unlike naively
// stepping by a literal 15 in whole seconds, which for FT4 doesn't track the
// real 7.5 s grid at all (the bug this replaces: ft8_qso_start()'s old
// tx1_slot search could pick a "slot" that no real FT4 boundary ever lands
// on, or mis-classify a boundary it did land on, so a parity-restricted
// pounce/CQ-lock could silently target a slot that would never actually
// fire or be scanned).
static int64_t next_slot_sec(bool match_parity, bool want_even, ftx_protocol_t proto)
{
    int period_ms = (proto == FTX_PROTOCOL_FT4) ? 7500 : 15000;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms  = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t next_ms = (now_ms / period_ms) * period_ms + period_ms;
    if (match_parity) {
        while ((((next_ms / period_ms) % 2) == 0) != want_even) next_ms += period_ms;
    }
    return next_ms / 1000;
}

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

// Cache + uppercase the operator callsign for message scanning. Returns false
// (with err) if no callsign is configured.
static bool load_my_call(char *err, size_t err_len)
{
    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.my_callsign[0]) {
        if (err) snprintf(err, err_len, "Set your callsign first (Settings)");
        return false;
    }
    size_t ci;
    for (ci = 0; ci < sizeof(s_my_call) - 1 && s.my_callsign[ci]; ci++) {
        char c = s.my_callsign[ci];
        s_my_call[ci] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    s_my_call[ci] = '\0';
    return true;
}

// "R-08" / "R+02" / "RRR" - they rogered our report (vs. repeating their grid,
// which can also begin with 'R' for far-east locators like RE78). Also covers
// the ARRL Field Day roger form "R 16A EMA" (R followed by a space then their
// class+section) - that combination of leading R + space never occurs in a
// plain grid/report token, so no fd_mode gate is needed.
static bool is_roger_token(const char *t)
{
    if (t[0] != 'R') return false;
    char c = t[1];
    return c == '-' || c == '+' || c == 'R' || c == ' ' || (c >= '0' && c <= '9');
}

// Strip the <angle brackets> ft8_lib puts around a hash-resolved callsign
// ("<KN6LFB>" -> "KN6LFB") so token matching works on the bare call. An
// unresolved hash decodes as "<...>" and becomes "...", which can never
// match a real callsign - correct (we don't know who it is).
static void strip_hash_brackets(char *tok)
{
    size_t n = strlen(tok);
    if (n >= 2 && tok[0] == '<' && tok[n - 1] == '>') {
        memmove(tok, tok + 1, n - 2);
        tok[n - 2] = '\0';
    }
}

// Split a decoded message into its first two tokens (callsigns) and "rest"
// (everything after, trimmed of leading spaces, verbatim - may itself
// contain spaces, e.g. an ARRL Field Day "R 16A EMA" exchange). Replaces a
// fixed 3-token sscanf, which truncated multi-word third fields.
// Hash brackets are stripped from both callsign tokens (see above).
static bool split_msg3(const char *text, char *tok1, size_t cap1,
                       char *tok2, size_t cap2, char *rest, size_t cap_rest)
{
    const char *p = text;
    while (*p == ' ') p++;
    const char *s1 = p;
    while (*p && *p != ' ') p++;
    size_t l1 = (size_t)(p - s1);
    if (l1 == 0 || l1 >= cap1) return false;
    memcpy(tok1, s1, l1); tok1[l1] = '\0';

    while (*p == ' ') p++;
    const char *s2 = p;
    while (*p && *p != ' ') p++;
    size_t l2 = (size_t)(p - s2);
    if (l2 == 0 || l2 >= cap2) return false;
    memcpy(tok2, s2, l2); tok2[l2] = '\0';

    while (*p == ' ') p++;
    snprintf(rest, cap_rest, "%s", p);

    strip_hash_brackets(tok1);
    strip_hash_brackets(tok2);
    return true;
}

// Format a coarse SNR into an FT8 report token: "-07", "+02", "-15".
static void fmt_report(int snr_db, char *out, size_t len)
{
    if (snr_db < RPT_MIN_DB) snr_db = RPT_MIN_DB;
    if (snr_db > RPT_MAX_DB) snr_db = RPT_MAX_DB;
    snprintf(out, len, "%+03d", snr_db);
}

// Public wrapper so UI code (ft8_pileup_modal.c) formats reports with the
// exact same clamping/format convention instead of growing another private
// copy (ft8_sim.c already has one - don't add a third).
void ft8_qso_fmt_report(int snr_db, char *out, size_t len)
{
    fmt_report(snr_db, out, len);
}

// Build "R<report>" for TX2. Their report is e.g. "-10"; pass through if it
// already starts with 'R'.
static void make_roger(const char *their_report, char *out, size_t len)
{
    if (their_report[0] == 'R') snprintf(out, len, "%s", their_report);
    else                        snprintf(out, len, "R%s", their_report);
}

// --- DT-follow-partner (operator request 2026-07-19) -----------------------
// When a QSO partner transmits significantly off the band's timing (a weak,
// badly-clocked or deliberately-offset station), shift OUR TX to land on THEIR
// beat (ft8_tx_set_follow_offset_ms) so they copy us better than a UTC burst
// sitting at the edge of their decode window. The partner's raw slot DT minus
// the band consensus (ft8_get_last_timing_ms) gives their true offset from the
// band, and crucially cancels our common ~560 ms RX audio latency. Engaged/
// updated each time we hear them, reset to 0 the moment the QSO ends. Works both
// directions (we called them, or they answered our CQ). Threshold = operator's.
#define DT_FOLLOW_THRESHOLD_MS 200

static void clear_dt_follow(void)
{
    if (ft8_tx_get_follow_offset_ms() != 0) {
        ft8_tx_set_follow_offset_ms(0);
        ESP_LOGI(TAG, "DT-follow: back to UTC/GPS beat");
    }
}

static void update_dt_follow(const char *target)
{
    if (!target || !target[0]) return;
    int consensus;
    if (!ft8_get_last_timing_ms(&consensus)) return;   // no band reference yet

    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, target) != 0) continue;
        int excess = (int)snap[i].last_dt_ms - consensus;   // partner's offset from the band
        if (excess > DT_FOLLOW_THRESHOLD_MS || excess < -DT_FOLLOW_THRESHOLD_MS) {
            if (ft8_tx_get_follow_offset_ms() != excess) {
                ft8_tx_set_follow_offset_ms(excess);
                ft8_status_set("DT-follow %s: TX %+d ms", target, excess);
                ESP_LOGI(TAG, "DT-follow %s: partner_dt=%d consensus=%d -> TX %+d ms",
                         target, (int)snap[i].last_dt_ms, consensus, excess);
            }
        } else {
            clear_dt_follow();   // partner is on the band's beat -> normal
        }
        return;
    }
    // target not in this snapshot -> keep the current offset; re-hear updates it.
}

// Scan the ft8_screen table for a message FROM s_target TO s_my_call decoded
// in slot_sec. Fills one of report_buf / *got_rr73 / *got_73, and *snr_db_out
// with OUR locally-measured SNR of their signal (independent of any numeric
// report token in the text) - this is the pounce-side equivalent of the SNR
// cqrun_answer() already captures for CQ-run, used as RST_SENT.
static bool scan_for_response(int64_t slot_sec,
                              char *report_buf, size_t report_cap,
                              bool *got_rr73, bool *got_73, int *snr_db_out)
{
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, s_target) != 0) continue;
        if (snap[i].last_utc != slot_sec) continue;

        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue; // not addressed to us
        if (strcmp(tok2, s_target)  != 0) continue; // not from them

        if (snr_db_out) *snr_db_out = snap[i].last_snr_db;
        if (strcmp(rest, "RR73") == 0) { *got_rr73 = true; return true; }
        if (strcmp(rest, "73")   == 0) { *got_73   = true; return true; }
        if (rest[0] != '\0') {
            snprintf(report_buf, report_cap, "%s", rest);
            return true;
        }
    }
    return false;
}

// Returns true if `text` (a decoded message, e.g. "CQ POTA OZ1LAV JO45" or
// "OZ1LAV W9XYZ -05") passes the CQ-run filter settings: matched against the
// whole message so POTA/SOTA tags, grids, country prefixes etc. are usable,
// not just the callsign.
// Each filter field can hold multiple space- and/or comma-separated terms
// (e.g. "POTA SOTA" or "JA, VK"); matches if `text` contains ANY of them.
static bool ft8_filter_contains_any(const char *text, const char *terms)
{
    char buf[FT8_FILTER_TEXT_LEN];
    strncpy(buf, terms, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, " ,");
    while (tok) {
        if (tok[0] && strstr(text, tok)) return true;
        tok = strtok(NULL, " ,");
    }
    return false;
}

bool ft8_filter_match(const char *text, const ft8_filters_t *f)
{
    bool incl_any_en = f->incl_en[0] || f->incl_en[1];
    if (incl_any_en) {
        bool ok = false;
        for (int i = 0; i < 2; i++) {
            if (f->incl_en[i] && f->incl_text[i][0] && ft8_filter_contains_any(text, f->incl_text[i])) { ok = true; break; }
        }
        if (!ok) return false;
    }
    for (int i = 0; i < 2; i++) {
        if (f->excl_en[i] && f->excl_text[i][0] && ft8_filter_contains_any(text, f->excl_text[i])) return false;
    }
    return true;
}

// Scan for any message addressed TO s_my_call in slot_sec - a reply to our CQ.
// Picks the best-SNR caller when several answer at once. Fills caller / freq /
// snr and one of report_buf / *got_rr73 / *got_73.
static bool scan_for_reply_to_me(int64_t slot_sec,
                                 char *caller_buf, size_t caller_cap,
                                 int  *caller_freq_out,
                                 int  *caller_snr_out,
                                 char *report_buf,  size_t report_cap,
                                 bool *got_rr73, bool *got_73)
{
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    qmx_settings_t qs;
    settings_load_all(&qs);

    int     best_idx = -1;
    int16_t best_snr = INT16_MIN;

    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec) continue;
        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;  // not addressed to us
        if (strcmp(tok2, s_my_call) == 0) continue;  // avoid MYCALL MYCALL loops
        // Empty third field is ACCEPTED: a nonstandard-callsign answer
        // ("<MYCALL> PJ4/K1ABC", i3=4) has no room for a grid - it's still a
        // real answer to our CQ. Standard messages always carry a third
        // field, so this only ever admits the nonstd case. cqrun_answer()
        // doesn't need `rest` (it sends OUR measured report either way).
        if (!ft8_filter_match(snap[i].last_text, &qs.ft8_filters)) continue;
        // Worked-before: tok2 is the answering station's callsign. If we've
        // logged them already ON THIS BAND and the operator enabled the filter,
        // ignore their answer so the CQ keeps running for someone new (same rule
        // the robot applies to CQ callers). Band-aware: a new band is a new slot.
        if (qs.ft8_filters.excl_worked_before &&
            adif_log_contains_call_on_band(tok2, cat_get_frequency())) continue;
        if (snap[i].last_snr_db > best_snr) {
            best_snr = snap[i].last_snr_db;
            best_idx = i;
        }
    }

    if (best_idx < 0) return false;

    char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
    split_msg3(snap[best_idx].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest));
    snprintf(caller_buf, caller_cap, "%s", tok2);
    if (caller_freq_out) *caller_freq_out = snap[best_idx].last_freq;
    if (caller_snr_out)  *caller_snr_out  = snap[best_idx].last_snr_db;
    if (strcmp(rest, "RR73") == 0) { *got_rr73 = true; return true; }
    if (strcmp(rest, "73")   == 0) { *got_73   = true; return true; }
    snprintf(report_buf, report_cap, "%s", rest);
    return true;
}

// ---------------------------------------------------------------------------
// Outgoing-message bookkeeping
// ---------------------------------------------------------------------------

// Make req the current outgoing message and move to st. Resets the miss
// counter (this is only ever called on progress / start). Arming itself is
// deferred to on_tx_complete() / the idle fallback.
static void set_current(const ft8_tx_request_t *req, ft8_qso_state_t st)
{
    lock();
    if (req) { s_cur_req = *req; s_have_cur = true; }
    s_state        = st;
    s_missed_slots = 0;
    unlock();
}

// Arm the current outgoing message for its next matching slot.
//   - Repeating states (CQ / WAIT_RPT / WAIT_ROGER / WAIT_RR73): armed every TX
//     cycle, so we keep calling / keep pushing the same message.
//   - WAIT_DONE: the final 73/RR73 is armed exactly once (s_have_cur is then
//     cleared) so we don't keep keying up after signing off.
// Called from on_tx_complete() (every burst end) and, as a safety net, from
// arm_current_if_idle() right after a state change.
static void rearm_current(void)
{
    lock();
    ft8_qso_state_t st = s_state;
    bool have = s_have_cur;
    ft8_tx_request_t req = s_cur_req;
    bool one_shot  = (st == FT8_QSO_WAIT_DONE);
    bool repeating = (st == FT8_QSO_CQ || st == FT8_QSO_WAIT_RPT ||
                      st == FT8_QSO_WAIT_ROGER || st == FT8_QSO_WAIT_RR73);
    if (have && one_shot) s_have_cur = false;   // final is sent once
    unlock();

    if (!have || (!one_shot && !repeating)) return;
    char e[64];
    if (!ft8_tx_arm(&req, e, sizeof(e)))
        ESP_LOGW(TAG, "rearm_current failed: %s", e);
}

// Arm the current message now, but only if the TX engine is idle. Most of the
// time advance() runs while our re-armed burst is ACTIVE (so this no-ops and
// on_tx_complete() does the real arming); this just covers the gap when a
// state change lands while the engine happens to be idle.
static void arm_current_if_idle(void)
{
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_IDLE) rearm_current();
}

// QSO progressed: the ARMED request (if any) still carries the PREVIOUS
// step's message - on_tx_complete() re-armed it right after our last burst.
// Replace it with the fresh s_cur_req now, so the slot loop's hold-for-decode
// gate (ft8_test.c) can fire the NEW message in this same slot instead of
// re-sending the stale one (the "everything goes twice" bug). ft8_tx_arm()
// overwrites an ARMED request but refuses an ACTIVE one, so if a burst is
// already on-air this degrades to the old deferred-arming behaviour
// (on_tx_complete() arms the fresh content after the burst).
static void arm_current_replacing_armed(void)
{
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_ACTIVE) rearm_current();
}

// Build the next exchange message (<target> <me> <extra>) and adopt it as the
// current outgoing message, moving to state st. Parity is derived from slot_sec
// (we transmit on the slot opposite the one we heard them in). Returns false
// and leaves state unchanged if encoding fails.
static bool send_next(ft8_tx_kind_t kind, const char *target, int freq,
                      int64_t slot_sec, const char *extra, ft8_qso_state_t st)
{
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request(kind, target, freq, slot_sec, extra,
                              &req, err, sizeof(err))) {
        ESP_LOGE(TAG, "send_next build failed (extra='%s'): %s", extra, err);
        return false;
    }
    set_current(&req, st);
    return true;
}

// Same as send_next(), but builds an ARRL Field Day (class+section) message
// instead of the grid/report-based standard encoding.
static bool send_next_fd(ft8_tx_kind_t kind, const char *target, int freq,
                         int64_t slot_sec, const char *class_section, ft8_qso_state_t st)
{
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request_fd(kind, target, freq, slot_sec, class_section,
                                 &req, err, sizeof(err))) {
        ESP_LOGE(TAG, "send_next_fd build failed (extra='%s'): %s", class_section, err);
        return false;
    }
    set_current(&req, st);
    return true;
}

// The partner just repeated their previous message - they haven't copied our
// report yet - but we re-heard them THIS slot with a fresh SNR. Like WSJT-X,
// bring the report we keep re-sending (and the RST_SENT we'll log) up to that
// newest measurement, so the value that finally gets through to them - and into
// both stations' logs - is our most current reading, not the stale first one.
// Only rebuilds/re-arms when the value actually changed, so an unchanged SNR
// costs nothing. as_roger => TX2's "R<rpt>" (pounce, WAIT_RR73); otherwise a
// bare "<rpt>" (CQ-run, WAIT_ROGER). Never called in Field Day mode - that
// exchange is a fixed class+section, not a signal report.
static void refresh_our_report(int fresh_snr, bool as_roger,
                               const char *target, int freq, int64_t slot_sec)
{
    char fresh_rpt[8];
    fmt_report(fresh_snr, fresh_rpt, sizeof(fresh_rpt));
    if (strcmp(fresh_rpt, s_rst_sent) == 0) return;   // unchanged - nothing to do

    char extra[16];
    if (as_roger) make_roger(fresh_rpt, extra, sizeof(extra));
    else          snprintf(extra, sizeof(extra), "%s", fresh_rpt);

    ft8_tx_kind_t kind = as_roger ? FT8_TX_KIND_ROGER_RPT : FT8_TX_KIND_REPLY;
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request(kind, target, freq, slot_sec, extra,
                              &req, err, sizeof(err))) {
        ESP_LOGW(TAG, "report refresh build failed (extra='%s'): %s", extra, err);
        return;
    }
    // Adopt as the current outgoing message WITHOUT touching s_state or
    // s_missed_slots: this is a fresher report on the SAME step, not progress,
    // so the QSO timeout must keep counting (set_current() would zero it).
    lock();
    s_cur_req  = req;
    s_have_cur = true;
    unlock();

    strncpy(s_rst_sent, fresh_rpt, sizeof(s_rst_sent) - 1);
    s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
    ESP_LOGI(TAG, "report refreshed to %s (re-heard %s) - re-sending '%s'",
             fresh_rpt, target, req.display_text);
    arm_current_replacing_armed();
}

// Another station has drifted onto our locked CQ tone since we started
// calling (ft8_tx_is_clashing() true). Re-scan for the nearest still-clear
// 50 Hz slot and move there instead of just flagging "FREQ BUSY" and
// continuing to transmit over whoever is legitimately there. Only called
// from the CQ no-answer path - never mid-exchange, where the partner is
// tracking our specific tone.
static void relocate_cq_tone_if_clashing(void)
{
    if (!ft8_tx_is_clashing()) return;

    lock();
    int old_freq = s_freq_hz;
    unlock();

    int new_freq = ft8_find_clear_tone_hz_near(old_freq);
    if (new_freq == old_freq) return;   // band fully packed - nowhere clearer to go

    lock();
    s_freq_hz               = new_freq;
    s_cur_req.audio_freq_hz = new_freq;
    s_cq_saved.audio_freq_hz = new_freq;   // so a later QSO-timeout resume-CQ keeps the new tone
    unlock();

    ft8_tx_disarm();   // cancel the stale-frequency ARMED request
    arm_current_if_idle();
    ft8_status_set("CQ: moved off busy tone -> %d Hz", new_freq);
    ESP_LOGI(TAG, "CQ tone clash at %d Hz - relocated to %d Hz", old_freq, new_freq);
}

// --- Broken-QSO resume (operator request 2026-07-16) -----------------------
// When an exchange is abandoned (timeout, or a manual abort mid-exchange) the
// essentials are kept for QSO_RESUME_WINDOW_SEC so a partner who faded and
// came back is CONTINUED where the exchange stopped - their R-report/RR73/73
// picked up mid-flow - instead of restarted from TX1. Two triggers:
// re-pouncing them by hand (ft8_qso_start), and automatically when a message
// addressed to us from them decodes while we're not busy (ft8_qso_advance).
// The auto path only ever re-engages a station the operator already chose to
// work, within the window - not a new unattended-TX category.
#define QSO_RESUME_WINDOW_SEC 300
static char             s_resume_call[FT8_CALL_MAX_LEN];
static ft8_qso_state_t  s_resume_state;
static ft8_tx_request_t s_resume_req;       // the message we were repeating
static bool             s_resume_have_req;
static char             s_resume_rst_sent[8], s_resume_rst_rcvd[8];
static char             s_resume_fd_exch[FT8_FD_EXCH_LEN];
static bool             s_resume_from_cq;
static int              s_resume_freq_hz;
static int64_t          s_resume_at;        // time(NULL) at abandonment

// True while we're draining the pileup (auto-work-pileup): the current QSO was
// started by try_start_pileup_pounce(). Lets a timed-out pileup pounce advance
// to the NEXT waiting station instead of going sticky. Cleared whenever any
// non-pileup QSO starts (ft8_qso_start/_cq) or on a manual abort.
static bool             s_pileup_active;

// Snapshot the current exchange as resumable. Call under lock(), BEFORE the
// abandon path mutates state. Only exchange states are worth resuming.
static void resume_record_save_locked(void)
{
    if (!s_target[0] || !s_have_cur) return;
    if (s_state != FT8_QSO_WAIT_RPT && s_state != FT8_QSO_WAIT_ROGER &&
        s_state != FT8_QSO_WAIT_RR73) return;
    strncpy(s_resume_call, s_target, sizeof(s_resume_call) - 1);
    s_resume_call[sizeof(s_resume_call) - 1] = '\0';
    s_resume_state    = s_state;
    s_resume_req      = s_cur_req;
    s_resume_have_req = true;
    memcpy(s_resume_rst_sent, s_rst_sent, sizeof(s_resume_rst_sent));
    memcpy(s_resume_rst_rcvd, s_rst_rcvd, sizeof(s_resume_rst_rcvd));
    memcpy(s_resume_fd_exch,  s_fd_their_exch, sizeof(s_resume_fd_exch));
    s_resume_from_cq  = s_from_cq;
    s_resume_freq_hz  = s_freq_hz;
    s_resume_at       = (int64_t)time(NULL);
}

static bool resume_record_fresh(const char *call)
{
    if (!s_resume_call[0] || !s_resume_have_req || !call || !call[0]) return false;
    if (strcmp(s_resume_call, call) != 0) return false;
    return ((int64_t)time(NULL) - s_resume_at) <= QSO_RESUME_WINDOW_SEC;
}

// Restore the abandoned exchange and re-arm its last outgoing message; the
// normal advance() flow then processes whatever the partner sends next (an
// R-report/RR73 heard this very slot advances immediately). One-shot: the
// record is consumed. Caller ensures the machine is idle/interruptible.
static void resume_restore(void)
{
    lock();
    strncpy(s_target, s_resume_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_state        = s_resume_state;
    s_cur_req      = s_resume_req;
    s_have_cur     = true;
    memcpy(s_rst_sent, s_resume_rst_sent, sizeof(s_rst_sent));
    memcpy(s_rst_rcvd, s_resume_rst_rcvd, sizeof(s_rst_rcvd));
    memcpy(s_fd_their_exch, s_resume_fd_exch, sizeof(s_fd_their_exch));
    s_from_cq      = s_resume_from_cq;
    s_freq_hz      = s_resume_freq_hz;
    s_missed_slots = 0;
    s_min_scan_utc = 0;             // partner is audible NOW - scan immediately
    s_resume_call[0]  = '\0';
    s_resume_have_req = false;
    unlock();
}

// Did the abandoned partner transmit a message addressed to us THIS slot?
// (Runs on the decode task - the ~11 KB stack snapshot is fine there, same
// pattern as capture_pileup_callers.)
static bool resume_comeback_heard(int64_t slot_sec)
{
    if (!s_my_call[0]) return false;
    if (!resume_record_fresh(s_resume_call)) return false;
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec) continue;
        char t1[16], t2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, t1, sizeof(t1), t2, sizeof(t2),
                        rest, sizeof(rest))) continue;
        if (strcmp(t1, s_my_call) != 0) continue;
        if (strcmp(t2, s_resume_call) != 0) continue;
        return true;
    }
    return false;
}

// No progress this RX slot. Count it; on the Nth, give up on this station -
// resume CQ if we were running CQ, else go to sticky TIMEOUT.
static void register_miss(const char *waiting_for)
{
    lock();
    s_missed_slots++;
    int  m       = s_missed_slots;
    bool from_cq = s_from_cq;
    char tgt[FT8_CALL_MAX_LEN];
    strncpy(tgt, s_target, sizeof(tgt));
    tgt[sizeof(tgt) - 1] = '\0';
    unlock();

    if (m < QSO_TIMEOUT_SLOTS) {
        ft8_status_set("QSO %s: %s (%d/%d)...", tgt, waiting_for, m, QSO_TIMEOUT_SLOTS);
        return;
    }

    // Remember the half-finished exchange so a comeback (manual re-pounce or
    // the auto-resume scan in advance()) continues it instead of restarting.
    lock();
    resume_record_save_locked();
    unlock();

    clear_dt_follow();   // QSO over - back to the UTC/GPS beat

    if (from_cq && s_have_cq_saved) {
        // Drop the half-finished QSO and go back to calling CQ on the frequency.
        lock();
        s_cur_req      = s_cq_saved;
        s_have_cur     = true;
        s_state        = FT8_QSO_CQ;
        s_target[0]    = '\0';
        s_missed_slots = 0;
        unlock();
        ft8_tx_disarm();
        arm_current_if_idle();
        ft8_status_set("QSO %s lost - back to CQ", tgt);
        ESP_LOGI(TAG, "QSO %s timed out - resuming CQ", tgt);
    } else {
        lock(); s_state = FT8_QSO_TIMEOUT; unlock();
        ft8_tx_disarm();
        ft8_status_set("QSO %s: no response - timeout", tgt);
        ESP_LOGW(TAG, "QSO %s timed out", tgt);
    }
}

// ---------------------------------------------------------------------------

void ft8_qso_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_state         = FT8_QSO_IDLE;
    s_target[0]     = '\0';
    s_have_cur      = false;
    s_have_cq_saved = false;
    s_from_cq       = false;
    s_rst_sent[0]   = '\0';
    s_rst_rcvd[0]   = '\0';
    s_fd_their_exch[0] = '\0';
}

bool ft8_qso_start(const ft8_tx_request_t *tx1_req, char *err, size_t err_len)
{
    if (!tx1_req || !tx1_req->target_call[0]) {
        if (err) snprintf(err, err_len, "No target callsign");
        return false;
    }
    // Any explicitly-started QSO (manual tap, robot) ends pileup-drain tracking;
    // try_start_pileup_pounce() re-sets it right after this returns for the
    // pileup case. Scopes the timed-out-pileup auto-advance to pileup pounces
    // only, so a manual pounce timeout still goes sticky for the operator.
    s_pileup_active = false;
    clear_dt_follow();   // fresh QSO - engages again when we hear this partner

    // Refuse to clobber an exchange already in progress (CQ loop or any WAIT_*
    // state) - a tap on a different row used to silently overwrite s_target /
    // s_cur_req out from under the active exchange, leaving a stale message
    // (e.g. their grid re-armed with a bogus "R" prefix mid-exchange). The
    // operator must finish or ft8_qso_abort() the current one first.
    char busy_target[FT8_CALL_MAX_LEN];
    if (ft8_qso_is_busy(busy_target, sizeof(busy_target))) {
        if (err) {
            if (busy_target[0]) snprintf(err, err_len, "Busy: working %s", busy_target);
            else                snprintf(err, err_len, "Busy: calling CQ");
        }
        return false;
    }

    if (!load_my_call(err, err_len)) return false;

    // RESUME: re-pouncing a partner we abandoned within the last few minutes
    // continues the exchange where it stopped (re-arms the last message we
    // were sending; their next R-report/RR73/73 advances normally) instead of
    // restarting from TX1 - "just finish QSOs if they get broken up"
    // (operator request 2026-07-16).
    if (resume_record_fresh(tx1_req->target_call)) {
        char who[FT8_CALL_MAX_LEN];
        strncpy(who, s_resume_call, sizeof(who) - 1);
        who[sizeof(who) - 1] = '\0';
        resume_restore();
        ft8_pileup_remove(who);
        arm_current_if_idle();
        ft8_status_set("QSO %s: resuming where we left off", who);
        ESP_LOGI(TAG, "resume (manual): %s - continuing exchange", who);
        return true;
    }

    // "Skip TX1" (settings drawer toggle): instead of exchanging grids first,
    // jump straight to sending a signal report - the same message shape/state
    // cqrun_answer() already uses for its own first reply (FT8_TX_KIND_REPLY
    // with extra=report, landing in WAIT_ROGER). Their SNR/last-heard-slot come
    // from the same ft8_screen snapshot scan_for_response() uses; if they've
    // since aged out of the decode table (race with a stale row tap), fall
    // back to the normal grid-based TX1 rather than risk a wrong-parity or
    // bogus-report first message.
    qmx_settings_t qs;
    settings_load_all(&qs);

    ft8_tx_request_t req_to_arm  = *tx1_req;
    ft8_qso_state_t  start_state = FT8_QSO_WAIT_RPT;
    char             first_rpt[8] = "599";
    bool             skip_applied = false;

    // A REPLY whose third field is already a signal report ("+NN"/"-NN" in
    // extra_field) was deliberately built report-first — the pileup modal
    // does this, because a pileup entry called US (we're the CQ-side
    // station, so grid TX1 would be the wrong role) and has usually aged
    // out of the live decode table, meaning the skip_tx1 scan below would
    // miss and silently fall back to grid (Ken KF0AYY field report,
    // 2026-07-15). Honour it as-is, regardless of the skip_tx1 toggle:
    // arm unchanged and start in WAIT_ROGER, exactly like cqrun_answer()'s
    // first reply. Every other builder passes extra=NULL for REPLY (grid),
    // so this can't misfire on a decode-list or robot pounce.
    size_t pre_rpt_len = strnlen(tx1_req->extra_field, sizeof(tx1_req->extra_field));
    if (tx1_req->kind == FT8_TX_KIND_REPLY &&
        (tx1_req->extra_field[0] == '+' || tx1_req->extra_field[0] == '-') &&
        pre_rpt_len < sizeof(first_rpt)) {
        memcpy(first_rpt, tx1_req->extra_field, pre_rpt_len);
        first_rpt[pre_rpt_len] = '\0';
        start_state  = FT8_QSO_WAIT_ROGER;
        skip_applied = true;
    } else if (qs.ft8_filters.skip_tx1) {
        // The decode-table snapshot is ~11 KB (FT8_CALL_TABLE_SIZE * sizeof(
        // ft8_call_t)) and MUST NOT be a stack local here: ft8_qso_start() runs
        // on the LVGL event-callback's small (~8 KB) task stack when a pounce is
        // confirmed (pounce_btn_cb -> ft8_qso_start), and an ~11 KB stack frame
        // overflows it at the prologue -> "Stack protection fault" crash on
        // EVERY pounce, whether or not skip_tx1 is set (the frame is reserved
        // unconditionally). Heap it in PSRAM instead. (Same rule as ft8_tx.c's
        // ft8_find_clear_tone snapshot - see CLAUDE.md "Audit every malloc under
        // ~16 KB ... it's silently going to internal RAM".)
        ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (snap) {
            int n = 0;
            ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
            int     snr = 0;
            int64_t their_last_utc = 0;
            bool    found_target = false;
            for (int i = 0; i < n; i++) {
                if (strcmp(snap[i].call, tx1_req->target_call) == 0) {
                    snr            = snap[i].last_snr_db;
                    their_last_utc = snap[i].last_utc;
                    found_target   = true;
                    break;
                }
            }
            heap_caps_free(snap);
            if (found_target) {
                fmt_report(snr, first_rpt, sizeof(first_rpt));
                char build_err[64];
                if (ft8_tx_build_request(FT8_TX_KIND_REPLY, tx1_req->target_call,
                                         tx1_req->audio_freq_hz, their_last_utc,
                                         first_rpt, &req_to_arm, build_err, sizeof(build_err))) {
                    start_state  = FT8_QSO_WAIT_ROGER;
                    skip_applied = true;
                } else {
                    ESP_LOGW(TAG, "skip_tx1: build failed (%s) - falling back to grid TX1", build_err);
                }
            } else {
                ESP_LOGW(TAG, "skip_tx1: %s not in decode table - falling back to grid TX1",
                         tx1_req->target_call);
            }
        } else {
            ESP_LOGW(TAG, "skip_tx1: snapshot alloc failed - falling back to grid TX1");
        }
    }

    char arm_err[64];
    if (!ft8_tx_arm(&req_to_arm, arm_err, sizeof(arm_err))) {
        if (err) snprintf(err, err_len, "%s", arm_err);
        return false;
    }

    // Earliest valid RX slot to scan: one slot after our first message fires.
    // Computed in ms internally (next_slot_sec) using the armed request's own
    // protocol period, so this lands on a boundary the engine will actually
    // produce - see that function's comment for the FT4 bug this fixes. Also
    // gates the skip-TX1 WAIT_ROGER path the same way WAIT_RPT is gated below
    // (see the ft8_qso_advance() early-return) - without it, a decode cycle
    // landing between arming and the first burst actually firing would count
    // as a missed roger before we've transmitted a single message.
    int64_t tx1_slot = next_slot_sec(req_to_arm.use_parity, req_to_arm.want_even_slot,
                                     req_to_arm.protocol);

    lock();
    s_state         = start_state;
    strncpy(s_target, tx1_req->target_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_freq_hz       = req_to_arm.audio_freq_hz;
    s_min_scan_utc  = tx1_slot + 15;
    s_missed_slots  = 0;
    s_from_cq       = false;
    s_cur_req       = req_to_arm;   // re-send each cycle until they reply
    s_have_cur      = true;
    s_have_cq_saved = false;
    if (skip_applied) {
        // We sent them a numeric report directly. "599" is only the fallback:
        // their roger "R<rpt>" carries their own measurement of us (not an
        // echo of ours) and overwrites this in the WAIT_ROGER handler.
        strncpy(s_rst_sent, first_rpt, sizeof(s_rst_sent) - 1);
        s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
        strncpy(s_rst_rcvd, "599", sizeof(s_rst_rcvd) - 1);
        s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
    } else {
        // Normal pounce: we receive their report (RST_RCVD); we never give our
        // own numeric report in TX1/TX2 (RST_SENT = "599").
        strncpy(s_rst_sent, "599", sizeof(s_rst_sent) - 1);
        s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
        s_rst_rcvd[0] = '\0';
    }
    s_fd_their_exch[0] = '\0';
    unlock();

    // We're committing to working them now - take them out of the pileup
    // list (harmless no-op if they weren't in it, e.g. a direct tap on a
    // still-live decode row rather than the pileup list itself).
    ft8_pileup_remove(tx1_req->target_call);

    if (skip_applied) {
        ft8_status_set("QSO %s: sent report %s - waiting for roger", tx1_req->target_call, first_rpt);
        ESP_LOGI(TAG, "started QSO (pounce, skip-TX1): %s @ %d Hz report=%s, min_scan=%lld",
                 tx1_req->target_call, req_to_arm.audio_freq_hz, first_rpt, (long long)(tx1_slot + 15));
    } else {
        ft8_status_set("QSO %s: TX1 sent - waiting for report", tx1_req->target_call);
        ESP_LOGI(TAG, "started QSO (pounce): %s @ %d Hz, min_scan=%lld",
                 tx1_req->target_call, req_to_arm.audio_freq_hz, (long long)(tx1_slot + 15));
    }
    return true;
}

bool ft8_qso_start_cq(const ft8_tx_request_t *cq_req, char *err, size_t err_len)
{
    if (!cq_req) {
        if (err) snprintf(err, err_len, "No CQ request");
        return false;
    }
    if (!load_my_call(err, err_len)) return false;

    // CQ must alternate TX/RX every 30 s. If no parity preference was set,
    // lock to the parity of the first TX slot so re-arms always target the
    // same slot type and the opposite slot stays free for RX.
    ft8_tx_request_t req_copy = *cq_req;
    if (!req_copy.use_parity) {
        int64_t next_slot = next_slot_sec(false, false, req_copy.protocol);
        req_copy.use_parity     = true;
        req_copy.want_even_slot = slot_is_even(next_slot, req_copy.protocol);
    }

    char arm_err[64];
    if (!ft8_tx_arm(&req_copy, arm_err, sizeof(arm_err))) {
        if (err) snprintf(err, err_len, "%s", arm_err);
        return false;
    }

    lock();
    s_state         = FT8_QSO_CQ;
    s_target[0]     = '\0';
    s_freq_hz       = req_copy.audio_freq_hz;
    s_cur_req       = req_copy;
    s_have_cur      = true;
    s_cq_saved      = req_copy;
    s_have_cq_saved = true;
    s_from_cq       = true;
    s_missed_slots  = 0;
    s_pileup_active = false;   // starting CQ is not part of a pileup drain
    clear_dt_follow();         // no partner yet - transmit CQ on the UTC/GPS beat
    // CQ-run: RST_SENT set in cqrun_answer. "599" is only the RST_RCVD
    // fallback - their report answer (cqrun_answer) or numeric roger
    // (WAIT_ROGER) overwrites it with their actual measurement of us.
    s_rst_sent[0] = '\0';
    strncpy(s_rst_rcvd, "599", sizeof(s_rst_rcvd));
    s_fd_their_exch[0] = '\0';
    unlock();

    ft8_status_set("CQ: calling - listening for answers");
    ESP_LOGI(TAG, "CQ loop started @ %d Hz", cq_req->audio_freq_hz);
    return true;
}

// Build + adopt our reply to a station that answered our CQ (CQ-run). They
// sent their grid (or a report); we answer with a signal report and wait for
// their roger. If they jumped straight to RR73/73, we just send 73.
static void cqrun_answer(const char *caller, int caller_freq, int caller_snr,
                         const char *report, int64_t slot_sec,
                         bool got_rr73, bool got_73)
{
    // Stay on our own CQ tone for the entire exchange — the answering station
    // uses their own separate tone; switching to caller_freq would put us on
    // their (occupied) frequency and trigger a false clash warning.
    lock();
    strncpy(s_target, caller, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    int our_freq = s_freq_hz;   // already set to our CQ tone in ft8_qso_start_cq()
    unlock();

    // A station just answered our CQ - if they're off the band's beat, follow it.
    update_dt_follow(caller);

    // If their answer carried a numeric report of us instead of a grid
    // (OS4K opened with 'OZ1LAV OS4K -06'), that's RST_RCVD - capture it now
    // so it's logged even if they later jump straight to RR73.
    if (report && (report[0] == '+' || report[0] == '-') &&
        report[1] >= '0' && report[1] <= '9') {
        strncpy(s_rst_rcvd, report, sizeof(s_rst_rcvd) - 1);
        s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
    }

    // We're committing to working them now - take them out of the pileup
    // list (harmless no-op if they were never in it, e.g. an unfiltered
    // caller who answered on the very first slot of the CQ).
    ft8_pileup_remove(caller);

    qmx_settings_t qs;
    settings_load_all(&qs);
    bool fd_mode = qs.field_day_en && qs.fd_class[0] && qs.fd_section[0];

    bool ok;
    char rpt[8];
    fmt_report(caller_snr, rpt, sizeof(rpt));

    if (got_rr73 || got_73) {
        ok = send_next(FT8_TX_KIND_73, caller, our_freq, slot_sec, "73",
                       FT8_QSO_WAIT_DONE);
        if (ok) {
            strncpy(s_rst_sent, rpt, sizeof(s_rst_sent) - 1);
            s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
            ft8_status_set("QSO %s: sending 73 (their SNR %s)", caller, rpt);
        }
    } else if (fd_mode) {
        // Field Day: our first reply carries our own class+section (no "R" -
        // this is the first time we're sending it), not a numeric report.
        char exch[FT8_FD_EXCH_LEN];
        snprintf(exch, sizeof(exch), "%s %s", qs.fd_class, qs.fd_section);
        ok = send_next_fd(FT8_TX_KIND_REPLY, caller, our_freq, slot_sec, exch,
                          FT8_QSO_WAIT_ROGER);
        if (ok) ft8_status_set("QSO %s: answered - sending %s", caller, exch);
    } else {
        ok = send_next(FT8_TX_KIND_REPLY, caller, our_freq, slot_sec, rpt,
                       FT8_QSO_WAIT_ROGER);
        if (ok) {
            strncpy(s_rst_sent, rpt, sizeof(s_rst_sent) - 1);
            s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
            ft8_status_set("QSO %s: answered - sending report %s", caller, rpt);
        }
    }

    ESP_LOGI(TAG, "cqrun_answer: %s (their_freq=%d Hz, our_freq=%d Hz)",
             caller, caller_freq, our_freq);

    if (!ok) {
        // Couldn't build a valid reply - abandon this answer, keep calling CQ.
        lock();
        s_cur_req      = s_cq_saved;
        s_state        = FT8_QSO_CQ;
        s_target[0]    = '\0';
        s_missed_slots = 0;
        unlock();
    }
    arm_current_if_idle();
}

// Passive pileup capture: every station that addressed a message to us this
// slot, other than whoever we're actively working right now, gets upserted
// into the pileup list (ft8_pileup.c). Runs unconditionally regardless of
// QSO state - a caller answering our CQ while we're already mid-exchange
// with someone else is exactly the "gave up waiting" scenario the pileup
// list exists for, and they'd otherwise never get tracked (advance()'s
// per-state scans only ever look for messages FROM the current target). No
// TX ever results from this - purely a background list update.
static void capture_pileup_callers(int64_t slot_sec)
{
    if (!s_my_call[0]) return;   // nothing to scan for until identity is loaded

    char cur_target[FT8_CALL_MAX_LEN];
    lock();
    strncpy(cur_target, s_target, sizeof(cur_target) - 1);
    cur_target[sizeof(cur_target) - 1] = '\0';
    unlock();

    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec) continue;
        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;              // not addressed to us
        if (strcmp(tok2, s_my_call) == 0) continue;              // avoid MYCALL MYCALL loops
        if (cur_target[0] && strcmp(tok2, cur_target) == 0) continue;  // already our active partner
        // Already worked on this band? Then their trailing 73/RR73 (or a late
        // reply after we timed out and completed with someone else) must NOT
        // put them back in the pileup - the whole reason Dirk saw a completed
        // call linger. Same worked-before check the auto-pileup picker uses.
        if (adif_log_contains_call_on_band(tok2, cat_get_frequency())) continue;
        ft8_pileup_note_caller(tok2, snap[i].last_snr_db, snap[i].last_freq, slot_sec);
    }
}

// Auto-work-pileup: we've just finished (or timed out of) a QSO and the
// operator enabled "Auto-work pileup". Pick the strongest waiting caller from
// the pileup list and pounce them, exactly the way the robot answers a CQ - the
// TX1 is built the same, and ft8_qso_start() then applies Skip-TX1, the resume
// window, the busy-guard and identity checks centrally, so this inherits all of
// them for free. Returns true if a pounce was started (state now WAIT_*), false
// if disabled / nobody eligible / build refused (caller falls back to CQ/idle).
// Strongest-SNR first: best chance of a clean completion, standard pileup order.
static bool try_start_pileup_pounce(void)
{
    qmx_settings_t qs;
    settings_load_all(&qs);
    if (!qs.ft8_filters.auto_pileup)            return false;
    if (!qs.my_callsign[0] || !qs.my_grid[0])   return false;

    ft8_pileup_entry_t pile[FT8_PILEUP_MAX];
    int n = ft8_pileup_get_all(pile, FT8_PILEUP_MAX);
    if (n <= 0) return false;

    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!pile[i].call[0]) continue;
        // Skip anyone already worked on this band if that filter is on - same
        // rule the robot enforces, so auto-work never re-calls a logged station.
        if (qs.ft8_filters.excl_worked_before &&
            adif_log_contains_call_on_band(pile[i].call, cat_get_frequency())) continue;
        if (best < 0 || pile[i].snr_db > pile[best].snr_db) best = i;
    }
    if (best < 0) return false;

    // Reply on a clear tone (not their own), parity derived from the slot we last
    // heard them call us in (parity is periodic, so a several-minute-old
    // last_seen still gives the correct TX parity).
    int reply_freq_hz = ft8_find_clear_tone_hz();
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request(FT8_TX_KIND_REPLY, pile[best].call, reply_freq_hz,
                              pile[best].last_seen_utc, NULL, &req, err, sizeof(err))) {
        ESP_LOGW(TAG, "auto-pileup build_request(%s) failed: %s", pile[best].call, err);
        return false;
    }
    if (!ft8_qso_start(&req, err, sizeof(err))) {   // clears s_pileup_active
        ESP_LOGW(TAG, "auto-pileup ft8_qso_start(%s) refused: %s", pile[best].call, err);
        return false;
    }
    s_pileup_active = true;   // set AFTER ft8_qso_start (which clears it)
    ESP_LOGI(TAG, "auto-pileup: working %s (snr=%d, %d waiting)",
             pile[best].call, pile[best].snr_db, n);
    ft8_status_set("Pileup: working %s", pile[best].call);
    return true;
}

void ft8_qso_advance(int64_t slot_sec)
{
    capture_pileup_callers(slot_sec);

    // Comeback auto-resume: a partner we abandoned recently decoded THIS slot
    // with a message addressed to us, and we're not mid-exchange with anyone
    // else - pick the QSO back up where it stopped. Restoring BEFORE the
    // state snapshot below means the restored WAIT_* handler processes their
    // message from this very slot (no extra cycle lost). An armed CQ is
    // cancelled exactly like cqrun_answer() does for a fresh answer.
    {
        lock();
        ft8_qso_state_t st0 = s_state;
        unlock();
        bool interruptible = (st0 == FT8_QSO_IDLE || st0 == FT8_QSO_DONE ||
                              st0 == FT8_QSO_TIMEOUT || st0 == FT8_QSO_CQ);
        if (interruptible && resume_comeback_heard(slot_sec)) {
            char who[FT8_CALL_MAX_LEN];
            strncpy(who, s_resume_call, sizeof(who) - 1);
            who[sizeof(who) - 1] = '\0';
            if (st0 == FT8_QSO_CQ) ft8_tx_disarm();
            resume_restore();
            ft8_status_set("QSO %s: partner came back - resuming", who);
            ESP_LOGI(TAG, "resume (auto): %s heard again - continuing exchange", who);
        }
    }

    lock();
    ft8_qso_state_t st = s_state;
    char    target[FT8_CALL_MAX_LEN];
    int     freq     = s_freq_hz;
    int64_t min_scan = s_min_scan_utc;
    bool    from_cq  = s_from_cq;
    strncpy(target, s_target, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    unlock();

    if (st == FT8_QSO_TIMEOUT) {
        // A pileup-initiated pounce that got no answer (the caller wandered off
        // in the minutes since they called) would normally go sticky. Instead
        // clear it and move to the next waiting station so one dead caller
        // doesn't stall the whole drain. A human/robot pounce timeout is left
        // sticky (s_pileup_active is false for those).
        if (s_pileup_active) {
            ft8_qso_abort();                       // TIMEOUT -> IDLE, clears s_pileup_active
            if (try_start_pileup_pounce()) return; // next waiting station
        }
        return;
    }
    if (st == FT8_QSO_IDLE) return;

    if (st == FT8_QSO_DONE) {
        // Auto-work pileup: before falling back to CQ/idle, work anyone still
        // waiting in the pileup (they called us during a busy exchange). Drains
        // strongest-first across successive completions; only when the list is
        // empty do we resume CQ / go idle as before.
        if (try_start_pileup_pounce()) return;
        s_pileup_active = false;
        // A CQ-run QSO resumes calling CQ on the same tone instead of going
        // idle - same as the QSO_TIMEOUT_SLOTS give-up path in register_miss(),
        // just on the success side. Saves the operator from re-tapping Call CQ
        // after every single contact during an activation.
        lock();
        bool resume = s_from_cq && s_have_cq_saved;
        ft8_tx_request_t cq_req = s_cq_saved;
        if (resume) {
            s_cur_req      = cq_req;
            s_have_cur     = true;
            s_state        = FT8_QSO_CQ;
            s_target[0]    = '\0';
            s_missed_slots = 0;
        } else {
            s_state    = FT8_QSO_IDLE;
            s_have_cur = false;
        }
        unlock();
        if (resume) {
            arm_current_if_idle();
            ft8_status_set("CQ: calling - listening for answers");
            ESP_LOGI(TAG, "QSO done - resuming CQ @ %d Hz", cq_req.audio_freq_hz);
        } else {
            ft8_status_set("Idle");
        }
        return;
    }

    if (st == FT8_QSO_WAIT_DONE) {
        // Final (73/RR73) armed or fired; once it leaves the air we're done.
        if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_IDLE) {
            lock(); s_state = FT8_QSO_DONE; s_have_cur = false; unlock();
            ft8_status_set("QSO %s: complete!", target);
            ESP_LOGI(TAG, "QSO with %s complete", target);
            clear_dt_follow();   // QSO done - back to the UTC/GPS beat
            // Drop the just-worked station from the pileup. It was removed at
            // qso_start, but the partner keeps addressing us through the
            // exchange (report/RR73/73, and Dirk DK7CVD's late-reply case), and
            // capture_pileup_callers no longer excludes them once s_target
            // clears - so without this (plus the worked-before skip in capture)
            // they'd reappear in the pileup right after a successful QSO.
            ft8_pileup_remove(target);

            // A completed QSO with this call supersedes any older resumable
            // half-QSO - never auto-resume into a duplicate afterwards.
            if (s_resume_call[0] && strcmp(s_resume_call, target) == 0) {
                s_resume_call[0]  = '\0';
                s_resume_have_req = false;
            }

            // Build and save the ADIF record.
            qmx_settings_t qs;
            settings_load_all(&qs);

            // Look up their grid from the ft8_screen decode table.
            ft8_call_t snap[FT8_CALL_TABLE_SIZE];
            int snap_n = 0;
            char their_grid[FT8_GRID_MAX_LEN] = {0};
            ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &snap_n);
            for (int i = 0; i < snap_n; i++) {
                if (strcmp(snap[i].call, target) == 0 && snap[i].last_grid[0]) {
                    strncpy(their_grid, snap[i].last_grid, sizeof(their_grid) - 1);
                    break;
                }
            }

            // ARRL Field Day exchange (if any): their_exch is "<class> <section>";
            // split off the section (last token) for the dedicated ADIF fields.
            char their_fd_class[FT8_FD_EXCH_LEN] = {0}, their_fd_section[FT8_FD_EXCH_LEN] = {0};
            lock();
            bool have_fd_exch = s_fd_their_exch[0] != '\0';
            char fd_exch_copy[FT8_FD_EXCH_LEN];
            strncpy(fd_exch_copy, s_fd_their_exch, sizeof(fd_exch_copy) - 1);
            fd_exch_copy[sizeof(fd_exch_copy) - 1] = '\0';
            unlock();
            if (have_fd_exch) {
                char *sp = strrchr(fd_exch_copy, ' ');
                if (sp) {
                    *sp = '\0';
                    snprintf(their_fd_class, sizeof(their_fd_class), "%s", fd_exch_copy);
                    snprintf(their_fd_section, sizeof(their_fd_section), "%s", sp + 1);
                }
            }

            adif_qso_t qso = {
                .their_call = target,
                .my_call    = s_my_call,
                .my_grid    = qs.my_grid,
                .their_grid = their_grid,
                .freq_hz    = cat_get_frequency(),
                .mode       = (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? "FT4" : "FT8",
                .rst_sent   = s_rst_sent,
                .rst_rcvd   = s_rst_rcvd,
                .qso_time   = time(NULL),
                .my_arrl_class      = have_fd_exch ? qs.fd_class   : NULL,
                .my_arrl_section    = have_fd_exch ? qs.fd_section : NULL,
                .their_arrl_class   = have_fd_exch ? their_fd_class   : NULL,
                .their_arrl_section = have_fd_exch ? their_fd_section : NULL,
            };
            adif_log_record(&qso);
            s_fd_their_exch[0] = '\0';
        }
        return;
    }

    // ---- CQ: listen for anyone answering us --------------------------------
    if (st == FT8_QSO_CQ) {
        char caller[FT8_CALL_MAX_LEN] = {0};
        int  caller_freq = freq;
        int  caller_snr  = 0;
        char report[FT8_FD_EXCH_LEN] = {0};
        bool got_rr73 = false, got_73 = false;
        if (scan_for_reply_to_me(slot_sec, caller, sizeof(caller),
                                 &caller_freq, &caller_snr,
                                 report, sizeof(report), &got_rr73, &got_73)) {
            ESP_LOGI(TAG, "CQ: %s answered @ %d Hz snr=%d (rr73=%d 73=%d)",
                     caller, caller_freq, caller_snr, got_rr73, got_73);
            ft8_tx_disarm();   // cancel the re-armed CQ (no-op if already ACTIVE)
            cqrun_answer(caller, caller_freq, caller_snr, report, slot_sec, got_rr73, got_73);
        } else {
            // No answer - check whether someone has drifted onto our tone
            // since we started calling, and move off it if so. Otherwise
            // on_tx_complete keeps the CQ armed at the same tone; idle
            // fallback only.
            relocate_cq_tone_if_clashing();
            arm_current_if_idle();
        }
        return;
    }

    // ---- Exchange states: hear the partner on the opposite-parity slot -----
    // (our first message hasn't fired yet -> nothing to hear; don't scan
    // early). Normally only WAIT_RPT needs this (pounce hasn't sent TX1 yet),
    // but a skip-TX1 pounce starts straight in WAIT_ROGER, so it needs the
    // same guard - gated to !from_cq so CQ-run's own WAIT_ROGER (whose reply
    // has always already fired by the time advance() can next run - see
    // cqrun_answer()) is never affected.
    if ((st == FT8_QSO_WAIT_RPT || (st == FT8_QSO_WAIT_ROGER && !from_cq)) &&
        slot_sec < min_scan) return;

    char report[FT8_FD_EXCH_LEN] = {0};
    bool got_rr73 = false, got_73 = false;
    int  snr_db   = 0;
    bool found = scan_for_response(slot_sec, report, sizeof(report), &got_rr73, &got_73, &snr_db);

    // Heard the partner this slot -> update the DT-follow-partner TX offset to
    // their current beat (engages only if they're >threshold off the band).
    if (found) update_dt_follow(target);

    qmx_settings_t qs_exch;
    settings_load_all(&qs_exch);
    bool fd_mode = qs_exch.field_day_en && qs_exch.fd_class[0] && qs_exch.fd_section[0];

    if (st == FT8_QSO_WAIT_RPT) {
        // POUNCE: we sent our grid; expect their signal report (normal mode)
        // or their class+section (Field Day - sent without "R", since this is
        // their first FD-specific message to us).
        if (!found) { register_miss("waiting for report"); return; }

        // Our own locally-measured SNR of their signal. This is what TX2's
        // "R<report>" must carry - our measurement of THEM, NOT an echo of the
        // report they sent us (fixed 2026-07-17: two field reports, Steve N0SZ
        // + Jonathan KN6LFB, both saw us reply R<their-report> regardless of
        // actual strength; skip-TX1 was already correct because it builds the
        // report from this same SNR). Also the RST_SENT logged for ADIF, same
        // convention cqrun_answer() uses for the CQ-run direction.
        char our_rpt[8];
        fmt_report(snr_db, our_rpt, sizeof(our_rpt));
        strncpy(s_rst_sent, our_rpt, sizeof(s_rst_sent) - 1);
        s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';

        bool ok;
        if (got_rr73 || got_73) {
            ESP_LOGI(TAG, "WAIT_RPT: %s sent RR73/73 directly", target);
            ok = send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73",
                           FT8_QSO_WAIT_DONE);
            if (ok) ft8_status_set("QSO %s: sending 73", target);
        } else if (fd_mode) {
            // Their class+section (no R - this is their first FD message).
            // Capture it for ADIF, then acknowledge with our own, R-prefixed.
            strncpy(s_fd_their_exch, report, sizeof(s_fd_their_exch) - 1);
            s_fd_their_exch[sizeof(s_fd_their_exch) - 1] = '\0';
            char roger[FT8_FD_EXCH_LEN];
            snprintf(roger, sizeof(roger), "R %s %s", qs_exch.fd_class, qs_exch.fd_section);
            ESP_LOGI(TAG, "WAIT_RPT (FD): %s sent '%s' -> TX2 %s", target, report, roger);
            ok = send_next_fd(FT8_TX_KIND_ROGER_RPT, target, freq, slot_sec, roger,
                              FT8_QSO_WAIT_RR73);
            if (ok) ft8_status_set("QSO %s: heard %s - sending %s", target, report, roger);
        } else {
            // TX2 = "R" + OUR measured report of them (our_rpt), not their
            // report echoed back. `report` (their report of us) is captured
            // as RST_RCVD only. roger[] sized generously so the inlined
            // make_roger's "R%s" can't trip -Wformat-truncation on our_rpt's
            // 8-byte bound (real content is tiny, e.g. "R-04").
            char roger[16];
            make_roger(our_rpt, roger, sizeof(roger));
            ESP_LOGI(TAG, "WAIT_RPT: %s reported us %s, we heard them %s -> TX2 %s",
                     target, report, our_rpt, roger);
            ok = send_next(FT8_TX_KIND_ROGER_RPT, target, freq, slot_sec, roger,
                           FT8_QSO_WAIT_RR73);
            if (ok) {
                // Capture their report of our signal (RST_RCVD for pounce).
                strncpy(s_rst_rcvd, report, sizeof(s_rst_rcvd) - 1);
                s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
                ft8_status_set("QSO %s: heard %s - sending %s", target, report, roger);
            }
        }
        if (!ok) {
            lock(); s_state = FT8_QSO_TIMEOUT; unlock();
            ft8_status_set("QSO %s: TX error", target);
        }
        arm_current_replacing_armed();
        return;
    }

    if (st == FT8_QSO_WAIT_ROGER) {
        // CQ-RUN: we sent a report; expect their R<report> (then we send RR73).
        if (!found) { register_miss("waiting for roger"); return; }

        if (got_rr73 || got_73) {
            if (send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73", FT8_QSO_WAIT_DONE))
                ft8_status_set("QSO %s: sending 73", target);
            arm_current_replacing_armed();
            return;
        }
        if (is_roger_token(report)) {
            if (report[1] == ' ') {
                // Field Day ack "R <theirclass> <theirsection>" - capture their
                // exchange for ADIF (strip the "R " prefix).
                strncpy(s_fd_their_exch, report + 2, sizeof(s_fd_their_exch) - 1);
                s_fd_their_exch[sizeof(s_fd_their_exch) - 1] = '\0';
            } else if (report[1] == '+' || report[1] == '-' ||
                       (report[1] >= '0' && report[1] <= '9')) {
                // Numeric roger "R<rpt>": the value is their own measurement of
                // OUR signal, not an echo of the report we sent (we sent -08,
                // OS4K rogered R-06 - live capture 2026-07-15). This is the
                // RST_RCVD for a CQ-run/skip-TX1 QSO; without this it logged
                // the "599" placeholder. Excludes "RRR" (no numeric value).
                strncpy(s_rst_rcvd, report + 1, sizeof(s_rst_rcvd) - 1);
                s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
            }
            if (send_next(FT8_TX_KIND_ROGER_RPT, target, freq, slot_sec, "RR73",
                          FT8_QSO_WAIT_DONE))
                ft8_status_set("QSO %s: rogered %s - sending RR73", target, report);
            arm_current_replacing_armed();
            return;
        }
        // They repeated their grid/report (didn't get ours). Before counting
        // the miss, refresh our report to the SNR we measured THIS slot so the
        // re-send - and the RST_SENT we log - carries our freshest reading, the
        // way WSJT-X does. FD's fixed class+section is never refreshed.
        if (!fd_mode) refresh_our_report(snr_db, false, target, freq, slot_sec);
        register_miss("re-sending report");
        return;
    }

    if (st == FT8_QSO_WAIT_RR73) {
        // POUNCE: we sent R<report>; expect RR73/73 (then we send 73).
        if (!found || (!got_rr73 && !got_73)) {
            // Re-heard them repeating their report (found, but no RR73 yet):
            // refresh our R<report> to this slot's fresh SNR before re-sending,
            // same WSJT-X behaviour as the CQ-run WAIT_ROGER path above.
            if (found && !fd_mode) refresh_our_report(snr_db, true, target, freq, slot_sec);
            register_miss("waiting for RR73");
            return;
        }
        ESP_LOGI(TAG, "WAIT_RR73: %s sent RR73/73 - arming TX3", target);
        if (send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73", FT8_QSO_WAIT_DONE))
            ft8_status_set("QSO %s: sending 73", target);
        else {
            lock(); s_state = FT8_QSO_TIMEOUT; unlock();
            ft8_status_set("QSO %s: TX error", target);
        }
        arm_current_replacing_armed();
        return;
    }
}

void ft8_qso_on_tx_complete(void)
{
    // Re-arm whatever we're currently sending for its next matching slot. This
    // gives the CQ loop its 30 s cadence and keeps exchange messages repeating
    // until answered; the final 73/RR73 is armed once (see rearm_current).
    rearm_current();
}

bool ft8_qso_override_next(ft8_tx_kind_t kind, char *err, size_t err_len)
{
    lock();
    ft8_qso_state_t st = s_state;
    char target[FT8_CALL_MAX_LEN];
    strncpy(target, s_target, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    int  freq      = s_freq_hz;
    bool have_cur  = s_have_cur;
    bool use_par   = s_cur_req.use_parity;
    bool want_even = s_cur_req.want_even_slot;
    ft8_tx_request_t cur_copy = s_cur_req;
    unlock();

    if (st != FT8_QSO_WAIT_RPT && st != FT8_QSO_WAIT_ROGER && st != FT8_QSO_WAIT_RR73) {
        if (err) snprintf(err, err_len, "No active QSO exchange");
        return false;
    }

    if (kind == FT8_TX_KIND_REPLY) {
        if (!have_cur) {
            if (err) snprintf(err, err_len, "No current message");
            return false;
        }
        char arm_err[64];
        bool ok = ft8_tx_arm(&cur_copy, arm_err, sizeof(arm_err));
        if (!ok && err) snprintf(err, err_len, "%s", arm_err);
        ESP_LOGI(TAG, "QSO override: re-send '%s'", cur_copy.display_text);
        return ok;
    }

    const char *extra = (kind == FT8_TX_KIND_73) ? "73" : "RR73";
    ft8_tx_request_t req;
    char build_err[64];
    if (!ft8_tx_build_request(kind, target, freq, (int64_t)time(NULL), extra,
                               &req, build_err, sizeof(build_err))) {
        if (err) snprintf(err, err_len, "%s", build_err);
        return false;
    }
    if (have_cur && use_par) {
        req.use_parity     = true;
        req.want_even_slot = want_even;
    }

    ft8_tx_disarm();
    set_current(&req, FT8_QSO_WAIT_DONE);
    arm_current_if_idle();
    ft8_status_set("QSO %s: manual %s", target, extra);
    ESP_LOGI(TAG, "QSO override: %s -> %s, WAIT_DONE", target, extra);
    return true;
}

void ft8_qso_abort(void)
{
    lock();
    char target[FT8_CALL_MAX_LEN];
    strncpy(target, s_target, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    // A mid-exchange abort is resumable too (no-op for CQ/idle aborts - the
    // save helper only records WAIT_* exchange states).
    resume_record_save_locked();
    s_state         = FT8_QSO_IDLE;
    s_target[0]     = '\0';
    s_have_cur      = false;
    s_have_cq_saved = false;
    s_from_cq       = false;
    s_pileup_active = false;   // an abort ends any pileup drain
    clear_dt_follow();         // ...and returns TX to the UTC/GPS beat
    unlock();
    ft8_tx_disarm();
    if (target[0]) ft8_status_set("QSO %s: aborted", target);
    ESP_LOGI(TAG, "QSO aborted");
}

ft8_qso_state_t ft8_qso_get_state(void)
{
    lock(); ft8_qso_state_t st = s_state; unlock();
    return st;
}

void ft8_qso_get_target(char *buf, size_t len)
{
    if (!buf || !len) return;
    lock();
    strncpy(buf, s_target, len - 1);
    buf[len - 1] = '\0';
    unlock();
}

bool ft8_qso_is_busy(char *target_buf, size_t len)
{
    lock();
    ft8_qso_state_t st = s_state;
    if (target_buf && len) {
        strncpy(target_buf, s_target, len - 1);
        target_buf[len - 1] = '\0';
    }
    unlock();
    return st != FT8_QSO_IDLE && st != FT8_QSO_DONE && st != FT8_QSO_TIMEOUT;
}

void ft8_qso_get_cur_extra(char *buf, size_t len)
{
    if (!buf || !len) return;
    lock();
    if (s_have_cur && s_cur_req.extra_field[0])
        strncpy(buf, s_cur_req.extra_field, len - 1);
    else
        buf[0] = '\0';
    buf[len - 1] = '\0';
    unlock();
}

bool ft8_qso_cq_filter_active(void)
{
    lock();
    ft8_qso_state_t st = s_state;
    bool fc = s_from_cq;
    unlock();
    return fc && (st == FT8_QSO_CQ || st == FT8_QSO_WAIT_RPT ||
                  st == FT8_QSO_WAIT_ROGER || st == FT8_QSO_WAIT_RR73 ||
                  st == FT8_QSO_WAIT_DONE);
}

bool ft8_qso_get_priority_freq(int *freq_hz_out)
{
    lock();
    ft8_qso_state_t st   = s_state;
    bool             from_cq = s_from_cq;
    int              freq    = s_freq_hz;
    unlock();

    // Pounce only (WAIT_RPT / WAIT_ROGER / WAIT_RR73 — WAIT_ROGER normally
    // belongs to CQ-run, see the state table in ft8_qso.h, but a skip-TX1
    // pounce starts straight in WAIT_ROGER too; the `from_cq` check above
    // already excludes the real CQ-run case, so including it here only ever
    // matches the skip-TX1 pounce). s_freq_hz is the partner's own tone there
    // (we called THEM at it, and by FT8 convention they keep replying on it
    // for the whole exchange). CQ-run's s_freq_hz is OUR tone, not theirs, so
    // it's not a useful hint for who's replying to our report — skip it there.
    if (from_cq) return false;
    if (st != FT8_QSO_WAIT_RPT && st != FT8_QSO_WAIT_ROGER && st != FT8_QSO_WAIT_RR73)
        return false;

    if (freq_hz_out) *freq_hz_out = freq;
    return true;
}
