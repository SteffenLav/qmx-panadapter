// FT8 Hound (DXpedition) decision logic - see ft8_hound.h for the protocol and
// for why the Fox side is impossible on this radio.

#include "ft8_hound.h"
#include "ft8_tx.h"
#include "ft8_qso.h"          // ft8_qso_state_t - for ft8_hound_hint()
#include "storage/settings.h"
#include "adif/adif_log.h"    // adif_log_contains_call_on_band() - dupe guard
#include "cat/cat.h"          // cat_get_frequency() - which band we are on

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ft8_hound";

// True while the CURRENT contact was started by automatic mode, so a timeout can
// be cleared without touching a contact the operator started by hand.
static bool s_auto_started;

ft8_hound_mode_t ft8_hound_mode(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    if (s.hound_mode > (uint8_t)FT8_HOUND_AUTO) return FT8_HOUND_AUTO;
    return (ft8_hound_mode_t)s.hound_mode;
}

// Is this message text a plain CQ (with or without a modifier - "CQ", "CQ DX",
// "CQ FD")? A Fox calls CQ from the Fox region, which is half the signature.
static bool text_is_cq(const char *t)
{
    return t && strncmp(t, "CQ ", 3) == 0;
}

// Is this message text one station reporting or rogering ANOTHER - i.e. the
// station is working the band, not calling? The Fox region is where a Fox works
// its queue, so seeing this from a low-frequency station is the other half of the
// signature. Deliberately crude: three tokens where the third is a report,
// R-report or RR73 covers everything a Fox actually sends.
static bool text_is_working_someone(const char *t)
{
    if (!t || !t[0]) return false;
    // Third token, if any.
    const char *sp1 = strchr(t, ' ');
    if (!sp1) return false;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return false;
    const char *third = sp2 + 1;
    if (strncmp(third, "RR73", 4) == 0) return true;
    if (third[0] == 'R' && (third[1] == '+' || third[1] == '-')) return true;
    if (third[0] == '+' || third[0] == '-') return true;
    return false;
}

bool ft8_hound_looks_like_fox(const ft8_call_t *c)
{
    if (!c || !c->occupied || !c->call[0]) return false;
    // In the Fox region, and not so low that our own TX could not follow it down
    // there (the QMX audio path attenuates below FT8_TX_TONE_MIN_HZ, so a Fox
    // below that is one we could hear but never answer on frequency).
    if (c->last_freq <= 0 || c->last_freq >= FT8_HOUND_FOX_MAX_HZ) return false;
    if (c->last_freq < FT8_TX_TONE_MIN_HZ) return false;
    // Working the band from down there, or calling from down there.
    if (!text_is_cq(c->last_text) && !text_is_working_someone(c->last_text))
        return false;
    // Heard more than once. A single decode in the Fox region is far more likely
    // to be an ordinary station (or a decode artefact) than a DXpedition, and the
    // cost of waiting one more slot is nothing.
    return c->heard_count >= 2;
}

const ft8_call_t *ft8_hound_find_fox(const ft8_call_t *list, int n, int64_t slot_sec)
{
    const ft8_call_t *best = NULL;
    for (int i = 0; i < n; i++) {
        if (slot_sec && list[i].last_utc != slot_sec) continue;
        if (!ft8_hound_looks_like_fox(&list[i])) continue;
        if (!best || list[i].last_snr_db > best->last_snr_db) best = &list[i];
    }
    return best;
}

void ft8_hound_tick(int64_t slot_sec)
{
    if (ft8_hound_mode() != FT8_HOUND_AUTO) return;

    ft8_qso_state_t st = ft8_qso_get_state();
    // A hound contact that timed out goes sticky TIMEOUT. Clear it so automatic
    // mode keeps working the pileup - which is the normal outcome of calling a
    // Fox, not a fault. A HUMAN's timeout is left alone for them to see, exactly
    // as ft8_robot_tick() does.
    if (st == FT8_QSO_TIMEOUT) {
        if (s_auto_started) { ft8_qso_abort(); s_auto_started = false; }
        return;
    }
    if (st != FT8_QSO_IDLE) return;
    s_auto_started = false;

    qmx_settings_t qs;
    settings_load_all(&qs);
    if (!qs.my_callsign[0] || !qs.my_grid[0]) return;   // cannot transmit without identity

    // 11 KB on the stack, which is only safe because this runs on the decode
    // task's 64 KB stack - the same reason ft8_robot_tick() can do it. Do NOT
    // call this from taskLVGL (see CLAUDE.md's stack notes and the v0.20.1 crash).
    ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) return;
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    const ft8_call_t *fox = ft8_hound_find_fox(snap, n, slot_sec);
    if (fox) {
        // Already in the log on this band? Then leave it alone. A DXpedition
        // dupe earns neither station anything, and this is what stops automatic
        // mode from calling the same Fox until the operator intervenes.
        if (adif_log_contains_call_on_band(fox->call, (uint32_t)cat_get_frequency())) {
            ESP_LOGD(TAG, "fox %s already worked on this band - not calling", fox->call);
            fox = NULL;
        }
    }

    if (fox) {
        int tone = ft8_hound_pick_tx_tone();
        ft8_tx_request_t req;
        char err[64];
        if (!ft8_tx_build_request(FT8_TX_KIND_REPLY, fox->call, tone,
                                  fox->last_utc, NULL, &req, err, sizeof(err))) {
            ESP_LOGW(TAG, "hound build_request(%s) failed: %s", fox->call, err);
        } else if (ft8_qso_start(&req, err, sizeof(err))) {
            s_auto_started = true;
            ESP_LOGI(TAG, "HOUND auto: calling Fox %s (%d Hz down there) from %d Hz",
                     fox->call, (int)fox->last_freq, tone);
        } else {
            ESP_LOGW(TAG, "hound ft8_qso_start(%s) refused: %s", fox->call, err);
        }
    }

    heap_caps_free(snap);
}

int ft8_hound_pick_tx_tone(void)
{
    // Same occupancy mask the ordinary picker uses, so we avoid the tones we can
    // actually see in use - including the other hounds, who are all up here too.
    int n_slots = 0, n_stations = 0;
    const int base = FT8_TX_TONE_MIN_HZ, step = FT8_TX_TONE_STEP_HZ;
    uint64_t occ = ft8_tx_get_tone_occupancy(&n_slots, &n_stations);

    // n_stations == 0 means nothing has been heard yet, so an all-clear mask is
    // "unknown" rather than "empty band" - ft8_tx.h is explicit about that, and
    // trusting it would put us on 1100 Hz every time on a quiet band.
    if (n_stations > 0 && n_slots > 0) {
        // Walk UP from the hound floor and take the first slot that is free with
        // a clear neighbour on each side (an FT8 signal is ~44 Hz wide, so a bare
        // free slot with busy neighbours is not really free).
        for (int i = 0; i < n_slots; i++) {
            int hz = base + i * step;
            if (hz < FT8_HOUND_TX_MIN_HZ) continue;
            if (hz > FT8_TX_TONE_MAX_HZ) break;
            uint64_t self = 1ULL << i;
            uint64_t lo   = (i > 0) ? (1ULL << (i - 1)) : 0;
            uint64_t hi   = (i + 1 < n_slots) ? (1ULL << (i + 1)) : 0;
            if (occ & (self | lo | hi)) continue;
            return hz;
        }
    }
    // Nothing known yet (or everything busy): a fixed spot well inside the hound
    // region. Not random - a repeatable tone is easier to explain to an operator
    // watching the waterfall than one that moves every time.
    return 1500;
}

const char *ft8_hound_hint(int qso_state, const char *fox_call)
{
    (void)fox_call;
    // Takes an int in the header so ft8_hound.h needn't pull in ft8_qso.h, but
    // switches on the REAL enum here - magic numbers in a state machine are how
    // a renumbered enum silently starts telling the operator the wrong thing.
    switch ((ft8_qso_state_t)qso_state) {
        case FT8_QSO_WAIT_RPT:
            return "Hound: calling the Fox - it works a queue, so keep waiting";
        case FT8_QSO_WAIT_RR73:
            return "Hound: QSY'd onto the Fox - sending R-report";
        case FT8_QSO_WAIT_DONE:
            return "Hound: Fox answered - finishing";
        case FT8_QSO_DONE:
            return "Hound: in the log. Back on the hound tone";
        case FT8_QSO_TIMEOUT:
            return "Hound: no answer yet - the Fox is working others";
        case FT8_QSO_IDLE:
        case FT8_QSO_CQ:            // not a hound flow
        case FT8_QSO_WAIT_ROGER:    // CQ-run only
        default:
            return NULL;
    }
}
