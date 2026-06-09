// v0.13.0: FT8 QSO state machine — auto search-and-pounce.
//
// Message exchange we drive:
//   TX1  <their_call> <my_call> <my_grid>        (reply to their CQ)
//   RX2  <my_call>   <their_call> <report>        (their signal report to us)
//   TX2  <their_call> <my_call> R<report>         (roger their report)
//   RX3  <my_call>   <their_call> RR73|73         (they're done)
//   TX3  <their_call> <my_call> 73                (sign off)
//
// Short-circuit: if they send RR73 in RX2 (skipping the plain report), jump
// straight to TX3.
//
// Timeout: QSO_TIMEOUT_SLOTS consecutive missed RX slots → abort.
//
// ft8_qso_start() takes a pre-built ft8_tx_request_t (already encoded by
// ft8_tx_build_request) so we never re-encode TX1 — the modal already has
// the validated, tone-encoded message.

#include "ft8_qso.h"
#include "ft8_tx.h"
#include "ft8_status.h"
#include "ui/ft8_screen.h"
#include "storage/settings.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_qso";

#define QSO_TIMEOUT_SLOTS  2

static SemaphoreHandle_t  s_lock;
static ft8_qso_state_t    s_state          = FT8_QSO_IDLE;
static char               s_target[FT8_CALL_MAX_LEN];   // their callsign
static char               s_my_call[FT8_CALL_MAX_LEN];  // our callsign (uppercased)
static int                s_freq_hz;
static int64_t            s_tx1_min_scan_utc; // don't scan before this (TX1 not fired yet)
static int                s_missed_slots;
static ft8_tx_request_t   s_cq_req;                     // saved for CQ loop re-arm

// ---------------------------------------------------------------------------

static inline bool slot_is_even(int64_t sec) { return ((sec / 15) % 2) == 0; }
static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

// Scan the ft8_screen table for a message FROM s_target TO s_my_call decoded
// in slot_sec. Fills one of report_buf / *got_rr73 / *got_73.
// Returns true if a matching message was found.
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

        // Message format: "<call_to> <call_de> <extra>"
        char tok1[16], tok2[16], tok3[16];
        tok3[0] = '\0';
        if (sscanf(snap[i].last_text, "%15s %15s %15s", tok1, tok2, tok3) < 2) continue;
        if (strcmp(tok1, s_my_call) != 0) continue; // not addressed to us
        if (strcmp(tok2, s_target)  != 0) continue; // not from them

        if (strcmp(tok3, "RR73") == 0) { *got_rr73 = true; return true; }
        if (strcmp(tok3, "73")   == 0) { *got_73   = true; return true; }
        if (tok3[0] != '\0') {
            strncpy(report_buf, tok3, report_cap - 1);
            report_buf[report_cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

// Scan the ft8_screen table for any message addressed TO s_my_call decoded
// in slot_sec — i.e. a reply to our CQ. Picks the entry with the best SNR
// when multiple stations reply in the same slot.
// Fills caller_buf, *caller_freq_out, and one of report_buf / *got_rr73 / *got_73.
// Returns true if a usable reply was found.
static bool scan_for_reply_to_me(int64_t slot_sec,
                                  char *caller_buf, size_t caller_cap,
                                  int  *caller_freq_out,
                                  char *report_buf,  size_t report_cap,
                                  bool *got_rr73, bool *got_73)
{
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    int     best_idx = -1;
    int16_t best_snr = INT16_MIN;

    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec) continue;
        char tok1[16], tok2[16], tok3[16];
        tok3[0] = '\0';
        if (sscanf(snap[i].last_text, "%15s %15s %15s", tok1, tok2, tok3) < 2) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;  // not addressed to us
        if (strcmp(tok2, s_my_call) == 0) continue;  // avoid MYCALL MYCALL loops
        if (!tok3[0]) continue;                       // no third token — not a valid reply
        if (snap[i].last_snr_db > best_snr) {
            best_snr = snap[i].last_snr_db;
            best_idx = i;
        }
    }

    if (best_idx < 0) return false;

    char tok1[16], tok2[16], tok3[16];
    tok3[0] = '\0';
    sscanf(snap[best_idx].last_text, "%15s %15s %15s", tok1, tok2, tok3);
    strncpy(caller_buf, tok2, caller_cap - 1);
    caller_buf[caller_cap - 1] = '\0';
    if (caller_freq_out) *caller_freq_out = snap[best_idx].last_freq;
    if (strcmp(tok3, "RR73") == 0) { *got_rr73 = true; return true; }
    if (strcmp(tok3, "73")   == 0) { *got_73   = true; return true; }
    strncpy(report_buf, tok3, report_cap - 1);
    report_buf[report_cap - 1] = '\0';
    return true;
}

// Build "R<report>" for TX2. Their report is e.g. "-10".
// If it already starts with 'R' (e.g. "RRR"), pass it through.
static void make_roger(const char *their_report, char *out, size_t len)
{
    if (their_report[0] == 'R') {
        snprintf(out, len, "%s", their_report);
    } else {
        snprintf(out, len, "R%s", their_report);
    }
}

// ---------------------------------------------------------------------------

void ft8_qso_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_state     = FT8_QSO_IDLE;
    s_target[0] = '\0';
}

bool ft8_qso_start(const ft8_tx_request_t *tx1_req, char *err, size_t err_len)
{
    if (!tx1_req || !tx1_req->target_call[0]) {
        if (err) snprintf(err, err_len, "No target callsign");
        return false;
    }

    // Cache + uppercase our callsign for message scanning
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

    // Arm TX1 — includes Digi-mode pre-flight (~0-1 s)
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
    s_state            = FT8_QSO_WAIT_RPT;
    strncpy(s_target, tx1_req->target_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_freq_hz          = tx1_req->audio_freq_hz;
    s_tx1_min_scan_utc = tx1_slot + 15;
    s_missed_slots     = 0;
    unlock();

    ft8_status_set("QSO %s: TX1 sent — waiting for report", tx1_req->target_call);
    ESP_LOGI(TAG, "started QSO: %s @ %d Hz, min_scan=%lld",
             tx1_req->target_call, tx1_req->audio_freq_hz,
             (long long)(tx1_slot + 15));
    return true;
}

bool ft8_qso_start_cq(const ft8_tx_request_t *cq_req, char *err, size_t err_len)
{
    if (!cq_req) {
        if (err) snprintf(err, err_len, "No CQ request");
        return false;
    }

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

    char arm_err[64];
    if (!ft8_tx_arm(cq_req, arm_err, sizeof(arm_err))) {
        if (err) snprintf(err, err_len, "%s", arm_err);
        return false;
    }

    lock();
    s_state        = FT8_QSO_CQ;
    s_target[0]    = '\0';
    s_freq_hz      = cq_req->audio_freq_hz;
    s_cq_req       = *cq_req;
    s_missed_slots = 0;
    unlock();

    ft8_status_set("CQ: armed — looping until answered");
    ESP_LOGI(TAG, "CQ loop started @ %d Hz", cq_req->audio_freq_hz);
    return true;
}

void ft8_qso_advance(int64_t slot_sec)
{
    lock();
    ft8_qso_state_t st = s_state;
    char   target[FT8_CALL_MAX_LEN];
    int    freq    = s_freq_hz;
    int64_t min_scan = s_tx1_min_scan_utc;
    strncpy(target, s_target, sizeof(target));
    unlock();

    if (st == FT8_QSO_IDLE || st == FT8_QSO_TIMEOUT) return;

    if (st == FT8_QSO_DONE) {
        // Shown for one slot, then auto-return to idle
        lock(); s_state = FT8_QSO_IDLE; unlock();
        ft8_status_set("Idle");
        return;
    }

    if (st == FT8_QSO_WAIT_DONE) {
        // TX3 is armed; wait for it to fire (ft8_tx goes IDLE)
        char dummy[4]; int dummy_secs;
        ft8_tx_state_t tx_st = ft8_tx_get_status(dummy, sizeof(dummy), &dummy_secs);
        if (tx_st == FT8_TX_IDLE) {
            lock(); s_state = FT8_QSO_DONE; unlock();
            ft8_status_set("QSO %s: complete!", target);
            ESP_LOGI(TAG, "QSO with %s complete", target);
        }
        return;
    }

    // CQ loop — re-arm after every unanswered TX slot; stop when someone replies
    if (st == FT8_QSO_CQ) {
        // Only act once the CQ has actually fired (TX idle)
        if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) return;

        char caller[FT8_CALL_MAX_LEN] = {0};
        int  caller_freq = freq;
        char report[8]   = {0};
        bool got_rr73    = false;
        bool got_73      = false;
        bool found = scan_for_reply_to_me(slot_sec,
                                          caller, sizeof(caller), &caller_freq,
                                          report, sizeof(report),
                                          &got_rr73, &got_73);
        if (!found) {
            // No reply — re-arm CQ for the next matching slot
            ft8_tx_request_t cq_copy;
            lock(); cq_copy = s_cq_req; unlock();
            char arm_err[64];
            if (ft8_tx_arm(&cq_copy, arm_err, sizeof(arm_err))) {
                ft8_status_set("CQ: no reply — trying again");
                ESP_LOGI(TAG, "CQ: no reply in slot %lld, re-arming", (long long)slot_sec);
            } else {
                ESP_LOGW(TAG, "CQ re-arm failed: %s", arm_err);
                ft8_status_set("CQ: re-arm error");
            }
            return;
        }

        // Someone replied — start the exchange on their frequency
        ESP_LOGI(TAG, "CQ: reply from %s @ %d Hz, report='%s' rr73=%d 73=%d",
                 caller, caller_freq, report, got_rr73, got_73);
        lock();
        strncpy(s_target, caller, sizeof(s_target) - 1);
        s_target[sizeof(s_target) - 1] = '\0';
        s_freq_hz = caller_freq;
        unlock();

        ft8_tx_request_t req;
        char arm_err[64];
        bool armed;
        ft8_qso_state_t next_state;

        if (got_rr73 || got_73) {
            armed = ft8_tx_build_request(FT8_TX_KIND_73, caller, caller_freq,
                                         slot_sec, "73", &req, arm_err, sizeof(arm_err))
                 && ft8_tx_arm(&req, arm_err, sizeof(arm_err));
            next_state = FT8_QSO_WAIT_DONE;
            if (armed) ft8_status_set("QSO %s: sending 73", caller);
        } else {
            char roger[8];
            make_roger(report, roger, sizeof(roger));
            armed = ft8_tx_build_request(FT8_TX_KIND_ROGER_RPT, caller, caller_freq,
                                         slot_sec, roger, &req, arm_err, sizeof(arm_err))
                 && ft8_tx_arm(&req, arm_err, sizeof(arm_err));
            next_state = FT8_QSO_WAIT_RR73;
            if (armed) ft8_status_set("QSO %s: heard %s — sending %s",
                                      caller, report, roger);
        }

        if (armed) {
            lock(); s_state = next_state; s_missed_slots = 0; unlock();
        } else {
            ESP_LOGE(TAG, "CQ: failed to arm response to %s: %s", caller, arm_err);
            // Stay in CQ loop; re-arm CQ and try again next slot
            ft8_tx_request_t cq_copy;
            lock(); cq_copy = s_cq_req; unlock();
            ft8_tx_arm(&cq_copy, arm_err, sizeof(arm_err));
            ft8_status_set("CQ: TX error — retrying");
        }
        return;
    }

    // WAIT_RPT / WAIT_RR73 — TX1 must have fired before we start scanning
    if (slot_sec < min_scan) return;

    char report[8]  = {0};
    bool got_rr73   = false;
    bool got_73     = false;
    bool found = scan_for_response(slot_sec, report, sizeof(report), &got_rr73, &got_73);

    if (st == FT8_QSO_WAIT_RPT) {
        if (!found) {
            lock(); s_missed_slots++; int m = s_missed_slots; unlock();
            ESP_LOGW(TAG, "WAIT_RPT: no response from %s (%d/%d)",
                     target, m, QSO_TIMEOUT_SLOTS);
            if (m >= QSO_TIMEOUT_SLOTS) {
                lock(); s_state = FT8_QSO_TIMEOUT; unlock();
                ft8_tx_disarm();
                ft8_status_set("QSO %s: no response — timeout", target);
            } else {
                ft8_status_set("QSO %s: waiting for report (%d/%d)...",
                               target, m, QSO_TIMEOUT_SLOTS);
            }
            return;
        }

        ft8_tx_request_t req;
        char arm_err[64];
        bool armed;
        ft8_qso_state_t next_state;

        if (got_rr73 || got_73) {
            // Skipped straight to sign-off — send 73
            ESP_LOGI(TAG, "WAIT_RPT: %s sent RR73/73 directly", target);
            armed = ft8_tx_build_request(FT8_TX_KIND_73, target, freq,
                                         slot_sec, "73", &req, arm_err, sizeof(arm_err))
                 && ft8_tx_arm(&req, arm_err, sizeof(arm_err));
            next_state = FT8_QSO_WAIT_DONE;
            if (armed) ft8_status_set("QSO %s: sending 73", target);
        } else {
            char roger[8];
            make_roger(report, roger, sizeof(roger));
            ESP_LOGI(TAG, "WAIT_RPT: %s reported %s → TX2 %s", target, report, roger);
            armed = ft8_tx_build_request(FT8_TX_KIND_ROGER_RPT, target, freq,
                                         slot_sec, roger, &req, arm_err, sizeof(arm_err))
                 && ft8_tx_arm(&req, arm_err, sizeof(arm_err));
            next_state = FT8_QSO_WAIT_RR73;
            if (armed) ft8_status_set("QSO %s: heard %s — sending %s",
                                      target, report, roger);
        }

        if (armed) {
            lock(); s_state = next_state; s_missed_slots = 0; unlock();
        } else {
            ESP_LOGE(TAG, "failed to arm next TX: %s", arm_err);
            lock(); s_state = FT8_QSO_TIMEOUT; unlock();
            ft8_status_set("QSO %s: TX error — %s", target, arm_err);
        }

    } else if (st == FT8_QSO_WAIT_RR73) {
        if (!found || (!got_rr73 && !got_73)) {
            lock(); s_missed_slots++; int m = s_missed_slots; unlock();
            ESP_LOGW(TAG, "WAIT_RR73: no RR73 from %s (%d/%d)",
                     target, m, QSO_TIMEOUT_SLOTS);
            if (m >= QSO_TIMEOUT_SLOTS) {
                lock(); s_state = FT8_QSO_TIMEOUT; unlock();
                ft8_tx_disarm();
                ft8_status_set("QSO %s: no RR73 — timeout", target);
            } else {
                ft8_status_set("QSO %s: waiting for RR73 (%d/%d)...",
                               target, m, QSO_TIMEOUT_SLOTS);
            }
            return;
        }

        ESP_LOGI(TAG, "WAIT_RR73: %s sent RR73/73 — arming TX3", target);
        ft8_tx_request_t req;
        char arm_err[64];
        bool armed = ft8_tx_build_request(FT8_TX_KIND_73, target, freq,
                                          slot_sec, "73", &req, arm_err, sizeof(arm_err))
                  && ft8_tx_arm(&req, arm_err, sizeof(arm_err));
        if (armed) {
            lock(); s_state = FT8_QSO_WAIT_DONE; s_missed_slots = 0; unlock();
            ft8_status_set("QSO %s: sending 73...", target);
        } else {
            ESP_LOGE(TAG, "failed to arm TX3: %s", arm_err);
            lock(); s_state = FT8_QSO_TIMEOUT; unlock();
            ft8_status_set("QSO %s: TX error — %s", target, arm_err);
        }
    }
}

void ft8_qso_abort(void)
{
    lock();
    char target[FT8_CALL_MAX_LEN];
    strncpy(target, s_target, sizeof(target));
    s_state     = FT8_QSO_IDLE;
    s_target[0] = '\0';
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
