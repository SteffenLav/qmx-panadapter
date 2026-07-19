// FT8 simulation mode - see ft8_sim.h for the full design rationale.
//
// A fixed pool of phantom stations periodically "call CQ" by synthesizing
// real GFSK audio for a real FT8 message and running it through the real
// on-device decode pipeline (ft8_synth_and_decode(), shared with
// ft8_arrl_fd_e2e_selftest()), then injecting the result into the normal
// decode list exactly as a genuine RX would. When the operator transmits
// (pounce, CQ-run answer, or any reply), this task detects it via
// ft8_tx_get_status() and schedules the phantom's next message on the
// correct opposite-parity slot, driven by ft8_qso_get_state() - the SAME
// state the real machine just moved itself into, so the reply content
// (grid/report/RR73/Field-Day class+section) always matches what the real
// exchange is actually waiting for. This module never reaches into
// ft8_qso.c's internals - it only ever feeds the same decode-list input a
// real signal would, so the QSO machine can't tell the difference.
//
// Safety: this module does NOT key the radio - that hard interlock lives in
// ft8_tx.c (ft8_tx_run()/ft8_tx_arm() check sim_mode_en directly and skip
// every cat_* call). This file only ever calls ft8_screen_record_decode().

#include "ft8_sim.h"
#include "ft8_test.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ui/ft8_screen.h"
#include "storage/settings.h"
#include "ft8/message.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ft8_sim";

typedef struct {
    const char *call;
    const char *grid;
    const char *fd_class;
    const char *fd_section;
    float       tone_hz;
    bool        engaged;   // true while "in" a simulated QSO with us
} ft8_sim_phantom_t;

#define N_PHANTOMS 2
static ft8_sim_phantom_t s_phantoms[N_PHANTOMS] = {
    { "W1AW", "FN31", "3A", "EMA", 800.0f,  false },
    { "K9ZZ", "EN52", "5B", "WCF", 2100.0f, false },
};

#define CQ_REINJECT_PERIOD_SEC 30

// Encode (standard or ARRL FD), synthesize, decode, and inject into the
// decode list - the one path every phantom message goes through, so what
// shows up in the list always actually round-tripped the real pipeline.
static void build_and_inject(const char *call_to, const char *call_de, const char *extra,
                             bool use_fd, float tone_hz, int64_t slot_sec)
{
    ftx_message_t msg;
    ftx_message_rc_t rc = use_fd
        ? ftx_message_encode_arrl_fd(&msg, NULL, call_to, call_de, extra)
        : ftx_message_encode_std(&msg, NULL, call_to, call_de, extra);
    if (rc != FTX_MESSAGE_RC_OK) {
        ESP_LOGW(TAG, "encode failed for '%s' '%s' '%s' (fd=%d) rc=%d", call_to, call_de, extra, use_fd, (int)rc);
        return;
    }

    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr_db = -10, score = 30;
    if (!ft8_synth_and_decode(&msg, tone_hz, text, sizeof(text), &snr_db, &score)) {
        ESP_LOGW(TAG, "synth/decode failed for '%s' '%s' '%s'", call_to, call_de, extra);
        return;
    }

    ft8_screen_record_decode(text, score, snr_db, (int)tone_hz, slot_sec, 0);  // phantom = on-beat (dt 0)
    ESP_LOGI(TAG, "injected '%s' (snr=%d score=%d slot=%lld)", text, snr_db, score, (long long)slot_sec);
}

// Phantom CQ stations, refreshed periodically so they're tappable in the
// decode list. Skipped for any phantom currently "in" a simulated QSO.
static void inject_idle_cqs(void)
{
    int64_t slot_sec = (time(NULL) / 15) * 15;
    for (int i = 0; i < N_PHANTOMS; i++) {
        if (s_phantoms[i].engaged) continue;
        build_and_inject("CQ", s_phantoms[i].call, s_phantoms[i].grid, false,
                         s_phantoms[i].tone_hz, slot_sec);
    }
}

// Find `needle` as a whole space-delimited run of tokens inside `full` (not
// just a substring match), splitting the rest into prefix/suffix. Used to
// pull our own callsign out of the TX display text ("K1ABC OZ1LAV R 16A
// EMA" -> prefix="K1ABC", suffix="R 16A EMA") regardless of how many words
// the modifier/extra field carries.
static bool split_around(const char *full, const char *needle,
                         char *prefix, size_t prefix_cap, char *suffix, size_t suffix_cap)
{
    size_t nlen = strlen(needle);
    const char *p = full;
    while ((p = strstr(p, needle)) != NULL) {
        bool start_ok = (p == full) || (p[-1] == ' ');
        bool end_ok   = (p[nlen] == '\0') || (p[nlen] == ' ');
        if (start_ok && end_ok) break;
        p++;
    }
    if (!p) return false;

    size_t plen = (size_t)(p - full);
    if (plen >= prefix_cap) plen = prefix_cap - 1;
    memcpy(prefix, full, plen);
    prefix[plen] = '\0';
    while (plen > 0 && prefix[plen - 1] == ' ') prefix[--plen] = '\0';

    const char *s = p + nlen;
    while (*s == ' ') s++;
    snprintf(suffix, suffix_cap, "%s", s);
    return true;
}

// Block until 2s into `reply_slot`, mimicking real decode-arrival latency,
// then return. Called from this task only - blocking here is fine, nothing
// else needs servicing while our own TX burst is on-air anyway.
static void sleep_until_slot_plus(int64_t reply_slot, int extra_sec)
{
    int64_t fire_at = reply_slot + extra_sec;
    int64_t wait_s = fire_at - (int64_t)time(NULL);
    if (wait_s > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_s * 1000)));
}

// A phantom decides to answer our CQ. Picks the first idle (non-engaged)
// phantom; no-op if all are already "in" a simulated QSO.
static void schedule_cq_answer(const char *my_call, int64_t our_slot)
{
    ft8_sim_phantom_t *ph = NULL;
    for (int i = 0; i < N_PHANTOMS; i++) {
        if (!s_phantoms[i].engaged) { ph = &s_phantoms[i]; break; }
    }
    if (!ph) return;
    ph->engaged = true;

    int64_t reply_slot = our_slot + 15;
    sleep_until_slot_plus(reply_slot, 2);
    build_and_inject(my_call, ph->call, ph->grid, false, ph->tone_hz, reply_slot);
}

// Coarse SNR -> FT8 report token ("-07", "+02"), same convention as
// ft8_qso.c's fmt_report() (duplicated here - that one is file-static).
static void fmt_report(int snr_db, char *out, size_t len)
{
    if (snr_db < -24) snr_db = -24;
    if (snr_db > 15)  snr_db = 15;
    snprintf(out, len, "%+03d", snr_db);
}

// Our TX just addressed a known phantom directly (pounce reply, cqrun
// report, roger, ...). `qso_state` is read AFTER ft8_qso.c armed this TX, so
// it already reflects what we're now waiting for - that's what decides the
// phantom's reply content.
static void schedule_phantom_reply(ft8_sim_phantom_t *ph, const char *my_call,
                                   ft8_qso_state_t qso_state, bool field_day_en,
                                   int64_t our_slot)
{
    char extra[20];
    bool use_fd = false;

    switch (qso_state) {
    case FT8_QSO_WAIT_RPT:
        // Partner's first reply to our TX1 (pounce): a signal report of OUR
        // signal (their grid was already given in their original CQ - it's
        // not repeated here), or our Field Day exchange (no "R" yet - this
        // is their first FD message). Previously sent ph->grid again, which
        // ft8_qso.c's WAIT_RPT handler doesn't expect - it built a bogus
        // "R<grid>" roger instead of a real report, and the malformed
        // exchange manifested as a stuck loop (Ken KF0AYY field report).
        if (field_day_en) { snprintf(extra, sizeof(extra), "%s %s", ph->fd_class, ph->fd_section); use_fd = true; }
        else                fmt_report(-9, extra, sizeof(extra));
        break;
    case FT8_QSO_WAIT_RR73:
        // We sent R<report> / R<class+section>; partner closes with RR73.
        snprintf(extra, sizeof(extra), "RR73");
        break;
    case FT8_QSO_WAIT_ROGER:
        // CQ-run: we sent our report/exchange; partner rogers it.
        if (field_day_en) { snprintf(extra, sizeof(extra), "R %s %s", ph->fd_class, ph->fd_section); use_fd = true; }
        else                snprintf(extra, sizeof(extra), "R-09");
        break;
    default:
        return;  // WAIT_DONE etc. - nothing to reply to, the QSO is wrapping up
    }

    int64_t reply_slot = our_slot + 15;
    sleep_until_slot_plus(reply_slot, 2);
    build_and_inject(my_call, ph->call, extra, use_fd, ph->tone_hz, reply_slot);
}

static void ft8_sim_task(void *arg)
{
    (void)arg;
    ft8_tx_state_t prev_tx_state = FT8_TX_IDLE;
    int64_t last_cq_inject_sec = 0;
    bool was_active = false;
    bool warned_no_call = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        qmx_settings_t s;
        settings_load_all(&s);
        // FT8-only: this phantom-station simulator is hardcoded to FT8
        // protocol (ft8_synth_and_decode() in ft8_test.c) and has no concept
        // of the FT8/FT4 sub-mode, so injecting its fake traffic while the
        // real receiver is running FT4 timing would be nonsensical (fake
        // FT8-protocol QSOs appearing in a decode list whose real RX uses a
        // different slot length entirely). The drawer checkbox is dimmed and
        // locked while in FT4 (ui.c's apply_sim_mode_lock) as the primary
        // guard; this is the backend half of that same belt-and-suspenders
        // pattern, in case sim_mode_en is left on from a prior FT8 session.
        if (!s.sim_mode_en || ft8_op_mode_get() != FT8_OP_MODE_FT8) {
            if (was_active) { ESP_LOGI(TAG, "sim mode OFF (or FT4 active)"); was_active = false; }
            prev_tx_state = FT8_TX_IDLE;
            continue;
        }
        if (!s.my_callsign[0]) {
            if (!warned_no_call) {
                ESP_LOGW(TAG, "sim mode ON but no callsign set (Settings) - idling, nothing will be injected");
                warned_no_call = true;
            }
            prev_tx_state = FT8_TX_IDLE;
            continue;
        }
        warned_no_call = false;
        if (!was_active) { ESP_LOGI(TAG, "sim mode ON (my_call=%s)", s.my_callsign); was_active = true; }

        char my_call[FT8_CALL_MAX_LEN];
        size_t n = 0;
        for (; s.my_callsign[n] && n < sizeof(my_call) - 1; n++) {
            my_call[n] = (char)toupper((unsigned char)s.my_callsign[n]);
        }
        my_call[n] = '\0';

        // Whatever exchange was in progress is now over - free up the
        // phantom(s) involved so they can CQ (or be CQ'd) again.
        ft8_qso_state_t qso_state = ft8_qso_get_state();
        if (qso_state == FT8_QSO_IDLE || qso_state == FT8_QSO_DONE || qso_state == FT8_QSO_TIMEOUT) {
            for (int i = 0; i < N_PHANTOMS; i++) s_phantoms[i].engaged = false;
        }

        char tx_text[40];
        ft8_tx_state_t tx_state = ft8_tx_get_status(tx_text, sizeof(tx_text), NULL);
        if (tx_state == FT8_TX_ACTIVE && prev_tx_state != FT8_TX_ACTIVE) {
            int64_t our_slot = ((int64_t)time(NULL) / 15) * 15;
            char prefix[20], suffix[24];
            if (split_around(tx_text, my_call, prefix, sizeof(prefix), suffix, sizeof(suffix))) {
                bool is_cq = (strncmp(prefix, "CQ", 2) == 0) && (prefix[2] == '\0' || prefix[2] == ' ');
                if (is_cq) {
                    if (qso_state == FT8_QSO_CQ) schedule_cq_answer(my_call, our_slot);
                } else {
                    for (int i = 0; i < N_PHANTOMS; i++) {
                        if (strcmp(prefix, s_phantoms[i].call) == 0) {
                            schedule_phantom_reply(&s_phantoms[i], my_call, qso_state, s.field_day_en, our_slot);
                            break;
                        }
                    }
                }
            }
        }
        prev_tx_state = tx_state;

        int64_t now = time(NULL);
        if (now - last_cq_inject_sec >= CQ_REINJECT_PERIOD_SEC) {
            last_cq_inject_sec = now;
            inject_idle_cqs();
        }
    }
}

void ft8_sim_init(void)
{
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_sim_task, "ft8_sim", 65536, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_sim_task (rc=%d)", (int)rc);
    }
}
