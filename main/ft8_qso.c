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
#include "ft8_tx.h"
#include "ft8_status.h"
#include "ui/ft8_screen.h"
#include "storage/settings.h"
#include "adif/adif_log.h"
#include "cat/cat.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_qso";

// How many consecutive RX slots with no expected reply before we give up on
// the station we're working. ~4 cycles of patience (the user can wander off
// and come back, fading, QRM, etc.).
#define QSO_TIMEOUT_SLOTS  4

// Clamp the SNR we report to a sane FT8 range.
#define RPT_MIN_DB  (-24)
#define RPT_MAX_DB  (+15)

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
// Pounce: rst_rcvd = what they told us; rst_sent = "599" (we echo their report, not our own).
// CQ-run: rst_sent = our report of their signal; rst_rcvd = "599".
static char               s_rst_sent[8];
static char               s_rst_rcvd[8];

// ---------------------------------------------------------------------------

static inline bool slot_is_even(int64_t sec) { return ((sec / 15) % 2) == 0; }
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
// which can also begin with 'R' for far-east locators like RE78).
static bool is_roger_token(const char *t)
{
    if (t[0] != 'R') return false;
    char c = t[1];
    return c == '-' || c == '+' || c == 'R' || (c >= '0' && c <= '9');
}

// Format a coarse SNR into an FT8 report token: "-07", "+02", "-15".
static void fmt_report(int snr_db, char *out, size_t len)
{
    if (snr_db < RPT_MIN_DB) snr_db = RPT_MIN_DB;
    if (snr_db > RPT_MAX_DB) snr_db = RPT_MAX_DB;
    snprintf(out, len, "%+03d", snr_db);
}

// Build "R<report>" for TX2. Their report is e.g. "-10"; pass through if it
// already starts with 'R'.
static void make_roger(const char *their_report, char *out, size_t len)
{
    if (their_report[0] == 'R') snprintf(out, len, "%s", their_report);
    else                        snprintf(out, len, "R%s", their_report);
}

// Scan the ft8_screen table for a message FROM s_target TO s_my_call decoded
// in slot_sec. Fills one of report_buf / *got_rr73 / *got_73.
static bool scan_for_response(int64_t slot_sec,
                              char *report_buf, size_t report_cap,
                              bool *got_rr73, bool *got_73)
{
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, s_target) != 0) continue;
        if (snap[i].last_utc != slot_sec) continue;

        char tok1[16], tok2[16], tok3[16];
        tok3[0] = '\0';
        if (sscanf(snap[i].last_text, "%15s %15s %15s", tok1, tok2, tok3) < 2) continue;
        if (strcmp(tok1, s_my_call) != 0) continue; // not addressed to us
        if (strcmp(tok2, s_target)  != 0) continue; // not from them

        if (strcmp(tok3, "RR73") == 0) { *got_rr73 = true; return true; }
        if (strcmp(tok3, "73")   == 0) { *got_73   = true; return true; }
        if (tok3[0] != '\0') {
            snprintf(report_buf, report_cap, "%s", tok3);
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

static bool ft8_filter_match(const char *text, const ft8_filters_t *f)
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
        char tok1[16], tok2[16], tok3[16];
        tok3[0] = '\0';
        if (sscanf(snap[i].last_text, "%15s %15s %15s", tok1, tok2, tok3) < 2) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;  // not addressed to us
        if (strcmp(tok2, s_my_call) == 0) continue;  // avoid MYCALL MYCALL loops
        if (!tok3[0]) continue;                       // no third token
        if (!ft8_filter_match(snap[i].last_text, &qs.ft8_filters)) continue;
        if (snap[i].last_snr_db > best_snr) {
            best_snr = snap[i].last_snr_db;
            best_idx = i;
        }
    }

    if (best_idx < 0) return false;

    char tok1[16], tok2[16], tok3[16];
    tok3[0] = '\0';
    sscanf(snap[best_idx].last_text, "%15s %15s %15s", tok1, tok2, tok3);
    snprintf(caller_buf, caller_cap, "%s", tok2);
    if (caller_freq_out) *caller_freq_out = snap[best_idx].last_freq;
    if (caller_snr_out)  *caller_snr_out  = snap[best_idx].last_snr_db;
    if (strcmp(tok3, "RR73") == 0) { *got_rr73 = true; return true; }
    if (strcmp(tok3, "73")   == 0) { *got_73   = true; return true; }
    snprintf(report_buf, report_cap, "%s", tok3);
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
}

bool ft8_qso_start(const ft8_tx_request_t *tx1_req, char *err, size_t err_len)
{
    if (!tx1_req || !tx1_req->target_call[0]) {
        if (err) snprintf(err, err_len, "No target callsign");
        return false;
    }
    if (!load_my_call(err, err_len)) return false;

    char arm_err[64];
    if (!ft8_tx_arm(tx1_req, arm_err, sizeof(arm_err))) {
        if (err) snprintf(err, err_len, "%s", arm_err);
        return false;
    }

    // Earliest valid RX slot to scan: one slot after TX1 fires.
    int64_t now_sec  = (int64_t)time(NULL);
    int64_t tx1_slot = (now_sec / 15) * 15 + 15;
    if (tx1_req->use_parity) {
        while (slot_is_even(tx1_slot) != tx1_req->want_even_slot) tx1_slot += 15;
    }

    lock();
    s_state         = FT8_QSO_WAIT_RPT;
    strncpy(s_target, tx1_req->target_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_freq_hz       = tx1_req->audio_freq_hz;
    s_min_scan_utc  = tx1_slot + 15;
    s_missed_slots  = 0;
    s_from_cq       = false;
    s_cur_req       = *tx1_req;   // re-send TX1 each cycle until they reply
    s_have_cur      = true;
    s_have_cq_saved = false;
    // Pounce: we receive their report (RST_RCVD); we never give our own (RST_SENT = "599").
    strncpy(s_rst_sent, "599", sizeof(s_rst_sent));
    s_rst_rcvd[0] = '\0';
    unlock();

    ft8_status_set("QSO %s: TX1 sent - waiting for report", tx1_req->target_call);
    ESP_LOGI(TAG, "started QSO (pounce): %s @ %d Hz, min_scan=%lld",
             tx1_req->target_call, tx1_req->audio_freq_hz, (long long)(tx1_slot + 15));
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
        int64_t now_sec   = (int64_t)time(NULL);
        int64_t next_slot = (now_sec / 15) * 15 + 15;
        req_copy.use_parity     = true;
        req_copy.want_even_slot = (((next_slot / 15) % 2) == 0);
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
    // CQ-run: we give our report (RST_SENT set in cqrun_answer); they never give theirs (RST_RCVD = "599").
    s_rst_sent[0] = '\0';
    strncpy(s_rst_rcvd, "599", sizeof(s_rst_rcvd));
    unlock();

    ft8_status_set("CQ: calling - listening for answers");
    ESP_LOGI(TAG, "CQ loop started @ %d Hz", cq_req->audio_freq_hz);
    return true;
}

// Build + adopt our reply to a station that answered our CQ (CQ-run). They
// sent their grid (or a report); we answer with a signal report and wait for
// their roger. If they jumped straight to RR73/73, we just send 73.
static void cqrun_answer(const char *caller, int caller_freq, int caller_snr,
                         int64_t slot_sec, bool got_rr73, bool got_73)
{
    // Stay on our own CQ tone for the entire exchange — the answering station
    // uses their own separate tone; switching to caller_freq would put us on
    // their (occupied) frequency and trigger a false clash warning.
    lock();
    strncpy(s_target, caller, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    int our_freq = s_freq_hz;   // already set to our CQ tone in ft8_qso_start_cq()
    unlock();

    bool ok;
    if (got_rr73 || got_73) {
        ok = send_next(FT8_TX_KIND_73, caller, our_freq, slot_sec, "73",
                       FT8_QSO_WAIT_DONE);
        if (ok) ft8_status_set("QSO %s: sending 73", caller);
    } else {
        char rpt[8];
        fmt_report(caller_snr, rpt, sizeof(rpt));
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

void ft8_qso_advance(int64_t slot_sec)
{
    lock();
    ft8_qso_state_t st = s_state;
    char    target[FT8_CALL_MAX_LEN];
    int     freq     = s_freq_hz;
    int64_t min_scan = s_min_scan_utc;
    strncpy(target, s_target, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    unlock();

    if (st == FT8_QSO_IDLE || st == FT8_QSO_TIMEOUT) return;

    if (st == FT8_QSO_DONE) {
        lock(); s_state = FT8_QSO_IDLE; s_have_cur = false; unlock();
        ft8_status_set("Idle");
        return;
    }

    if (st == FT8_QSO_WAIT_DONE) {
        // Final (73/RR73) armed or fired; once it leaves the air we're done.
        if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_IDLE) {
            lock(); s_state = FT8_QSO_DONE; s_have_cur = false; unlock();
            ft8_status_set("QSO %s: complete!", target);
            ESP_LOGI(TAG, "QSO with %s complete", target);

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

            adif_qso_t qso = {
                .their_call = target,
                .my_call    = s_my_call,
                .my_grid    = qs.my_grid,
                .their_grid = their_grid,
                .freq_hz    = cat_get_frequency(),
                .mode       = "FT8",
                .rst_sent   = s_rst_sent,
                .rst_rcvd   = s_rst_rcvd,
                .qso_time   = time(NULL),
            };
            adif_log_record(&qso);
        }
        return;
    }

    // ---- CQ: listen for anyone answering us --------------------------------
    if (st == FT8_QSO_CQ) {
        char caller[FT8_CALL_MAX_LEN] = {0};
        int  caller_freq = freq;
        int  caller_snr  = 0;
        char report[8]   = {0};
        bool got_rr73 = false, got_73 = false;
        if (scan_for_reply_to_me(slot_sec, caller, sizeof(caller),
                                 &caller_freq, &caller_snr,
                                 report, sizeof(report), &got_rr73, &got_73)) {
            ESP_LOGI(TAG, "CQ: %s answered @ %d Hz snr=%d (rr73=%d 73=%d)",
                     caller, caller_freq, caller_snr, got_rr73, got_73);
            ft8_tx_disarm();   // cancel the re-armed CQ (no-op if already ACTIVE)
            cqrun_answer(caller, caller_freq, caller_snr, slot_sec, got_rr73, got_73);
        } else {
            // No answer - on_tx_complete keeps the CQ armed; idle fallback only.
            arm_current_if_idle();
        }
        return;
    }

    // ---- Exchange states: hear the partner on the opposite-parity slot -----
    // (pounce hasn't fired TX1 yet → nothing to hear; don't scan early)
    if (st == FT8_QSO_WAIT_RPT && slot_sec < min_scan) return;

    char report[8] = {0};
    bool got_rr73 = false, got_73 = false;
    bool found = scan_for_response(slot_sec, report, sizeof(report), &got_rr73, &got_73);

    if (st == FT8_QSO_WAIT_RPT) {
        // POUNCE: we sent our grid; expect their signal report.
        if (!found) { register_miss("waiting for report"); return; }

        bool ok;
        if (got_rr73 || got_73) {
            ESP_LOGI(TAG, "WAIT_RPT: %s sent RR73/73 directly", target);
            ok = send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73",
                           FT8_QSO_WAIT_DONE);
            if (ok) ft8_status_set("QSO %s: sending 73", target);
        } else {
            char roger[8];
            make_roger(report, roger, sizeof(roger));
            ESP_LOGI(TAG, "WAIT_RPT: %s reported %s -> TX2 %s", target, report, roger);
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
        arm_current_if_idle();
        return;
    }

    if (st == FT8_QSO_WAIT_ROGER) {
        // CQ-RUN: we sent a report; expect their R<report> (then we send RR73).
        if (!found) { register_miss("waiting for roger"); return; }

        if (got_rr73 || got_73) {
            if (send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73", FT8_QSO_WAIT_DONE))
                ft8_status_set("QSO %s: sending 73", target);
            arm_current_if_idle();
            return;
        }
        if (is_roger_token(report)) {
            if (send_next(FT8_TX_KIND_ROGER_RPT, target, freq, slot_sec, "RR73",
                          FT8_QSO_WAIT_DONE))
                ft8_status_set("QSO %s: rogered %s - sending RR73", target, report);
            arm_current_if_idle();
            return;
        }
        // They repeated their grid/report (didn't get ours): keep sending our
        // report, but count it so a dead exchange still times out.
        register_miss("re-sending report");
        return;
    }

    if (st == FT8_QSO_WAIT_RR73) {
        // POUNCE: we sent R<report>; expect RR73/73 (then we send 73).
        if (!found || (!got_rr73 && !got_73)) {
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
        arm_current_if_idle();
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
    s_state         = FT8_QSO_IDLE;
    s_target[0]     = '\0';
    s_have_cur      = false;
    s_have_cq_saved = false;
    s_from_cq       = false;
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
