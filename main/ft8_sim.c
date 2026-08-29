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
#include "ft8_pileup.h"
#include "storage/settings.h"
#include "ft8/message.h"

#include <string.h>
#include <sys/time.h>   // sim_now_ms - the slot grid is in ms
#include "esp_random.h"   // per-phantom fading
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui/ui_mode.h"

static const char *TAG = "ft8_sim";

static void fmt_report(int snr_db, char *out, size_t len);   // defined below
static int64_t sim_cur_slot(void);                           // defined below
static int64_t sim_next_slot(int64_t slot_sec);              // (slot arithmetic)

typedef struct {
    const char *call;
    const char *grid;
    const char *fd_class;
    const char *fd_section;
    float       tone_hz;
    bool        engaged;   // true while "in" a simulated QSO with us
    bool        deaf;      // never hears us: CQs but ignores every call - the
                           // station grey-listing exists for (test fixture)
    bool        terse;     // answers our CQ with a REPORT instead of a grid - the
                           // experienced operator who already knows us. Gyula
                           // HA3HZ hit this on the air and our CQ-run replied
                           // with a bare report instead of R<report>, wasting a
                           // cycle. A fixture so it stays reproducible on the
                           // bench, where it can be checked without an antenna.
    bool        worked;    // completed a QSO with us this sim session - stops
                           // answering our CQ (still CQs itself, so pounce and
                           // the worked-before filter stay testable). Cleared
                           // when sim mode is toggled off.
    // Pending outgoing message + patience: like a real operator, a phantom
    // repeats its current message on its parity (every 30 s) until it sees
    // our next step (a fresh detection replaces the pending) or its patience
    // runs out (SIM_PHANTOM_REPEATS sends) - then it gives up and goes back
    // to CQing. Injection happens from the main loop (never blocks detection).
    // FOX (Fox/Hound DXpedition practice target). A Fox differs from every other
    // phantom in three ways that all matter to the Hound code under test:
    //   * it transmits BELOW 1000 Hz (its tone_hz), which is what marks it as a
    //     Fox to ft8_hound_looks_like_fox() and what our R-report must QSY onto;
    //   * it works a QUEUE, so it is visibly mid-exchange with third parties -
    //     the condition that makes ft8_qso.c's busy-station hold fire, which
    //     Hound mode has to suspend or we would never call at all;
    //   * it IGNORES the first few calls, because that is what a pileup is.
    // fox_ignore_left counts those down; while it is non-zero the Fox answers
    // somebody else instead of us.
    bool        is_fox;
    int         fox_ignore_left;
    bool        pend_active;
    char        pend_text[40];   // PRE-SYNTHESIZED decoded text (see
                                 // build_message: synth once at scheduling,
                                 // land instantly at the due moment)
    int         pend_snr;
    // Where this station sits on the band, in dB, and it MOVES (#265). A real
    // station fades; the simulator's did not, which is why every row read
    // +9/+10 and why nothing that depends on a report CHANGING could be tested.
    // Seeded per phantom so some are comfortable and some are marginal.
    int         level_db;
    int         pend_score;
    bool        pend_early;      // fire early in the slot (pileup answers) vs
                                 // at the Fast-pounce-dependent decode instant
    int64_t     pend_next_slot;  // slot of the next (re)send
    int         pend_repeats;    // sends remaining incl. the first
} ft8_sim_phantom_t;

static void level_drift(ft8_sim_phantom_t *ph);   // defined with the slot helpers

// Total sends of one message before a phantom gives up (first send + retries).
#define SIM_PHANTOM_REPEATS 4

// A varied pool: US calls for the common case plus a few DX entities so the
// distance readout and worked-before/filter paths have something to chew on.
// Distinct tones (well spread across the 200-2900 Hz FT8 audio window) so the
// phantoms don't sit on top of each other in the waterfall/decode list.
// How many of our calls the Fox ignores (working others instead) before it comes
// back to us. Two is enough to prove the point and short enough that a bench test
// completes in a couple of minutes; a real pileup can be hundreds.
#define SIM_FOX_IGNORE_SLOTS 2

// The Fox's queue: calls it works while ignoring us. Deliberately not phantoms
// from the pool - they have their own entries in the decode list, and a Fox
// answering one of them would make that station look like it was transmitting
// when it was not.
static const char *const s_fox_queue[] = { "JA3ABC", "EA5XYZ", "VK2DEF", "PY2GHI" };

#define N_PHANTOMS 7
static ft8_sim_phantom_t s_phantoms[N_PHANTOMS] = {
    { "W1AW",   "FN31", "3A", "EMA", 700.0f,  false },        // ARRL HQ, US
    { "K9ZZ",   "EN52", "5B", "WCF", 2100.0f, false },        // US
    { "N5XYZ",  "EM12", "2A", "STX", 1200.0f, .terse = true }, // US - TERSE: answers
                           // our CQ with a report, not a grid (see `terse`).
    { "VK3ABC", "QF22", "1D", "DX",  1550.0f, .terse = true }, // Australia (DX)
                           /* ⭐ ALSO TERSE, and it must be a STRONG one (#292).
                            * N5XYZ alone could not reach the R-report entry in
                            * ft8_qso_start(): the pileup drain picks by STRONGEST
                            * SNR, so the weakest phantom is drained last, by which
                            * time it has gone back to calling CQ and the drain
                            * takes its "not to us - report-first" fallback instead.
                            * The terse path was therefore only ever exercised
                            * through cqrun_answer() - and the pileup drain is the
                            * path that actually carried the bug.
                            *
                            * VK3ABC is the second strongest, so the FIRST caller
                            * (W1AW) is taken by cqrun_answer and the drain then
                            * picks VK3ABC while its terse answer is still its
                            * last_text. That makes the R-report entry reachable on
                            * the bench with no radio. */
    { "JA1XYZ", "PM95", "1D", "DX",  1850.0f, false },        // Japan (DX)
    { "G0ABC",  "IO91", "1D", "DX",  2500.0f, .deaf = true }, // England (DX) - DEAF:
                           // CQs but never hears you; pounces at it time out.
                           // Practice target for the grey-list feature.
    // The FOX, at 500 Hz - inside the Fox region (below FT8_HOUND_FOX_MAX_HZ) and
    // above FT8_TX_TONE_MIN_HZ, so we can both hear it and follow it down there.
    // "K0FOX" is mnemonic on purpose: sim contacts land in the REAL ADIF log
    // (deliberately - it exercises the logging path), so when this turns up in the
    // log months later it should be obvious what it was.
    { "K0FOX",  "EM28", "1D", "DX",  500.0f,
      .is_fox = true, .fox_ignore_left = SIM_FOX_IGNORE_SLOTS },
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
                          bool use_fd, float tone_hz, int level_db,
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
    // At the phantom's own level, in noise - so the SNR that comes back is one
    // the decoder MEASURED rather than one the simulator announced (#265).
    if (!ft8_synth_and_decode_at(&msg, tone_hz, level_db,
                                 text, sizeof(text), &snr_db, &score)) {
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
                             bool use_fd, float tone_hz, int level_db, int64_t slot_sec)
{
    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr_db, score;
    if (!build_message(call_to, call_de, extra, use_fd, tone_hz, level_db,
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
    int64_t slot_sec = sim_cur_slot();
    while (*idx < N_PHANTOMS) {
        int i = (*idx)++;
        if (s_phantoms[i].engaged) continue;
        level_drift(&s_phantoms[i]);
        build_and_inject("CQ", s_phantoms[i].call, s_phantoms[i].grid, false,
                         s_phantoms[i].tone_hz, s_phantoms[i].level_db, slot_sec);
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
// `to_call` is who the phantom is addressing - us for every ordinary reply, but a
// Fox working its queue addresses somebody else entirely, which is the whole point
// of the busy-frequency test (see s_fox_queue).
static void set_pending(ft8_sim_phantom_t *ph, const char *to_call,
                        const char *extra, bool use_fd,
                        bool early, int64_t first_slot, int repeats)
{
    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr, score;
    // Fade between transmissions here too, not just while calling CQ: a partner
    // whose signal moves mid-exchange is the whole reason a re-sent report has
    // to be refreshed (#264), and with a fixed level that path could never be
    // exercised on the bench.
    level_drift(ph);
    if (!build_message(to_call, ph->call, extra, use_fd, ph->tone_hz, ph->level_db,
                       text, sizeof(text), &snr, &score)) {
        ph->pend_active = false;
        return;
    }

    // Re-scheduling the SAME message must NOT restart its timer. We detect our
    // own burst every slot we re-send on, so without this the pending reply is
    // pushed forward every 30 s and, with "Fast pounce" OFF, never fires at all:
    // the reply is due at first_slot + 16 s = T+31 while our next re-send lands
    // at T+30 and resets it to T+45. It loses by ~1 s, forever - the QSO times
    // out and the reply finally appears ~12 slots late the instant we stop
    // re-sending (hardware-observed twice, 2026-07-28). With Fast pounce ON the
    // due time is T+28 and it happened to win, which is why this hid for so
    // long. A real operator repeating themselves doesn't restart their
    // partner's reply timer either, so keeping the original schedule (and the
    // remaining patience) is also the more faithful behaviour.
    // Compared with the stored field's own capacity so a truncated pend_text
    // can't make every comparison a miss and silently restore the bug.
    if (ph->pend_active &&
        strncmp(ph->pend_text, text, sizeof(ph->pend_text) - 1) == 0) {
        ph->engaged = true;
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


// ---- Slot arithmetic, on whichever protocol is running -----------------------
//
// ⛔ NOT `+ 15`. FT8 slots are 15 s and FT4 slots are 7.5 s, and the engine
// records a slot by its whole-second truncation of a MILLISECOND boundary
// (ft8_test.c: `slot_sec = boundary_ms / 1000`). So in FT4 the ids step 7, 8,
// 7, 8 - and a phantom reply scheduled 15 seconds after "now" lands in a slot
// that half the time does not exist. ft8_qso_advance() scans for
// `last_utc == slot_sec`, so those replies were never seen and no simulated FT4
// QSO could ever complete (#256, found while trying to verify the FT4 ADIF
// change - the operator has no FT4 partner on the bench either).
//
// Both helpers work in ms on the same grid the engine uses, then truncate the
// same way it does.
static int64_t sim_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// The slot id for the boundary we are inside right now.
static int64_t sim_cur_slot(void)
{
    int64_t period = ft8_op_mode_slot_ms();
    return ((sim_now_ms() / period) * period) / 1000;
}

// The id of the slot AFTER the one `slot_sec` names. Snaps back to the true
// millisecond boundary first, because slot_sec has already lost up to 999 ms of
// it - in FT4 that is the difference between the right slot and no slot.
static int64_t sim_next_slot(int64_t slot_sec)
{
    int64_t period = ft8_op_mode_slot_ms();
    int64_t ms     = slot_sec * 1000;
    int64_t bound  = ((ms + period / 2) / period) * period;   // nearest grid point
    return (bound + period) / 1000;
}

// One slot's worth of fading for one phantom: a slow random walk, occasionally
// a deeper dip, held inside a range where the strongest are easy and the weakest
// are genuinely marginal. Nothing here pretends to be a propagation model - it
// only has to make the number MOVE, because a report that never changes cannot
// exercise the code that re-sends it.
#define SIM_LEVEL_MIN_DB  (-18)
#define SIM_LEVEL_MAX_DB  (12)
// Spread the pool out at the start: all six sitting on the same level would be
// the old problem in a new place.
static void levels_seed(void)
{
    for (int i = 0; i < N_PHANTOMS; i++)
        s_phantoms[i].level_db = SIM_LEVEL_MIN_DB +
                                 (int)(esp_random() % (SIM_LEVEL_MAX_DB - SIM_LEVEL_MIN_DB + 1));
}

static void level_drift(ft8_sim_phantom_t *ph)
{
    int step = (int)(esp_random() % 7) - 3;              // -3..+3 dB
    if ((esp_random() & 0x1F) == 0) step -= 6;           // an occasional fade
    ph->level_db += step;
    if (ph->level_db < SIM_LEVEL_MIN_DB) ph->level_db = SIM_LEVEL_MIN_DB;
    if (ph->level_db > SIM_LEVEL_MAX_DB) ph->level_db = SIM_LEVEL_MAX_DB;
}

// Phantoms answer our CQ. Engages up to SIM_PILEUP_CALLERS idle phantoms,
// each calling us in the SAME reply slot (a genuine pileup) at their own
// tones - and, like real operators, each keeps calling every 30 s until
// answered or out of patience. The QSO machine works one; the rest populate
// the pileup tracker/viewer. No-op if none are idle.
static void schedule_cq_answer(const char *my_call, int64_t our_slot)
{
    int64_t reply_slot = sim_next_slot(our_slot);
    int n = 0;
    for (int i = 0; i < N_PHANTOMS && n < SIM_PILEUP_CALLERS; i++) {
        if (s_phantoms[i].engaged || s_phantoms[i].worked) continue;
        if (s_phantoms[i].deaf) continue;   // can't hear our CQ either
        // A Fox never answers anyone's CQ - it IS the pileup's centre, and a Fox
        // in our pileup would be nonsense to anyone watching the decode list.
        if (s_phantoms[i].is_fox) continue;
        // Pileup answers land EARLY in the slot (instant landing - text is
        // pre-synthesized) so all of them are inside the slot's scan.
        //
        // A `terse` phantom answers with its report of us instead of its grid,
        // which is what an operator who already knows us does. Our reply must
        // then be R<report>, not another bare report (Gyula HA3HZ).
        char third[16];
        if (s_phantoms[i].terse) fmt_report(-9, third, sizeof(third));
        else                     snprintf(third, sizeof(third), "%s", s_phantoms[i].grid);
        set_pending(&s_phantoms[i], my_call, third, false, true,
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

    if (ph->deaf) {
        ESP_LOGI(TAG, "%s is deaf - ignoring our call (grey-list practice target)", ph->call);
        return;
    }

    // FOX: ignore the first few calls and work somebody else instead - a pileup,
    // in other words. This is the condition the Hound code has to survive: the
    // Fox's message is addressed to a third party, so ft8_qso.c's
    // partner_busy_with() sees a busy frequency and, WITHOUT the Hound
    // suspension, would hold our TX until its budget ran out. Which would mean
    // sitting silent through the pileup instead of calling into it.
    //
    // Only the opening call (our grid) gets ignored. Once the Fox has answered us
    // it plays the exchange straight through - a real Fox that has committed to a
    // hound finishes with it, and the interesting part after that is the QSY.
    if (ph->is_fox && ph->fox_ignore_left > 0 &&
        sent_extra[0] != 'R' && sent_extra[0] != '+' && sent_extra[0] != '-') {
        static int qi = 0;
        const char *victim = s_fox_queue[qi++ % (int)(sizeof(s_fox_queue) / sizeof(s_fox_queue[0]))];
        ph->fox_ignore_left--;
        ESP_LOGI(TAG, "FOX %s ignores our call (%d left) - working %s instead",
                 ph->call, ph->fox_ignore_left, victim);
        // Lands EARLY in the Fox's own slot so it is in the decode table before
        // ft8_qso_advance() scans that slot - the same reason pileup answers use
        // the early flag. One send: the Fox moves on to the next hound.
        set_pending(ph, victim, "-13", false, true, sim_next_slot(our_slot), 1);
        // NOT engaged: it owes us nothing, and leaving it engaged would stop its
        // idle CQs, which are what keep it looking like a Fox in the decode list.
        ph->engaged = false;
        return;
    }

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

    set_pending(ph, my_call, extra, use_fd, false, sim_next_slot(our_slot), repeats);
}

static void ft8_sim_task(void *arg)
{
    (void)arg;
    int64_t last_tx_slot = -1;   // slot of the last burst we handled (see below)
    int64_t last_cq_inject_sec = 0;
    levels_seed();               // #265: spread the pool out before anyone calls
    bool was_active = false;
    bool warned_no_call = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        qmx_settings_t s;
        settings_load_all(&s);
        // FT4 WORKS HERE NOW (#256). This used to be FT8-only, and the reason
        // given was honest: the synth was hardcoded to FT8's waveform and the
        // slot arithmetic to a flat 15 s, so phantom traffic in FT4 would have
        // been nonsense - which is why sim mode switched itself off. Both halves
        // are fixed: ft8_synth_and_decode_at() encodes and decodes in whichever
        // protocol is running, and sim_next_slot() follows the real grid (FT4
        // slot ids step 7, 8, 7, 8 - never a flat 15, which is why a phantom's
        // reply used to land in a slot that did not exist).
        //
        // It matters because the simulator is how an FT4 change gets tested at
        // all: nobody has an FT4 partner on the bench, and the last FT4 fix had
        // to ship host-tested only.
        /* AND the FT8 PAGE must actually be up - a separate condition from the
         * protocol one above, kept when the WSPR page landed.
         *
         * sim_mode_en is ONE switch shared with the WSPR sim, so turning it on
         * to work on WSPR without a radio also turned this on - and this task
         * does a full GFSK synthesis plus a REAL decode per phantom, whose
         * decode half runs on the core-0 helper. Measured on the WSPR page with
         * the radio wedged: core 0 sat at 0.0% idle and the WSPR sim's own
         * window synthesis was starved for minutes, against ~73% idle on the
         * same page in live RX. Injecting phantoms into a decode list nobody is
         * looking at is pure cost, so gate on the page as well as the setting. */
        if (!s.sim_mode_en || ui_mode_get() != UI_MODE_FT8) {
            if (was_active) {
                ESP_LOGI(TAG, "sim mode idle (off, or not on the FT8 page)");
                was_active = false;
                // Fresh session next time: worked phantoms answer CQs again,
                // and nothing pending survives the toggle.
                for (int i = 0; i < N_PHANTOMS; i++) {
                    s_phantoms[i].worked      = false;
                    s_phantoms[i].engaged     = false;
                    s_phantoms[i].pend_active = false;
                    // The Fox's patience resets too, so each sim session starts
                    // with a pileup to fight through rather than a Fox that
                    // answers instantly because a previous session used it up.
                    if (s_phantoms[i].is_fox)
                        s_phantoms[i].fox_ignore_left = SIM_FOX_IGNORE_SLOTS;
                }
                // Wipe the phantoms out of the decode list and pileup too.
                // Without this they lingered on screen after the toggle - up to
                // FT8_ROW_STALE_SEC of fake stations sitting in a list the
                // operator has just told us is supposed to be real, which is
                // both confusing and tappable (a pounce at a station that no
                // longer exists). Safe from this task: both take their own
                // mutex, and the view rebuilds from the 1 Hz refresh.
                ft8_screen_clear();
                ft8_pileup_clear();
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
        if (!was_active) {
            ESP_LOGI(TAG, "sim mode ON (my_call=%s)", s.my_callsign);
            was_active = true;
            // Mirror the OFF transition below: real decodes/pileup callers
            // sitting on screen from before the toggle are exactly as
            // confusing and tappable here as leftover phantoms are after
            // leaving sim mode - a station you might pounce that the
            // operator has just told us is not real. Clear on the way IN,
            // not just on the way out.
            ft8_screen_clear();
            ft8_pileup_clear();
        }

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
        int64_t cur_slot = sim_cur_slot();
        // TWO SLOTS OF THE PROTOCOL THAT IS RUNNING, not a hardcoded 30 s.
        // Consecutive bursts are parity-locked two slots apart: 30 s in FT8 but
        // 15 s in FT4 - so a flat 30 ignored every FT4 burst after the first,
        // the phantom never saw our report go out, never sent its roger, and
        // every simulated FT4 QSO timed out. Operator, watching the screen:
        // "seems that a qso is never really initiated?" - exactly that. The
        // FT4 ids truncate to a diff of exactly 15, so the comparison stays
        // exact rather than needing slop.
        int64_t min_sep = ((int64_t)ft8_op_mode_slot_ms() * 2) / 1000;
        if (tx_state == FT8_TX_ACTIVE &&
            (last_tx_slot < 0 || cur_slot - last_tx_slot >= min_sep)) {
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

        // FOX: work the queue continuously, whether or not anyone has called us.
        //
        // A real Fox transmits in every one of its slots, mostly reports and RR73s
        // to the hounds it is working, with the occasional CQ. That pattern is not
        // decoration here - it is the ONLY thing that distinguishes a Fox from an
        // ordinary station low in the passband, and ft8_hound_looks_like_fox()
        // requires it (several distinct addressees within a few minutes). The first
        // version of this Fox only worked others while ignoring our calls, which
        // left it indistinguishable from any other CQ caller until we had already
        // called it - so detection could never fire first, and the one station that
        // DID get identified as a Fox was a phantom at 700 Hz that simply calls CQ.
        {
            static int64_t last_fox_work = 0;
            int64_t nowq = time(NULL);
            for (int i = 0; i < N_PHANTOMS; i++) {
                ft8_sim_phantom_t *fx = &s_phantoms[i];
                if (!fx->is_fox || fx->engaged || fx->pend_active) continue;
                // Every other slot (30 s), which is the Fox's own parity.
                if (nowq - last_fox_work < 30) continue;
                last_fox_work = nowq;
                static int qn = 0;
                const int nq = (int)(sizeof(s_fox_queue) / sizeof(s_fox_queue[0]));
                const char *victim = s_fox_queue[qn++ % nq];
                // Alternate report / RR73 so the queue looks like real traffic
                // rather than one message repeated at different callsigns.
                const char *extra = (qn % 2) ? "-11" : "RR73";
                set_pending(fx, victim, extra, false, true,
                            sim_next_slot(sim_cur_slot()), 1);
                fx->engaged = false;   // it owes US nothing; keep it CQ-able too
                break;                 // one per iteration - synth is not free
            }
        }

        // Idle-CQ refresh: one phantom per loop iteration (see
        // inject_next_idle_cq), restarted every CQ_REINJECT_PERIOD_SEC.
        static int s_cq_idx = N_PHANTOMS;   // pool position; ==N when batch done
        int64_t now = time(NULL);
        // Only start a new batch once the previous one FINISHED. Each phantom
        // costs ~3.2 s of synth+decode, so a 7-strong pool needs ~22 s - and
        // restarting the index on the timer alone silently starved whichever
        // phantom was last in the pool: adding the Fox (7th) made it CQ never,
        // which is exactly the symptom that showed up on the bench, since a Fox
        // that never transmits cannot be detected as one. The pool can grow
        // without anyone having to re-derive this arithmetic now.
        if (now - last_cq_inject_sec >= CQ_REINJECT_PERIOD_SEC && s_cq_idx >= N_PHANTOMS) {
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
