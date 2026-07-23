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
    bool        worked;    // completed a QSO with us this sim session - stops
                           // answering our CQ (still CQs itself, so pounce and
                           // the worked-before filter stay testable). Cleared
                           // when sim mode is toggled off.
    // Pending outgoing message + patience: like a real operator, a phantom
    // repeats its current message on its parity (every 30 s) until it sees
    // our next step (a fresh detection replaces the pending) or its patience
    // runs out (SIM_PHANTOM_REPEATS sends) - then it gives up and goes back
    // to CQing. Injection happens from the main loop (never blocks detection).
    bool        pend_active;
    char        pend_text[40];   // PRE-SYNTHESIZED decoded text (see
                                 // build_message: synth once at scheduling,
                                 // land instantly at the due moment)
    int         pend_snr;
    int         pend_score;
    bool        pend_early;      // fire early in the slot (pileup answers) vs
                                 // at the Fast-pounce-dependent decode instant
    int64_t     pend_next_slot;  // slot of the next (re)send
    int         pend_repeats;    // sends remaining incl. the first
} ft8_sim_phantom_t;

// Total sends of one message before a phantom gives up (first send + retries).
#define SIM_PHANTOM_REPEATS 4

// A varied pool: US calls for the common case plus a few DX entities so the
// distance readout and worked-before/filter paths have something to chew on.
// Distinct tones (well spread across the 200-2900 Hz FT8 audio window) so the
// phantoms don't sit on top of each other in the waterfall/decode list.
#define N_PHANTOMS 6
static ft8_sim_phantom_t s_phantoms[N_PHANTOMS] = {
    { "W1AW",   "FN31", "3A", "EMA", 700.0f,  false },  // ARRL HQ, US
    { "K9ZZ",   "EN52", "5B", "WCF", 2100.0f, false },  // US
    { "N5XYZ",  "EM12", "2A", "STX", 1200.0f, false },  // US
    { "VK3ABC", "QF22", "1D", "DX",  1550.0f, false },  // Australia (DX)
    { "JA1XYZ", "PM95", "1D", "DX",  1850.0f, false },  // Japan (DX)
    { "G0ABC",  "IO91", "1D", "DX",  2500.0f, false },  // England (DX)
};

// How many phantoms pile onto our CQ in the same reply slot. >1 builds a real
// pileup so the pileup tracker/viewer and Skip-TX1 paths can be exercised.
#define SIM_PILEUP_CALLERS 4

#define CQ_REINJECT_PERIOD_SEC 30

// Encode (standard or ARRL FD) + synthesize GFSK audio + run it through the
// REAL decode pipeline, returning the decoded text/snr/score. The expensive
// part (~2-3 s of synth+decode) - kept separate from injection so a message
// can be prepared once and then LANDED at an exact instant (and repeated for
// free). Injecting at synth-completion time made landing times slip ~3 s per
// queued message, which pushed an exchange reply past its slot boundary and
// out of ft8_qso_advance()'s scan (hardware-observed: R-09 landing at
// boundary +1.5 s, missed every cycle, QSO never advanced).
static bool build_message(const char *call_to, const char *call_de, const char *extra,
                          bool use_fd, float tone_hz,
                          char *text_out, size_t text_cap, int *snr_out, int *score_out)
{
    ftx_message_t msg;
    ftx_message_rc_t rc = use_fd
        ? ftx_message_encode_arrl_fd(&msg, NULL, call_to, call_de, extra)
        : ftx_message_encode_std(&msg, NULL, call_to, call_de, extra);
    if (rc != FTX_MESSAGE_RC_OK) {
        ESP_LOGW(TAG, "encode failed for '%s' '%s' '%s' (fd=%d) rc=%d", call_to, call_de, extra, use_fd, (int)rc);
        return false;
    }

    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr_db = -10, score = 30;
    if (!ft8_synth_and_decode(&msg, tone_hz, text, sizeof(text), &snr_db, &score)) {
        ESP_LOGW(TAG, "synth/decode failed for '%s' '%s' '%s'", call_to, call_de, extra);
        return false;
    }
    snprintf(text_out, text_cap, "%s", text);
    if (snr_out)   *snr_out   = snr_db;
    if (score_out) *score_out = score;
    return true;
}

// Prepare + inject immediately (idle CQs - landing time uncritical).
static void build_and_inject(const char *call_to, const char *call_de, const char *extra,
                             bool use_fd, float tone_hz, int64_t slot_sec)
{
    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr_db, score;
    if (!build_message(call_to, call_de, extra, use_fd, tone_hz,
                       text, sizeof(text), &snr_db, &score)) return;
    ft8_screen_record_decode(text, score, snr_db, (int)tone_hz, slot_sec, 0);  // phantom = on-beat (dt 0)
    ESP_LOGI(TAG, "injected '%s' (snr=%d score=%d slot=%lld)", text, snr_db, score, (long long)slot_sec);
}

// Phantom CQ stations, refreshed periodically so they're tappable in the
// decode list. Skipped for any phantom currently "in" a simulated QSO.
//
// Injected ONE per call (round-robin via *idx), not as a whole-pool batch:
// each synth+decode takes ~2-3 s, so a 6-phantom batch blocked the sim task
// ~18 s straight - longer than a 12.6 s TX burst, which meant the task's
// 500 ms TX-rising-edge poll could miss entire bursts (hardware-observed:
// every other burst went undetected, so the phantom never answered it).
// Returns true if it injected (caller then comes back next iteration for the
// rest of the pool; TX detection runs in between).
static bool inject_next_idle_cq(int *idx)
{
    int64_t slot_sec = (time(NULL) / 15) * 15;
    while (*idx < N_PHANTOMS) {
        int i = (*idx)++;
        if (s_phantoms[i].engaged) continue;
        build_and_inject("CQ", s_phantoms[i].call, s_phantoms[i].grid, false,
                         s_phantoms[i].tone_hz, slot_sec);
        return true;
    }
    return false;
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

// How many seconds into the phantom's TX slot its message should LAND in the
// decode list, mimicking the operator's own decode latency per the "Fast
// pounce (early decode)" toggle:
//   ON  -> lands ~13 s in (just before the boundary - the early-decode cut)
//   OFF -> lands ~16 s in (a normal decode pass, AFTER the boundary; the
//          sim advance in ft8_test.c waits ~3.5 s past the boundary to match)
// Landing is instant (pre-synthesized text), so these ARE the landing times.
static int reply_visibility_delay_sec(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    return s.ft8_early_decode ? 13 : 16;
}

// Set (or replace) a phantom's pending message. The expensive synth+decode
// runs HERE, once; the main loop's pending pump then lands the stored text
// instantly at each due moment (repeats reuse it for free). Blocking ~2-3 s
// here is fine - TX detection is slot-keyed, so a burst can't be missed.
static void set_pending(ft8_sim_phantom_t *ph, const char *my_call,
                        const char *extra, bool use_fd,
                        bool early, int64_t first_slot, int repeats)
{
    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr, score;
    if (!build_message(my_call, ph->call, extra, use_fd, ph->tone_hz,
                       text, sizeof(text), &snr, &score)) {
        ph->pend_active = false;
        return;
    }
    snprintf(ph->pend_text, sizeof(ph->pend_text), "%s", text);
    ph->pend_snr       = snr;
    ph->pend_score     = score;
    ph->pend_early     = early;
    ph->pend_next_slot = first_slot;
    ph->pend_repeats   = repeats;
    ph->pend_active    = true;
    ph->engaged        = true;
}

// Phantoms answer our CQ. Engages up to SIM_PILEUP_CALLERS idle phantoms,
// each calling us in the SAME reply slot (a genuine pileup) at their own
// tones - and, like real operators, each keeps calling every 30 s until
// answered or out of patience. The QSO machine works one; the rest populate
// the pileup tracker/viewer. No-op if none are idle.
static void schedule_cq_answer(const char *my_call, int64_t our_slot)
{
    int64_t reply_slot = our_slot + 15;
    int n = 0;
    for (int i = 0; i < N_PHANTOMS && n < SIM_PILEUP_CALLERS; i++) {
        if (s_phantoms[i].engaged || s_phantoms[i].worked) continue;
        // Pileup answers land EARLY in the slot (instant landing - text is
        // pre-synthesized) so all of them are inside the slot's scan.
        set_pending(&s_phantoms[i], my_call, s_phantoms[i].grid, false, true,
                    reply_slot, SIM_PHANTOM_REPEATS);
        n++;
    }
    ESP_LOGI(TAG, "CQ: %d phantom(s) will call in slot %lld", n, (long long)reply_slot);
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
// report, roger, manual single-step Transmit, ...). The reply is derived
// from WHAT WE ACTUALLY SENT (`sent_extra`, the third field of our message)
// - the same way a real partner would react - NOT from ft8_qso's state:
// a manual step-by-step Transmit (Roy KI0ER's intelligent-Transmit flow)
// never starts the QSO machine at all (state stays IDLE), and the old
// state-driven switch silently sent no reply there. Content mapping is
// WSJT-X partner etiquette:
//   grid / anything else -> their report of us ("-09")
//   report (+NN/-NN)     -> their roger + report ("R-09")
//   R<report>            -> "RR73"
//   RR73                 -> "73"   (polite close)
//   73                   -> nothing (QSO over)
// Field Day still keys off `qso_state` for the exchange steps, since the FD
// class+section content isn't derivable from our suffix alone.
static void schedule_phantom_reply(ft8_sim_phantom_t *ph, const char *my_call,
                                   const char *sent_extra,
                                   ft8_qso_state_t qso_state, bool field_day_en,
                                   int64_t our_slot)
{
    char extra[20];
    bool use_fd = false;

    // In-QSO phantoms must stop idle-CQing: the decode list keys one entry per
    // call, so a later 'CQ <ph>' overwrites the reply text/slot in that entry
    // before ft8_qso_advance() reads it - the QSO machine then never sees the
    // reply and re-sends the same message until timeout (hardware-observed:
    // R-09 recorded at +86 s, clobbered by the phantom's own CQ at +94 s,
    // advance() at +97 s found only the CQ). Real stations don't CQ mid-QSO.
    ph->engaged = true;

    int repeats = SIM_PHANTOM_REPEATS;

    if (field_day_en && qso_state == FT8_QSO_WAIT_RPT) {
        // Their first FD message: class+section, no "R" yet.
        snprintf(extra, sizeof(extra), "%s %s", ph->fd_class, ph->fd_section);
        use_fd = true;
    } else if (field_day_en && qso_state == FT8_QSO_WAIT_ROGER) {
        // We sent our exchange; partner rogers it.
        snprintf(extra, sizeof(extra), "R %s %s", ph->fd_class, ph->fd_section);
        use_fd = true;
    } else if (strcmp(sent_extra, "73") == 0) {
        // We signed off - nothing more to say. Also drop any pending repeat
        // (without this they'd keep re-sending their previous message at a
        // finished QSO) and let them go back to CQing.
        ph->pend_active = false;
        ph->engaged     = false;
        return;
    } else if (strcmp(sent_extra, "RR73") == 0) {
        snprintf(extra, sizeof(extra), "73");
        repeats = 1;   // a courtesy close, sent once - not repeated
    } else if (sent_extra[0] == 'R' &&
               (sent_extra[1] == '+' || sent_extra[1] == '-')) {
        snprintf(extra, sizeof(extra), "RR73");
    } else if (sent_extra[0] == '+' || sent_extra[0] == '-') {
        snprintf(extra, sizeof(extra), "R-09");
    } else {
        // Grid TX1 (or anything unrecognized): their report of our signal.
        fmt_report(-9, extra, sizeof(extra));
    }

    set_pending(ph, my_call, extra, use_fd, false, our_slot + 15, repeats);
}

static void ft8_sim_task(void *arg)
{
    (void)arg;
    int64_t last_tx_slot = -1;   // slot of the last burst we handled (see below)
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
            if (was_active) {
                ESP_LOGI(TAG, "sim mode OFF (or FT4 active)");
                was_active = false;
                // Fresh session next time: worked phantoms answer CQs again,
                // and nothing pending survives the toggle.
                for (int i = 0; i < N_PHANTOMS; i++) {
                    s_phantoms[i].worked      = false;
                    s_phantoms[i].engaged     = false;
                    s_phantoms[i].pend_active = false;
                }
            }
            last_tx_slot = -1;
            continue;
        }
        if (!s.my_callsign[0]) {
            if (!warned_no_call) {
                ESP_LOGW(TAG, "sim mode ON but no callsign set (Settings) - idling, nothing will be injected");
                warned_no_call = true;
            }
            last_tx_slot = -1;
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

        // NOTE: engagement is no longer tied to the QSO machine's state (a
        // manual step-by-step Transmit never starts it, so IDLE told us
        // nothing). A phantom now disengages when its own patience runs out
        // (pending pump below) or when we sign off to it (the "73" case in
        // schedule_phantom_reply) - like a real operator walking away.
        ft8_qso_state_t qso_state = ft8_qso_get_state();

        // Burst detection is SLOT-keyed, not edge-triggered. The old
        // ACTIVE-rising-edge test missed bursts whenever this task had been
        // blocked >1 slot in a synth call (idle-CQ batch / pileup answer):
        // it went to sleep with prev=ACTIVE (old burst) and woke with the
        // NEXT burst already ACTIVE - no edge, no phantom reply, and the
        // exchange stalled a full extra cycle (hardware-observed). A burst
        // occupies exactly one slot, so "ACTIVE in a slot we haven't handled"
        // is the reliable trigger; requiring >= 2 periods of separation skips
        // the same burst spilling ~0.4 s past its own boundary (legitimate
        // consecutive bursts are parity-locked >= 2 periods apart anyway).
        char tx_text[40];
        ft8_tx_state_t tx_state = ft8_tx_get_status(tx_text, sizeof(tx_text), NULL);
        int64_t cur_slot = ((int64_t)time(NULL) / 15) * 15;
        if (tx_state == FT8_TX_ACTIVE &&
            (last_tx_slot < 0 || cur_slot - last_tx_slot >= 30)) {
            last_tx_slot = cur_slot;
            int64_t our_slot = cur_slot;
            // display_text uses s.my_callsign verbatim (ft8_tx.c) - which may
            // not be upper-case - while calls in the message are upper-cased by
            // the encoder round-trip. Match on an upper-cased copy so a
            // lower/mixed-case stored callsign can't silently defeat detection
            // (which manifested as "phantoms never reply" - no token match, no
            // reply scheduled, and no log said so).
            char tx_up[40];
            for (size_t k = 0; k < sizeof(tx_up) - 1 && tx_text[k]; k++)
                tx_up[k] = (char)toupper((unsigned char)tx_text[k]);
            tx_up[(strlen(tx_text) < sizeof(tx_up) - 1) ? strlen(tx_text) : sizeof(tx_up) - 1] = '\0';

            char prefix[20], suffix[24];
            if (split_around(tx_up, my_call, prefix, sizeof(prefix), suffix, sizeof(suffix))) {
                bool is_cq = (strncmp(prefix, "CQ", 2) == 0) && (prefix[2] == '\0' || prefix[2] == ' ');
                ESP_LOGI(TAG, "our TX ACTIVE '%s' -> to='%s' extra='%s' is_cq=%d qso_state=%d",
                         tx_up, prefix, suffix, (int)is_cq, (int)qso_state);
                if (is_cq) {
                    if (qso_state == FT8_QSO_CQ) schedule_cq_answer(my_call, our_slot);
                    else ESP_LOGW(TAG, "  CQ text but qso_state!=CQ (%d) - no answer scheduled", (int)qso_state);
                } else {
                    bool matched = false;
                    for (int i = 0; i < N_PHANTOMS; i++) {
                        if (strcmp(prefix, s_phantoms[i].call) == 0) {
                            // Our RR73/73 closes the exchange - remember this
                            // phantom as worked so it stops answering our CQ
                            // (see .worked). It still replies to THIS message
                            // if the state calls for it (e.g. 73 ack).
                            if (strcmp(suffix, "RR73") == 0 || strcmp(suffix, "73") == 0)
                                s_phantoms[i].worked = true;
                            schedule_phantom_reply(&s_phantoms[i], my_call, suffix, qso_state, s.field_day_en, our_slot);
                            matched = true;
                            break;
                        }
                    }
                    if (!matched)
                        ESP_LOGW(TAG, "  addressee '%s' is not a phantom - no reply scheduled", prefix);
                }
            } else {
                ESP_LOGW(TAG, "our TX ACTIVE '%s' but my_call '%s' not found as a token - no reply scheduled",
                         tx_up, my_call);
            }
        }

        // Pending pump: land every due message INSTANTLY (the text was
        // synthesized at scheduling time), so landing times never slip past
        // the slot boundary no matter how many phantoms are due at once. A
        // phantom whose repeats run out gives up: disengages and goes back to
        // CQing on the next idle-CQ pass.
        {
            int64_t now_s = time(NULL);
            int vis = reply_visibility_delay_sec();
            for (int i = 0; i < N_PHANTOMS; i++) {
                ft8_sim_phantom_t *ph = &s_phantoms[i];
                if (!ph->pend_active) continue;
                int fire_off = ph->pend_early ? 3 : vis;
                if (now_s < ph->pend_next_slot + fire_off) continue;
                ft8_screen_record_decode(ph->pend_text, ph->pend_score, ph->pend_snr,
                                         (int)ph->tone_hz, ph->pend_next_slot, 0);
                ESP_LOGI(TAG, "injected '%s' (snr=%d score=%d slot=%lld)",
                         ph->pend_text, ph->pend_snr, ph->pend_score,
                         (long long)ph->pend_next_slot);
                ph->pend_next_slot += 30;   // their parity: every other slot
                if (--ph->pend_repeats <= 0) {
                    ph->pend_active = false;
                    ph->engaged     = false;
                    ESP_LOGI(TAG, "%s gave up (out of patience) - back to CQ", ph->call);
                }
            }
        }

        // Idle-CQ refresh: one phantom per loop iteration (see
        // inject_next_idle_cq), restarted every CQ_REINJECT_PERIOD_SEC.
        static int s_cq_idx = N_PHANTOMS;   // pool position; ==N when batch done
        int64_t now = time(NULL);
        if (now - last_cq_inject_sec >= CQ_REINJECT_PERIOD_SEC) {
            last_cq_inject_sec = now;
            s_cq_idx = 0;
        }
        if (s_cq_idx < N_PHANTOMS) inject_next_idle_cq(&s_cq_idx);
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
