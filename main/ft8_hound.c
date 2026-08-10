// FT8 Hound (DXpedition) decision logic - see ft8_hound.h for the protocol and
// for why the Fox side is impossible on this radio.

#include "ft8_hound.h"
#include "ft8_tx.h"
#include "ft8_qso.h"          // ft8_qso_state_t - for ft8_hound_hint()
#include "storage/settings.h"
#include "adif/adif_log.h"    // adif_log_contains_call_on_band() - dupe guard
#include "cat/cat.h"          // cat_get_frequency() - which band we are on
#include "ft8_status.h"       // ft8_status_set() - the FT8 status line

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

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

// ---- "is it working a QUEUE?" -----------------------------------------------
//
// Sitting below 1000 Hz and calling CQ is NOT enough, and assuming it was is the
// first thing this code got wrong: on the bench it identified W1AW - an ordinary
// phantom whose only distinguishing feature is a 700 Hz tone - as a Fox and
// called it. Plenty of ordinary stations work down there.
//
// What actually distinguishes a Fox is that it works MANY stations in quick
// succession. One at a time is an ordinary QSO; several different callsigns
// inside a couple of minutes is a DXpedition running a queue, and nothing else
// on the band looks like that.
//
// The decode table keeps only each station's LAST message, so the history has to
// live here: a handful of candidates, each with the distinct callsigns we have
// seen them address. Small and fixed - this is a hint, not a database.
#define FOX_CAND_MAX        6
#define FOX_ADDR_MAX        3    // distinct addressees remembered per candidate
#define FOX_QUEUE_MIN_ADDR  2    // ...and how many make it a queue
#define FOX_WINDOW_SEC      240  // forget everything older than this

typedef struct {
    char    call[FT8_CALL_MAX_LEN];
    char    addr[FOX_ADDR_MAX][FT8_CALL_MAX_LEN];
    int     n_addr;
    int64_t last_ts;
} fox_cand_t;

static fox_cand_t s_cands[FOX_CAND_MAX];

// Note that `de` was heard working `to`. Called for every low-frequency station
// on every tick; cheap, and only ever grows a tiny table.
static void fox_note_working(const char *de, const char *to, int64_t now)
{
    if (!de || !de[0] || !to || !to[0]) return;

    int slot = -1, oldest = 0;
    for (int i = 0; i < FOX_CAND_MAX; i++) {
        if (strcmp(s_cands[i].call, de) == 0) { slot = i; break; }
        if (s_cands[i].last_ts < s_cands[oldest].last_ts) oldest = i;
    }
    if (slot < 0) {                       // new candidate: take a free or stalest row
        slot = oldest;
        memset(&s_cands[slot], 0, sizeof(s_cands[slot]));
        snprintf(s_cands[slot].call, sizeof(s_cands[slot].call), "%s", de);
    }
    fox_cand_t *k = &s_cands[slot];
    // Expired since we last saw it? Start its history over, so a station that
    // worked somebody an hour ago does not look like a queue now.
    if (k->last_ts && (now - k->last_ts) > FOX_WINDOW_SEC) {
        k->n_addr = 0;
        memset(k->addr, 0, sizeof(k->addr));
    }
    k->last_ts = now;

    for (int a = 0; a < k->n_addr; a++)
        if (strcmp(k->addr[a], to) == 0) return;        // already counted
    if (k->n_addr < FOX_ADDR_MAX) {
        snprintf(k->addr[k->n_addr], sizeof(k->addr[0]), "%s", to);
        k->n_addr++;
        if (k->n_addr == FOX_QUEUE_MIN_ADDR)
            ESP_LOGI(TAG, "%s is working a queue (%s, %s...) - looks like a Fox",
                     de, k->addr[0], k->addr[1]);
    }
}

static bool fox_has_queue(const char *call, int64_t now)
{
    for (int i = 0; i < FOX_CAND_MAX; i++) {
        if (strcmp(s_cands[i].call, call) != 0) continue;
        if ((now - s_cands[i].last_ts) > FOX_WINDOW_SEC) return false;
        return s_cands[i].n_addr >= FOX_QUEUE_MIN_ADDR;
    }
    return false;
}

// Split "<to> <de> <rest>" far enough to read the first two tokens.
static bool msg_to_de(const char *t, char *to, size_t to_sz, char *de, size_t de_sz)
{
    if (!t || !t[0]) return false;
    const char *sp1 = strchr(t, ' ');
    if (!sp1) return false;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return false;
    size_t l1 = (size_t)(sp1 - t), l2 = (size_t)(sp2 - sp1 - 1);
    if (l1 == 0 || l1 >= to_sz || l2 == 0 || l2 >= de_sz) return false;
    memcpy(to, t, l1); to[l1] = '\0';
    memcpy(de, sp1 + 1, l2); de[l2] = '\0';
    return true;
}

void ft8_hound_observe(const ft8_call_t *list, int n, int64_t now)
{
    for (int i = 0; i < n; i++) {
        const ft8_call_t *c = &list[i];
        if (!c->occupied || !c->call[0]) continue;
        if (c->last_freq <= 0 || c->last_freq >= FT8_HOUND_FOX_MAX_HZ) continue;
        if (!text_is_working_someone(c->last_text)) continue;
        char to[FT8_CALL_MAX_LEN], de[FT8_CALL_MAX_LEN];
        if (!msg_to_de(c->last_text, to, sizeof(to), de, sizeof(de))) continue;
        if (strcmp(de, c->call) != 0) continue;      // the sender must be this row
        if (strncmp(to, "CQ", 2) == 0) continue;
        fox_note_working(de, to, now);
    }
}

bool ft8_hound_looks_like_fox(const ft8_call_t *c)
{
    if (!c || !c->occupied || !c->call[0]) return false;
    // In the Fox region, and not so low that our own TX could not follow it down
    // there (the QMX audio path attenuates below FT8_TX_TONE_MIN_HZ, so a Fox
    // below that is one we could hear but never answer on frequency).
    if (c->last_freq <= 0 || c->last_freq >= FT8_HOUND_FOX_MAX_HZ) return false;
    if (c->last_freq < FT8_TX_TONE_MIN_HZ) return false;
    // Its current message must be Fox-shaped: a CQ from down there, or a report
    // to somebody. (A Fox alternates between the two.)
    if (!text_is_cq(c->last_text) && !text_is_working_someone(c->last_text))
        return false;
    // Heard more than once - one decode in the Fox region is far more likely to be
    // an ordinary station, or an artefact, than a DXpedition.
    if (c->heard_count < 2) return false;
    // And the test that actually means something: is it working a QUEUE? See
    // fox_note_working() for why nothing weaker will do.
    return fox_has_queue(c->call, (int64_t)time(NULL));
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
    const ft8_hound_mode_t mode = ft8_hound_mode();
    if (!ft8_hound_enabled(mode)) return;
    const bool automatic = (mode == FT8_HOUND_AUTO);

    ft8_qso_state_t st = ft8_qso_get_state();
    // A hound contact that timed out goes sticky TIMEOUT. Clear it so automatic
    // mode keeps working the pileup - which is the normal outcome of calling a
    // Fox, not a fault. A HUMAN's timeout is left alone for them to see, exactly
    // as ft8_robot_tick() does.
    if (st == FT8_QSO_TIMEOUT) {
        if (automatic && s_auto_started) { ft8_qso_abort(); s_auto_started = false; }
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

    // Update the queue history FIRST - looks_like_fox() answers from it.
    ft8_hound_observe(snap, n, (int64_t)time(NULL));

    // NOT restricted to this exact slot, unlike the robot's CQ scan. A Fox
    // transmits in ONE window, so demanding last_utc == slot_sec means every look
    // that lands on the other window misses it - at best half the chances, and on
    // the bench (where the phantom Fox calls every ~30 s) almost all of them: the
    // first version of this never once fired, with nothing in the log to say why.
    //
    // Parity stays correct because ft8_tx_build_request() derives it from the
    // Fox's OWN last_utc, so a one-slot-stale sighting still places our call in
    // the window opposite the Fox's. Bounded at two slots: older than that and we
    // would be calling something that may have stopped transmitting.
    const ft8_call_t *fox = ft8_hound_find_fox(snap, n, 0);
    if (fox && slot_sec > 0) {
        int64_t age = slot_sec - fox->last_utc;
        // The window is asymmetric, and the negative side is not a curiosity: a
        // decode can legitimately be NEWER than the slot being scanned, because it
        // belongs to the slot now in progress (we scan a slot after it ends, and a
        // message landing early in the next one is already in the table). Rejecting
        // that as "impossible" is what swallowed every sighting on the bench -
        // detection fired correctly and then the Fox was silently discarded here.
        //
        // One slot ahead, two slots behind. Parity is safe either way: it comes
        // from the Fox's OWN last_utc, so our call always lands in the window
        // opposite the one it transmitted in.
        if (age > 2 * 15 || age < -15) fox = NULL;
    }
    if (fox) {
        // Already in the log on this band? Then leave it alone. A DXpedition
        // dupe earns neither station anything, and this is what stops automatic
        // mode from calling the same Fox until the operator intervenes.
        if (adif_log_contains_call_on_band(fox->call, (uint32_t)cat_get_frequency())) {
            // INFO, change-detected - not debug. A silent suppression here cost a
            // whole bench cycle on 2026-08-10: detection was working perfectly and
            // the Fox was being dropped on this line, with nothing in the log to
            // say so. Note that with no CAT frequency (radio off, e.g. simulation)
            // adif_log_contains_call_on_band() falls back to a call-only match, so
            // in sim ANY earlier contact with that call suppresses it.
            static char last_dupe[FT8_CALL_MAX_LEN];
            if (strcmp(last_dupe, fox->call) != 0) {
                snprintf(last_dupe, sizeof(last_dupe), "%s", fox->call);
                ESP_LOGI(TAG, "Fox %s is already in the log for this band - not calling",
                         fox->call);
            }
            fox = NULL;
        }
    }

    // GUIDED: say it is there, and what a tap will do. This is the whole of
    // guided mode - the operator keeps every transmission decision, and the
    // device's job is to make sure they know the opportunity exists, since a Fox
    // is easy to miss down at the bottom of the passband among the ordinary
    // traffic. Posting through ft8_status keeps ONE writer for the status line
    // (the QSO machine owns it once a contact starts).
    if (fox && !automatic) {
        ft8_status_set("Fox %s at %d Hz - tap it to call as Hound",
                       fox->call, (int)fox->last_freq);
    }

    // One line per sighting, change-detected on the callsign. Kept permanently
    // (not a debug aid): "why did Hound not call that Fox" is the first question
    // any field report will ask, and the answer is almost always either that we
    // never saw it as a Fox or that it was already in the log.
    if (fox) {
        static char last_seen[FT8_CALL_MAX_LEN];
        if (strcmp(last_seen, fox->call) != 0) {
            snprintf(last_seen, sizeof(last_seen), "%s", fox->call);
            ESP_LOGI(TAG, "Fox seen: %s at %d Hz, snr %d, heard %u - mode %s",
                     fox->call, (int)fox->last_freq, (int)fox->last_snr_db,
                     (unsigned)fox->heard_count, automatic ? "automatic" : "guided");
        }
    }

    if (fox && automatic) {
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

