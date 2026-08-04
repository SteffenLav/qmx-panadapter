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
#include "esp_random.h"
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

// Copilot hint: the newest interpretation of the decode stream. Written in
// feed context under the same spinlock, read by the LVGL tick.
static cw_hint_t s_hint_kind = CW_HINT_NONE;
static char      s_hint_subj[CALL_MAX];
static int64_t   s_hint_us;
static int64_t   s_cq_heard_us;   // a CQ token went by (pairs with "DE <call>")
static int64_t   s_pota_heard_us; // "POTA"/"SOTA" went by (marks an activation)
static char      s_heard_park[12];// their park reference, "" if none heard
static int64_t   s_heard_park_us;

#define CQ_PAIR_WINDOW_US (10LL * 1000000)  // CQ -> DE <call> pairing window
#define HINT_STICKY_US    (10LL * 1000000)  // a strong read holds for repeats

// Bounded copy for use INSIDE a spinlock. snprintf must never run with
// interrupts off on a path that repeats: the MIPI-DSI frame-restart ISR runs
// on core 0 and a multi-ms interrupts-off window makes the panel drop a frame
// and blank cyan (CLAUDE.md "No long interrupts-off critical sections").
// Hint classification put snprintf inside this lock and the simulator then
// drove it every few seconds - the operator saw exactly that cyan flash,
// 2026-08-04.
static void copy_small(char *dst, size_t dst_sz, const char *src)
{
    size_t i = 0;
    if (!dst || dst_sz == 0) return;
    if (src) for (; src[i] && i + 1 < dst_sz; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void set_hint_locked(cw_hint_t kind, const char *subj, int64_t now)
{
    s_hint_kind = kind;
    copy_small(s_hint_subj, sizeof(s_hint_subj), subj);
    s_hint_us = now;
}

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

// A POTA park reference: 1-2 letters, an optional hyphen, then 4-5 digits -
// "US-1211", "K-1234", "DL-0123", and the hyphen-less forms some operators
// send to save time. Deliberately requires the long digit run so it can never
// swallow a callsign (W1AW, SM5ABC) or a report.
static bool is_park_ref(const char *t)
{
    size_t i = 0, n = strlen(t);
    if (n < 5 || n > 9) return false;
    while (i < n && isalpha((unsigned char)t[i])) i++;
    if (i < 1 || i > 2) return false;
    if (t[i] == '-') i++;
    size_t digits = 0;
    for (; i < n; i++, digits++)
        if (!isdigit((unsigned char)t[i])) return false;
    return digits >= 4 && digits <= 5;
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

    // Copilot hints for the non-callsign tokens. Ordered weakest-first so a
    // stronger signal later in the same transmission overwrites: hearing
    // "CQ ... DE W1AW" ends on the call, and "<us> DE W1AW" ends on
    // ANSWERED_YOU, which is what the operator most needs to see.
    if (strcmp(tok, "CQ") == 0)  {
        s_cq_heard_us = now;
        set_hint_locked(CW_HINT_CALLING_CQ, NULL, now);
        return;
    }
    // "POTA" (or SOTA) next to a CQ marks a park activation - worth saying so,
    // because it tells the operator this is a station worth working and that a
    // park reference is coming.
    if (strcmp(tok, "POTA") == 0 || strcmp(tok, "SOTA") == 0) {
        s_pota_heard_us = now;
        return;
    }
    if (is_park_ref(tok)) {
        copy_small(s_heard_park, sizeof(s_heard_park), tok);
        s_heard_park_us = now;
        set_hint_locked(CW_HINT_PARK, tok, now);
        return;
    }
    if (strcmp(tok, "QRZ") == 0) { set_hint_locked(CW_HINT_QRZ, NULL, now); return; }
    if (strcmp(tok, "AGN") == 0 || strcmp(tok, "PSE") == 0) {
        set_hint_locked(CW_HINT_REPEAT, NULL, now); return;
    }
    if (strcmp(tok, "73") == 0 || strcmp(tok, "88") == 0 || strcmp(tok, "TU") == 0) {
        set_hint_locked(CW_HINT_SIGNOFF, NULL, now); return;
    }

    if (s_my_call[0] && strcmp(tok, s_my_call) == 0) {
        s_addressed_us = now;
        set_hint_locked(CW_HINT_YOUR_CALL, NULL, now);
        return;
    }

    char rst[4];
    if (is_rst(tok, rst)) {
        // Report the RST to the operator whatever the ladder state - hearing
        // one before adopting is exactly when a beginner needs telling what
        // it was. Only STORE it for the log once a target is being worked.
        set_hint_locked(CW_HINT_REPORT, rst, now);
        if (s_state != CW_ESM_CQ && s_target[0]) memcpy(s_rst_rcvd, rst, sizeof(rst));
        return;
    }

    if (!is_plausible_call(tok)) return;

    // A senders-position call (after DE) is trusted more, but any plausible
    // call updates the card - the operator decides what to do with it.
    copy_small(s_heard, sizeof(s_heard), tok);
    s_heard_us = now;

    // Classify this call. Strongest first:
    //   answering us  - our call, then "DE <them>"        -> act now
    //   calling CQ    - a CQ, then "DE <them>"            -> you may answer
    //   heard         - anything else plausible
    bool answering_us = was_de && s_addressed_us &&
                        (now - s_addressed_us) < ADDR_WINDOW_US;
    bool cq_sender    = was_de && s_cq_heard_us &&
                        (now - s_cq_heard_us) < CQ_PAIR_WINDOW_US;
    bool pota_sender  = cq_sender && s_pota_heard_us &&
                        (now - s_pota_heard_us) < CQ_PAIR_WINDOW_US;
    cw_hint_t k = answering_us ? CW_HINT_ANSWERED_YOU
                : pota_sender  ? CW_HINT_CQ_POTA
                : cq_sender    ? CW_HINT_CQ_FROM
                               : CW_HINT_CALL_HEARD;
    // A station repeating its own call ("DE W1AW W1AW", standard practice)
    // must not downgrade the read: only the FIRST of the repeats follows the
    // "DE", so the later ones would otherwise demote a strong classification
    // to a bare "heard a callsign" (caught by the self-test).
    bool same_subject_recent = (strcmp(s_hint_subj, tok) == 0) &&
                               (now - s_hint_us) < HINT_STICKY_US;
    // NOTE: every "strong" call hint must be listed here. Adding CW_HINT_CQ_POTA
    // beside CQ_FROM without adding it to this guard let the repeated callsign
    // demote it again - the exact bug the guard exists to prevent, caught by
    // the self-test 2026-08-04.
    bool would_downgrade = (k == CW_HINT_CALL_HEARD) &&
                           (s_hint_kind == CW_HINT_ANSWERED_YOU ||
                            s_hint_kind == CW_HINT_CQ_FROM ||
                            s_hint_kind == CW_HINT_CQ_POTA);
    if (same_subject_recent && would_downgrade) s_hint_us = now;  // keep, refresh age
    else                                        set_hint_locked(k, tok, now);

    // Auto-adopt: they addressed US (our call heard moments ago), this is
    // the DE <call> that follows, and we are in a CQ run they could be
    // answering. Flagged here, applied on the LVGL tick (adoption touches
    // UI-facing state transitions and logs).
    if (s_state == CW_ESM_CQ && was_de &&
        s_cq_sent_us && (now - s_cq_sent_us) < ADOPT_WINDOW_US &&
        s_addressed_us && (now - s_addressed_us) < ADDR_WINDOW_US) {
        copy_small(s_pending_adopt_call, sizeof(s_pending_adopt_call), tok);
        s_pending_auto_adopt = true;
    }
}

// ---- hint self-test ---------------------------------------------------------
// The live path needs a radio keying real CW and a finger on the screen, so
// the classifier is verified here instead: canned decode text in, expected
// hint out. Runs once at boot and restores every static it touches.
void cw_qso_hint_selftest(void)
{
    struct { const char *decoded; cw_hint_t want; const char *want_subj; } cases[] = {
        // A CQ call resolves to WHO is calling it - that is what the
        // operator acts on. Both cases repeat the callsign the way real CW
        // does; the repeat must not demote the classification (this pair is
        // the regression test for exactly that bug, found here 2026-08-04).
        { "CQ CQ DE W1AW W1AW K",     CW_HINT_CQ_FROM,      "W1AW" },
        { "OZ1LAV DE W1AW W1AW",      CW_HINT_ANSWERED_YOU, "W1AW" },
        // A bare CQ with no sender yet still says something useful.
        { "CQ CQ CQ",                 CW_HINT_CALLING_CQ,   ""     },
        // A call with no DE and no context is just a sighting.
        { "SM5ABC",                   CW_HINT_CALL_HEARD,   "SM5ABC" },
        // POTA: a park activation resolves to who is activating, and a park
        // reference is called out. Both hyphenated and bare forms, and the
        // reference must survive the tokenizer in one piece.
        { "CQ POTA DE W1AW W1AW K",   CW_HINT_CQ_POTA,      "W1AW" },
        { "UR 599 599 US-1211 BK",    CW_HINT_PARK,         "US-1211" },
        { "QTH K-4521 HR",            CW_HINT_PARK,         "K-4521" },
        // Bare (hyphen-less) reference, and NOT ending on a token that would
        // legitimately produce a different hint - "... TU" really does mean
        // SIGNOFF, which is what the first version of this case got wrong.
        { "MY PARK US1211",           CW_HINT_PARK,         "US1211" },
        // Our call alone (someone working us mid-exchange, or a tail-end).
        { "OZ1LAV K",                 CW_HINT_YOUR_CALL,    ""     },
        // Reports, plain and cut-number.
        { "UR 599 599 BK",            CW_HINT_REPORT,       "599"  },
        { "UR 5NN 5NN",               CW_HINT_REPORT,       "599"  },
        // Sign-off and service tokens.
        { "TU 73 EE",                 CW_HINT_SIGNOFF,      ""     },
        { "AGN AGN BK",               CW_HINT_REPEAT,       ""     },
        { "QRZ DE",                   CW_HINT_QRZ,          ""     },
    };

    // Save everything the tokenizer/classifier touches.
    portENTER_CRITICAL(&s_mux);
    char saved_call[CALL_MAX]; memcpy(saved_call, s_my_call, sizeof(saved_call));
    cw_esm_state_t saved_state = s_state;
    char saved_target[CALL_MAX]; memcpy(saved_target, s_target, sizeof(saved_target));
    char saved_heard[CALL_MAX];  memcpy(saved_heard, s_heard, sizeof(saved_heard));
    char saved_rst[4];           memcpy(saved_rst, s_rst_rcvd, sizeof(saved_rst));
    int64_t saved_cq = s_cq_sent_us, saved_addr = s_addressed_us;
    bool saved_pending = s_pending_auto_adopt;
    snprintf(s_my_call, sizeof(s_my_call), "OZ1LAV");
    s_state = CW_ESM_CQ; s_target[0] = '\0';
    portEXIT_CRITICAL(&s_mux);

    int pass = 0, fail = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        portENTER_CRITICAL(&s_mux);
        s_hint_kind = CW_HINT_NONE; s_hint_subj[0] = '\0'; s_hint_us = 0;
        s_prev_was_de = false; s_tok_len = 0; s_addressed_us = 0;
        s_cq_heard_us = 0; s_pota_heard_us = 0; s_heard_park[0] = '\0';
        portEXIT_CRITICAL(&s_mux);

        cw_qso_feed(cases[i].decoded, (int)strlen(cases[i].decoded));
        cw_qso_feed(" ", 1);            // flush the final token

        cw_hint_t got; char subj[CALL_MAX]; char line[96];
        cw_qso_get_hint(&got, subj, sizeof(subj), NULL);
        bool ok = (got == cases[i].want) && (strcmp(subj, cases[i].want_subj) == 0);
        cw_qso_hint_text(got, subj, line, sizeof(line));
        if (ok) { pass++; ESP_LOGI(TAG, "hint selftest PASS '%s' -> \"%s\"", cases[i].decoded, line); }
        else    { fail++; ESP_LOGE(TAG, "hint selftest FAIL '%s' -> kind=%d subj='%s' (want kind=%d subj='%s')",
                                   cases[i].decoded, (int)got, subj,
                                   (int)cases[i].want, cases[i].want_subj); }
    }

    portENTER_CRITICAL(&s_mux);
    memcpy(s_my_call, saved_call, sizeof(s_my_call));
    s_state = saved_state;
    memcpy(s_target, saved_target, sizeof(s_target));
    memcpy(s_heard, saved_heard, sizeof(s_heard));
    memcpy(s_rst_rcvd, saved_rst, sizeof(s_rst_rcvd));
    s_cq_sent_us = saved_cq; s_addressed_us = saved_addr;
    s_pending_auto_adopt = saved_pending;
    s_hint_kind = CW_HINT_NONE; s_hint_subj[0] = '\0'; s_hint_us = 0;
    s_prev_was_de = false; s_tok_len = 0; s_cq_heard_us = 0;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "hint selftest: %d passed, %d failed", pass, fail);
}

bool cw_qso_get_hint(cw_hint_t *kind_out, char *subj_out, size_t subj_sz,
                     int *age_s_out)
{
    portENTER_CRITICAL(&s_mux);
    cw_hint_t k = s_hint_kind;
    int64_t   ts = s_hint_us;
    if (subj_out && subj_sz) copy_small(subj_out, subj_sz, s_hint_subj);
    portEXIT_CRITICAL(&s_mux);
    if (kind_out) *kind_out = k;
    if (age_s_out) *age_s_out = ts ? (int)((esp_timer_get_time() - ts) / 1000000) : 0;
    return k != CW_HINT_NONE;
}

const char *cw_qso_hint_text(cw_hint_t kind, const char *subj,
                             char *buf, size_t buf_sz)
{
    const char *s = (subj && subj[0]) ? subj : "";
    switch (kind) {
    case CW_HINT_CALLING_CQ:
        snprintf(buf, buf_sz, "Someone is calling CQ - you may answer");
        break;
    case CW_HINT_CQ_FROM:
        snprintf(buf, buf_sz, "%s is calling CQ - tap the card to answer", s);
        break;
    case CW_HINT_CQ_POTA:
        snprintf(buf, buf_sz, "%s is activating a park - tap the card to work them", s);
        break;
    case CW_HINT_PARK:
        snprintf(buf, buf_sz, "That was their park reference: %s", s);
        break;
    case CW_HINT_ANSWERED_YOU:
        snprintf(buf, buf_sz, "%s is answering YOU - tap the card to work them", s);
        break;
    case CW_HINT_YOUR_CALL:
        snprintf(buf, buf_sz, "That was YOUR callsign");
        break;
    case CW_HINT_REPORT:
        snprintf(buf, buf_sz, "That was a signal report: %s", s);
        break;
    case CW_HINT_SIGNOFF:
        snprintf(buf, buf_sz, "They are signing off (73 / TU)");
        break;
    case CW_HINT_REPEAT:
        snprintf(buf, buf_sz, "They want a repeat - send it again");
        break;
    case CW_HINT_QRZ:
        snprintf(buf, buf_sz, "QRZ - they are asking who called");
        break;
    case CW_HINT_CALL_HEARD:
        snprintf(buf, buf_sz, "Heard a callsign: %s", s);
        break;
    case CW_HINT_NONE:
    default:
        if (buf_sz) buf[0] = '\0';
        break;
    }
    return buf;
}

void cw_qso_feed(const char *s, int n)
{
    int64_t now = esp_timer_get_time();
    // ONE SHORT critical section per character, not one across the whole
    // message. The old single-lock loop held interrupts off for an entire
    // transmission (~40 characters plus a token classification per word),
    // which is the multi-ms window that makes the DSI panel drop a frame and
    // flash cyan - and the practice simulator now feeds text every few
    // seconds, so it fired regularly (operator-observed 2026-08-04). The
    // per-character split keeps every window to a few instructions except at
    // a token boundary, and the classification inside it is now allocation-
    // and format-free (see copy_small).
    for (int i = 0; i < n; i++) {
        char c = (char)toupper((unsigned char)s[i]);
        // '-' is a token character so POTA references survive as one token
        // ("US-1211", not "US" + "1211"). It can never be mistaken for a
        // callsign: is_plausible_call() rejects anything outside [A-Z0-9/].
        bool tok_char = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '/' || c == '-';
        portENTER_CRITICAL(&s_mux);
        if (tok_char && s_tok_len < TOK_MAX) {
            s_tok[s_tok_len++] = c;
        } else if (s_tok_len > 0) {
            s_tok[s_tok_len] = '\0';
            process_token_locked(s_tok, now);
            s_tok_len = 0;
        }
        portEXIT_CRITICAL(&s_mux);
    }
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

bool cw_qso_get_heard_park(char *out, size_t out_sz)
{
    portENTER_CRITICAL(&s_mux);
    bool have = s_heard_park[0] != '\0';
    if (out && out_sz) copy_small(out, out_sz, s_heard_park);
    portEXIT_CRITICAL(&s_mux);
    return have;
}

void cw_qso_clear_capture(void)
{
    portENTER_CRITICAL(&s_mux);
    s_heard[0]      = '\0';
    s_heard_us      = 0;
    s_heard_park[0] = '\0';
    s_heard_park_us = 0;
    s_pota_heard_us = 0;
    s_hint_kind    = CW_HINT_NONE;
    s_hint_subj[0] = '\0';
    s_hint_us      = 0;
    s_cq_heard_us  = 0;
    s_addressed_us = 0;
    s_cq_sent_us   = 0;
    s_prev_was_de  = false;
    s_tok_len      = 0;
    s_pending_auto_adopt = false;
    portEXIT_CRITICAL(&s_mux);
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
// Phantoms answer whatever the ladder last sent, injected through the SAME
// path a real decode takes (cw_screen_feed_decoded), so capture/ESM/log are
// exercised end to end with no radio. Timing: replies land ~3 s after our
// send, roughly a real operator's turnaround.
//
// The copy is deliberately IMPERFECT. A real QMX decoder drops characters,
// mis-times elements into neighbouring letters, and turns band noise into a
// steady dribble of E/T/I/M (hardware-observed: "TB002 T;" arriving all
// session with no signal present). Feeding the capture and copilot layers
// clean text flattered them - the heuristics have to survive what the radio
// actually produces, and the operator has to learn to read through it.
static int64_t s_sim_reply_due_us = 0;
static int     s_sim_reply_kind   = 0;   // 1=answer CQ, 2=report, 3=final
static int64_t s_sim_noise_due_us = 0;   // idle band noise
static int64_t s_sim_cq_due_us    = 0;   // an unsolicited CQ to pounce on
static char    s_sim_peer[CALL_MAX] = "W1AW";   // who is working us now

// Calls the phantoms draw from. Mixed prefixes so the station card's DXCC
// lookup and the worked-before check get exercised too.
static const char *SIM_CALLS[] = { "W1AW", "SM5ABC", "G3XYZ", "JA1TST", "VE3ABC" };

static uint32_t rnd_pct(void) { return esp_random() % 100; }

// A mis-timed element yields a NEIGHBOURING character, not a random one -
// that is what makes real decoder output readable-but-wrong.
static char confuse(char c)
{
    switch (c) {
    case 'E': return 'I';  case 'I': return 'E';
    case 'T': return 'M';  case 'M': return 'T';
    case 'A': return 'W';  case 'W': return 'A';
    case 'N': return 'D';  case 'D': return 'N';
    case 'S': return 'H';  case 'H': return 'S';
    case 'U': return 'V';  case 'V': return 'U';
    case 'K': return 'C';  case 'C': return 'K';
    case 'R': return 'L';  case 'L': return 'R';
    case 'O': return 'M';  case 'G': return 'Q';
    case '5': return 'S';  case '0': return 'T';  case '9': return 'N';
    default:  return c;
    }
}

// Corrupt one transmission the way a marginal copy would arrive. Rates are
// deliberately modest: a 4-character call survives intact ~75% of the time,
// and real CW repeats the call twice, so capture usually still gets it -
// while the operator sees enough damage to have to think.
static void sim_corrupt(char *dst, size_t dst_sz, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < dst_sz; p++) {
        if (*p == ' ') { dst[o++] = ' '; continue; }
        uint32_t r = rnd_pct();
        if (r < 6)  continue;                            // element missed entirely
        if (r < 12) { dst[o++] = confuse(*p); continue; } // mis-timed
        dst[o++] = *p;
        if (r >= 98) dst[o++] = confuse(*p);              // element split in two
    }
    dst[o] = '\0';
}

static void sim_feed(const char *what, const char *clean)
{
    char dirty[128];
    sim_corrupt(dirty, sizeof(dirty), clean);
    ESP_LOGI(TAG, "[SIM] %s: '%s' -> copied as '%s'", what, clean, dirty);
    cw_screen_feed_decoded(dirty, (int)strlen(dirty));
}

// Idle band noise: short dit/dah-ish fragments, exactly what the QMX decoder
// emits with no signal. Never contains a digit, so is_plausible_call() must
// reject all of it - if a noise burst ever shows up on the station card, the
// capture heuristics have a bug.
static void sim_feed_noise(void)
{
    static const char pool[] = "ETIMSANE";
    char buf[10];
    int n = 1 + (int)(esp_random() % 3), o = 0;
    buf[o++] = ' ';
    for (int i = 0; i < n; i++) buf[o++] = pool[esp_random() % (sizeof(pool) - 1)];
    buf[o++] = ' ';
    buf[o] = '\0';
    cw_screen_feed_decoded(buf, o);
}

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
    int64_t now = esp_timer_get_time();

    char my[CALL_MAX], tgt[CALL_MAX];
    portENTER_CRITICAL(&s_mux);
    memcpy(my, s_my_call, sizeof(my));
    memcpy(tgt, s_target, sizeof(tgt));
    cw_esm_state_t st = s_state;
    portEXIT_CRITICAL(&s_mux);
    if (!my[0]) return;

    // Band noise, always - a real session never has a silent decoder.
    if (!s_sim_noise_due_us) s_sim_noise_due_us = now + 4000000;
    if (now >= s_sim_noise_due_us) {
        s_sim_noise_due_us = now + 3000000 + (int64_t)(esp_random() % 6000000);
        sim_feed_noise();
    }

    // Someone else calling CQ, so the operator can practise ANSWERING rather
    // than only being answered. Only while idle: a station calling CQ in the
    // middle of our exchange would be noise in the worst sense.
    if (st == CW_ESM_CQ && !tgt[0]) {
        if (!s_sim_cq_due_us) s_sim_cq_due_us = now + 15000000;
        if (now >= s_sim_cq_due_us) {
            s_sim_cq_due_us = now + 25000000 + (int64_t)(esp_random() % 20000000);
            const char *c = SIM_CALLS[esp_random() % (sizeof(SIM_CALLS) / sizeof(SIM_CALLS[0]))];
            char msg[96];
            // Every third CQ or so is a park activation, so the POTA hints
            // and the card's park line get exercised too.
            if ((esp_random() % 3) == 0)
                snprintf(msg, sizeof(msg), " CQ POTA DE %s %s K ", c, c);
            else
                snprintf(msg, sizeof(msg), " CQ CQ DE %s %s K ", c, c);
            sim_feed("station calls CQ", msg);
        }
    } else {
        s_sim_cq_due_us = 0;   // re-arm the delay once we are free again
    }

    if (!s_sim_reply_due_us || now < s_sim_reply_due_us) return;
    s_sim_reply_due_us = 0;

    // Whoever we are actually working answers - if the operator pounced on a
    // CQ, the replies must come from THAT station, not a hardcoded W1AW.
    if (tgt[0]) snprintf(s_sim_peer, sizeof(s_sim_peer), "%s", tgt);
    else if (s_sim_reply_kind == 1)
        snprintf(s_sim_peer, sizeof(s_sim_peer), "%s",
                 SIM_CALLS[esp_random() % (sizeof(SIM_CALLS) / sizeof(SIM_CALLS[0]))]);

    char msg[96];
    switch (s_sim_reply_kind) {
        case 1:  // they answer our CQ
            snprintf(msg, sizeof(msg), " %s %s DE %s %s K ", my, my, s_sim_peer, s_sim_peer);
            break;
        case 2:  // their report back (we just sent ours, state moved to TU)
            snprintf(msg, sizeof(msg), " %s DE %s UR 579 579 US-%04u BK ",
                     my, s_sim_peer, (unsigned)(esp_random() % 10000));
            break;
        default: // courtesy final after our TU
            snprintf(msg, sizeof(msg), " TU 73 EE ");
            break;
    }
    sim_feed("phantom replies", msg);
}
