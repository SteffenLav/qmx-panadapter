// FT8 robot: unattended auto-answer of other stations' CQs. See ft8_robot.h
// for the design rationale. This file is purely the "who do we call" decision;
// the QSO itself runs on the existing ft8_qso state machine.

#include "ft8_robot.h"
#include "ft8_greylist.h"
#include "ft8_qso.h"
#include "ft8_tx.h"
#include "ft8_status.h"
#include "ft8_test.h"   // ft8_op_mode_slot_ms() - FT8 vs FT4 slot grid
#include "ui/ft8_screen.h"
#include "storage/settings.h"
#include "adif/adif_log.h"
#include "util/maidenhead.h"
#include "cat/cat.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "ft8_robot";

// True while the QSO machine is busy with a QSO *we* (the robot) started, so we
// know to clear a sticky pounce TIMEOUT and keep going unattended (a human-
// started QSO that times out is left sticky for the operator to see/clear).
static bool s_robot_qso = false;

// Great-circle distance (km) between two grids. Returns -1 if either is
// unusable, so callers can treat "unknown distance" as lowest priority.
static double grid_distance_km(const char *a, const char *b)
{
    double la1, lo1, la2, lo2;
    if (!maidenhead_to_latlon(a, &la1, &lo1)) return -1.0;
    if (!maidenhead_to_latlon(b, &la2, &lo2)) return -1.0;
    return haversine_km(la1, lo1, la2, lo2);
}

// ---------------------------------------------------------------------------
// Target selection
// ---------------------------------------------------------------------------

// Parity-alternation state for the run-limiter in ft8_robot_tick(). See the
// comment there for why hunting drifts onto one slot window and stays.
#define ROBOT_PARITY_RUN_MAX 3
static int s_last_parity = -1;   // parity of our last pounce (-1 = none yet)
static int s_parity_run  = 0;    // consecutive pounces on that same parity

// Is `text` a general CQ (starts with the "CQ" token)? We only auto-answer
// CQs, never tail-end someone else's exchange.
static bool is_cq(const char *text)
{
    return text[0] == 'C' && text[1] == 'Q' &&
           (text[2] == ' ' || text[2] == '\0');
}

// Higher = preferred. Encodes the operator's ranking choice into a single
// comparable score; SNR is in dB (already small ints), distance in km.
static double rank_score(const ft8_call_t *c, ft8_robot_priority_t pri,
                         const char *my_grid)
{
    switch (pri) {
        case FT8_ROBOT_PRI_WEAKEST:
            return (double)(-c->last_snr_db);
        case FT8_ROBOT_PRI_DISTANT: {
            double d = grid_distance_km(my_grid, c->last_grid);
            return (d < 0) ? -1.0 : d;   // unknown grid -> lowest
        }
        case FT8_ROBOT_PRI_STRONGEST:
        default:
            return (double)c->last_snr_db;
    }
}

void ft8_robot_tick(int64_t slot_sec)
{
    qmx_settings_t qs;
    settings_load_all(&qs);
    const ft8_filters_t *f = &qs.ft8_filters;

    if (!f->robot_en) { s_robot_qso = false; return; }

    ft8_qso_state_t st = ft8_qso_get_state();

    // A robot-started pounce that timed out goes sticky TIMEOUT; clear it so we
    // keep going unattended. A human QSO's timeout (s_robot_qso==false) is left
    // for the operator.
    if (st == FT8_QSO_TIMEOUT) {
        if (s_robot_qso) { ft8_qso_abort(); s_robot_qso = false; }
        return;
    }
    // Busy with any QSO (ours or a human's) — don't interfere.
    if (st != FT8_QSO_IDLE) return;

    // Reached IDLE: any prior robot QSO has ended.
    s_robot_qso = false;

    if (!qs.my_callsign[0] || !qs.my_grid[0]) return;  // can't TX without identity

    // Scan the live decode list for CQ callers heard in THIS slot (so the
    // reply parity is the next slot and the data is fresh), apply the filters,
    // and keep the best-ranked survivor.
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    // Deliberately let the OTHER slot window have a turn now and then.
    //
    // Every candidate in a single tick necessarily shares one parity (the loop
    // below only accepts stations heard in THIS slot), so the choice is never
    // within a slot - it is across them. And it is biased: an exchange takes a
    // fixed number of slots, so completing a QSO tends to drop us back to idle on
    // the SAME parity every time, and we can sit on one window for a long stretch.
    // Roy KI0ER (2026-08-05) saw exactly that, and pointed out the cost: the
    // occupancy picture for the window we never transmit in is never refreshed,
    // so the automatic tone picker is working from a stale half of the band.
    //
    // After ROBOT_PARITY_RUN_MAX consecutive pounces on one parity, give up ONE
    // slot so the next tick lands on the other one. Bounded on purpose: the run
    // counter resets on the skip, so we can never skip twice running and go deaf.
    const int64_t period_ms = ft8_op_mode_slot_ms();
    int this_parity = (int)((((int64_t)slot_sec * 1000 + period_ms / 2) / period_ms) % 2);
    if (s_parity_run >= ROBOT_PARITY_RUN_MAX && this_parity == s_last_parity) {
        ESP_LOGI(TAG, "yielding this %s slot after %d pounces on it - sampling the other window",
                 this_parity ? "odd" : "even", s_parity_run);
        s_parity_run = 0;
        return;
    }

    int    best_idx   = -1;
    double best_score = 0;
    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec)        continue;  // not this slot
        if (!is_cq(snap[i].last_text))           continue;  // not a CQ
        if (!snap[i].call[0])                    continue;
        if (strcmp(snap[i].call, qs.my_callsign) == 0) continue;  // our own echo
        if (f->excl_worked_before &&
            adif_log_contains_call_on_band(snap[i].call, cat_get_frequency())) continue;
        // Grey-listed after repeated failed pounces - stop re-calling them
        // every time their CQ reappears (gated by the same greylist_en the
        // timeout tracker uses).
        if (qs.greylist_en && ft8_greylist_contains(snap[i].call)) continue;
        if (!ft8_filter_match(snap[i].last_text, f)) continue;   // include/exclude terms

        double score = rank_score(&snap[i], (ft8_robot_priority_t)f->robot_priority, qs.my_grid);
        if (best_idx < 0 || score > best_score) {
            best_idx   = i;
            best_score = score;
        }
    }
    if (best_idx < 0) return;   // nobody eligible this slot

    const ft8_call_t *t = &snap[best_idx];

    // Remember which window this pounce used, for the run-limiter above.
    if (this_parity == s_last_parity) s_parity_run++;
    else { s_last_parity = this_parity; s_parity_run = 1; }

    // Build TX1 exactly like the manual row_activate() path: our reply goes on a
    // clear tone (not the caller's own), parity derived from their last_utc.
    // Honours TX hold, same as every other TX path.
    int reply_freq_hz = ft8_tx_pick_tone_hz();
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request(FT8_TX_KIND_REPLY, t->call, reply_freq_hz,
                              t->last_utc, NULL, &req, err, sizeof(err))) {
        ESP_LOGW(TAG, "robot build_request(%s) failed: %s", t->call, err);
        return;
    }
    if (ft8_qso_start(&req, err, sizeof(err))) {
        // Robot-started QSOs abandon a busy target instead of holding for it -
        // the robot picked from a list, so there is nothing to be loyal to.
        ft8_qso_mark_robot_started();
    } else {
        ESP_LOGW(TAG, "robot ft8_qso_start(%s) refused: %s", t->call, err);
        return;
    }
    s_robot_qso = true;
    ESP_LOGI(TAG, "robot answering CQ from %s (pri=%d score=%.0f, %d heard this slot)",
             t->call, f->robot_priority, best_score, n);
    ft8_status_set("Robot: answering %s", t->call);
}
