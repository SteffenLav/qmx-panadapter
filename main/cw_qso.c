// CW ESM ladder + callsign capture. See cw_qso.h for the contract and
// docs/cw-page-design.md for the design.
//
// The capture works on a running character stream with decode errors — it
// deliberately has NO message framing. Heuristics, all cheap:
//   - tokens split on anything outside [A-Z0-9/];
//   - a plausible callsign has a letter AND a digit, length 3..10, and is
//     not a stopword (599/5NN/73-style tokens survive the letter+digit rule
//     otherwise);
//   - "DE" marks the next token as a SENDER call — the strongest signal;
//   - hearing our OWN call arms "they are addressing us";
//   - our call + DE + their call within a short window after our CQ =
//     auto-adopt (they answered us).
// A wrong capture costs one glance at the card; the operator taps to adopt,
// so precision beats recall everywhere here.

#include "cw_qso.h"
#include "storage/settings.h"
#include "adif/adif_log.h"
#include "cat/cat.h"
#include "ui/cw_screen_view.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cw_qso";

#define CALL_MAX        12
#define TOK_MAX         12
#define ADOPT_WINDOW_US (120LL * 1000000)   // our-CQ recency for auto-adopt
#define ADDR_WINDOW_US  (12LL * 1000000)    // our-call-heard -> DE <call> pairing window

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static char           s_my_call[CALL_MAX];
static cw_esm_state_t s_state = CW_ESM_CQ;
static char           s_target[CALL_MAX];
static char           s_heard[CALL_MAX];      // latest plausible non-target call
static int64_t        s_heard_us;
static char           s_rst_rcvd[4];
static int64_t        s_cq_sent_us;           // our last CQ went out
static int64_t        s_addressed_us;         // our own call last decoded
static bool           s_pending_auto_adopt;   // set by feed(), consumed on the LVGL tick
static char           s_pending_adopt_call[CALL_MAX];

// tokenizer state (feed context only)
static char   s_tok[TOK_MAX + 1];
static int    s_tok_len;
static bool   s_prev_was_de;

static bool is_stopword(const char *t)
{
    static const char *stop[] = {
        "CQ", "DE", "TU", "UR", "BK", "K", "KN", "EE", "E", "R", "RST",
        "5NN", "73", "88", "QRZ", "QTH", "PSE", "AGN", "HW", "POTA", "SOTA",
        "TEST", "FB", "GM", "GA", "GE", "OM", "RIG", "ANT", "WX", "ES",
    };
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); i++)
        if (strcmp(t, stop[i]) == 0) return true;
    return false;
}

static bool is_plausible_call(const char *t)
{
    size_t n = strlen(t);
    if (n < 3 || n > 10) return false;
    bool has_alpha = false, has_digit = false;
    for (size_t i = 0; i < n; i++) {
        if (isalpha((unsigned char)t[i])) has_alpha = true;
        else if (isdigit((unsigned char)t[i])) has_digit = true;
        else if (t[i] != '/') return false;
    }
    return has_alpha && has_digit && !is_stopword(t);
}

// "579" / "5NN" style report: 3 chars, RST ranges, N = cut 9.
static bool is_rst(const char *t, char out[4])
{
    if (strlen(t) != 3) return false;
    char r = t[0], s = t[1], tn = t[2];
    if (s == 'N') s = '9';
    if (tn == 'N') tn = '9';
    if (r < '1' || r > '5') return false;
    if (s < '1' || s > '9' || tn < '1' || tn > '9') return false;
    out[0] = r; out[1] = s; out[2] = tn; out[3] = '\0';
    return true;
}

// Runs inside the spinlock, feed context. No LVGL, no blocking calls.
static void process_token_locked(const char *tok, int64_t now)
{
    if (strcmp(tok, "DE") == 0) { s_prev_was_de = true; return; }
    bool was_de = s_prev_was_de;
    s_prev_was_de = false;

    if (s_my_call[0] && strcmp(tok, s_my_call) == 0) {
        s_addressed_us = now;
        return;
    }

    char rst[4];
    if (s_state != CW_ESM_CQ && s_target[0] && is_rst(tok, rst)) {
        memcpy(s_rst_rcvd, rst, sizeof(rst));
        return;
    }

    if (!is_plausible_call(tok)) return;

    // A senders-position call (after DE) is trusted more, but any plausible
    // call updates the card - the operator decides what to do with it.
    snprintf(s_heard, sizeof(s_heard), "%s", tok);
    s_heard_us = now;

    // Auto-adopt: they addressed US (our call heard moments ago), this is
    // the DE <call> that follows, and we are in a CQ run they could be
    // answering. Flagged here, applied on the LVGL tick (adoption touches
    // UI-facing state transitions and logs).
    if (s_state == CW_ESM_CQ && was_de &&
        s_cq_sent_us && (now - s_cq_sent_us) < ADOPT_WINDOW_US &&
        s_addressed_us && (now - s_addressed_us) < ADDR_WINDOW_US) {
        snprintf(s_pending_adopt_call, sizeof(s_pending_adopt_call), "%s", tok);
        s_pending_auto_adopt = true;
    }
}

void cw_qso_feed(const char *s, int n)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        char c = (char)toupper((unsigned char)s[i]);
        bool tok_char = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/';
        if (tok_char && s_tok_len < TOK_MAX) {
            s_tok[s_tok_len++] = c;
        } else if (s_tok_len > 0) {
            s_tok[s_tok_len] = '\0';
            process_token_locked(s_tok, now);
            s_tok_len = 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void cw_qso_init(void) { /* statics zero-init; nothing else */ }

void cw_qso_set_my_call(const char *call)
{
    portENTER_CRITICAL(&s_mux);
    s_my_call[0] = '\0';
    if (call) {
        snprintf(s_my_call, sizeof(s_my_call), "%s", call);
        for (char *p = s_my_call; *p; p++) *p = (char)toupper((unsigned char)*p);
    }
    portEXIT_CRITICAL(&s_mux);
}

void cw_qso_note_sent(const char *text)
{
    if (!text) return;
    // "CQ" as a standalone leading token marks a CQ run for auto-adoption.
    bool is_cq = (strncmp(text, "CQ", 2) == 0) && (text[2] == ' ' || text[2] == '\0');
    portENTER_CRITICAL(&s_mux);
    if (is_cq) s_cq_sent_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
}

cw_esm_state_t cw_qso_get_state(void)
{
    portENTER_CRITICAL(&s_mux);
    cw_esm_state_t st = s_state;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

// Bounded copy usable inside the spinlock (strncpy trips
// -Werror=stringop-truncation here, and snprintf is heavy for a critical
// section).
static void copy_locked(char *out, size_t out_sz, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < out_sz; i++) out[i] = src[i];
    out[i] = '\0';
}

void cw_qso_get_target(char *out, size_t out_sz)
{
    if (!out || !out_sz) return;
    portENTER_CRITICAL(&s_mux);
    copy_locked(out, out_sz, s_target);
    portEXIT_CRITICAL(&s_mux);
}

bool cw_qso_get_heard(char *out, size_t out_sz, int *age_s_out)
{
    if (!out || !out_sz) return false;
    portENTER_CRITICAL(&s_mux);
    bool have = s_heard[0] != '\0';
    copy_locked(out, out_sz, s_heard);
    int64_t age = have ? (esp_timer_get_time() - s_heard_us) : 0;
    // Consume a pending auto-adopt on this same LVGL-side poll.
    bool auto_adopt = s_pending_auto_adopt;
    char adopt_call[CALL_MAX];
    memcpy(adopt_call, s_pending_adopt_call, sizeof(adopt_call));
    s_pending_auto_adopt = false;
    portEXIT_CRITICAL(&s_mux);
    if (auto_adopt) {
        ESP_LOGI(TAG, "auto-adopt: %s answered our CQ", adopt_call);
        cw_qso_adopt(adopt_call);
    }
    if (age_s_out) *age_s_out = (int)(age / 1000000);
    return have;
}

void cw_qso_get_rst_rcvd(char *out, size_t out_sz)
{
    if (!out || !out_sz) return;
    portENTER_CRITICAL(&s_mux);
    copy_locked(out, out_sz, s_rst_rcvd);
    portEXIT_CRITICAL(&s_mux);
}

void cw_qso_adopt(const char *call)
{
    if (!call || !call[0]) return;
    portENTER_CRITICAL(&s_mux);
    snprintf(s_target, sizeof(s_target), "%s", call);
    s_rst_rcvd[0] = '\0';
    s_state = CW_ESM_EXCH;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "target adopted: %s", s_target);
}

void cw_qso_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    s_target[0]   = '\0';
    s_rst_rcvd[0] = '\0';
    s_state       = CW_ESM_CQ;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "ladder reset to CQ");
}

// Compose "<target> <memory text>" (memory raw, macros expanded later by the
// view's shared expander, which also handles {MYCALL}/{PARK}).
static bool compose(char *out, size_t out_sz, int mem_idx, bool prefix_target)
{
    qmx_settings_t s;
    settings_load_all(&s);
    const char *m = s.cw_msg[mem_idx];
    if (!m[0]) return false;
    char tgt[CALL_MAX];
    cw_qso_get_target(tgt, sizeof(tgt));
    if (prefix_target) {
        if (!tgt[0]) return false;
        snprintf(out, out_sz, "%s %s", tgt, m);
    } else {
        snprintf(out, out_sz, "%s", m);
    }
    return true;
}

bool cw_qso_next_text(char *out, size_t out_sz, const char *label_out[])
{
    static const char *labels[] = { "CQ", "report", "TU + log" };
    cw_esm_state_t st = cw_qso_get_state();
    if (label_out) *label_out = labels[st];
    switch (st) {
        case CW_ESM_CQ:   return compose(out, out_sz, 0, false);  // M1
        case CW_ESM_EXCH: return compose(out, out_sz, 2, true);   // <CALL> + M3
        case CW_ESM_TU:   return compose(out, out_sz, 3, true);   // <CALL> + M4
    }
    return false;
}

bool cw_qso_on_next_sent(void)
{
    cw_esm_state_t st = cw_qso_get_state();
    if (st == CW_ESM_EXCH) {
        portENTER_CRITICAL(&s_mux);
        s_state = CW_ESM_TU;
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    if (st == CW_ESM_TU) {
        // The TU is on its way: log the QSO. RST sent is genuinely 599 (it
        // is what memory M3 keys); RST rcvd only if the capture heard one -
        // never fabricated (the FT8 lesson, v1.3.4).
        qmx_settings_t s;
        settings_load_all(&s);
        char tgt[CALL_MAX], rst[4];
        cw_qso_get_target(tgt, sizeof(tgt));
        cw_qso_get_rst_rcvd(rst, sizeof(rst));
        adif_qso_t q = {
            .their_call = tgt,
            .my_call    = s.my_callsign,
            .my_grid    = s.my_grid,
            .their_grid = "",
            .freq_hz    = cat_get_frequency(),  // 0 with no radio = sim/test record
            .mode       = "CW",
            .rst_sent   = "599",
            .rst_rcvd   = rst,
            .qso_time   = time(NULL),
        };
        adif_log_record(&q);
        ESP_LOGI(TAG, "QSO logged: %s CW rst_rcvd='%s'", tgt, rst);
        cw_qso_reset();
        return true;
    }
    return false;   // CQ state: stay, keep calling
}

// ---- phantom practice station (sim mode) ------------------------------------
// Scripted W1AW answers whatever the ladder last sent, injected through the
// SAME path a real decode takes (cw_screen_feed_decoded), so capture/ESM/log
// are exercised end to end with no radio. Timing: replies land ~3 s after
// our send, roughly a real operator's turnaround.
static int64_t s_sim_reply_due_us = 0;
static int     s_sim_reply_kind   = 0;   // 1=answer CQ, 2=report, 3=final

void cw_qso_sim_arm(void)
{
    // Called by the view on a [SIM] send, BEFORE the ladder advances, so the
    // pre-send state says what the phantom should answer with: a CQ gets a
    // call-in, our report gets their report, our TU gets a courtesy final.
    cw_esm_state_t st = cw_qso_get_state();
    s_sim_reply_kind = (st == CW_ESM_CQ) ? 1 : (st == CW_ESM_EXCH) ? 2 : 3;
    s_sim_reply_due_us = esp_timer_get_time() + 3000000;
}

void cw_qso_sim_tick(void)
{
    if (!s_sim_reply_due_us || esp_timer_get_time() < s_sim_reply_due_us) return;
    s_sim_reply_due_us = 0;

    char my[CALL_MAX];
    portENTER_CRITICAL(&s_mux);
    memcpy(my, s_my_call, sizeof(my));
    portEXIT_CRITICAL(&s_mux);
    if (!my[0]) return;

    char msg[96];
    switch (s_sim_reply_kind) {
        case 1:  // they answer our CQ
            snprintf(msg, sizeof(msg), " %s %s DE W1AW W1AW K ", my, my);
            break;
        case 2:  // their report back (we just sent ours, state moved to TU)
            snprintf(msg, sizeof(msg), " %s DE W1AW UR 579 579 BK ", my);
            break;
        default: // courtesy final after our TU
            snprintf(msg, sizeof(msg), " TU 73 EE ");
            break;
    }
    ESP_LOGI(TAG, "[SIM] phantom sends: %s", msg);
    cw_screen_feed_decoded(msg, (int)strlen(msg));
}
