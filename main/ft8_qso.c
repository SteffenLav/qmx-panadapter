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
#include "ft8_pileup.h"
#include "ft8_greylist.h"
#include "ft8_hound.h"  // Fox/Hound (DXpedition) rules - see the s_hound_active notes
#include "ft8_tx.h"
#include "ft8_test.h"   // ft8_op_mode_get() - FT8/FT4 sub-mode, for ADIF MODE
#include "ft8_status.h"
#include "ui/ft8_screen.h"
#include "storage/settings.h"
#include "adif/adif_log.h"
#include "net/spots.h"  // spots_activation_for_call() - tag a chase with SIG/SIG_INFO
#include "cat/cat.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_qso";

// How many consecutive RX slots with no expected reply before we give up on
// the station we're working. ~6 cycles of patience (the user can wander off
// and come back, fading, QRM, etc.). Bumped from 4 after a logged near-miss
// (2026-06-26): an OH5KNL RR73 landed just 29s after a 4-slot timeout fired -
// they needed one more cycle to successfully copy our report and reply.
#define QSO_TIMEOUT_SLOTS  6

// Clamp the SNR we report to a sane FT8 range.
#define RPT_MIN_DB  (-24)
#define RPT_MAX_DB  (+15)

// ARRL Field Day exchange text, max len "32F WCF" + "R " prefix + NUL.
#define FT8_FD_EXCH_LEN 20

static SemaphoreHandle_t  s_lock;
static ft8_qso_state_t    s_state          = FT8_QSO_IDLE;
static char               s_target[FT8_CALL_MAX_LEN];   // their callsign
static char               s_my_call[FT8_CALL_MAX_LEN];  // our callsign (uppercased)
static int                s_freq_hz;                    // OUR AF tone for our own replies
// #201: has THIS exchange put anything on the air yet? Until it has, our tone
// is nobody's reference and may still be re-picked; from the first burst
// onwards the partner is tracking it and it must not move. Set by
// ft8_qso_on_tx_complete(), cleared wherever an exchange begins.
static bool s_tx_sent = false;

// #219: relocations made AFTER our first transmission, i.e. mid-exchange.
// Bounded so a busy band cannot make us wander up and down the waterfall while
// the partner is trying to find us. Reset with every new exchange.
static int s_midqso_moves = 0;

// How far a MID-QSO move may go, and how many are allowed.
//
// Roy KI0ER established that moving mid-QSO is legitimate at all: decoders work
// the whole passband and match on callsign, so the partner is not following our
// tone. Gyula HA3HZ established the limit: a station who has NARROWED their
// receive bandwidth (MSHV does this to keep decode inside the reply window) may
// simply not hear someone who jumped far away, and his own conclusion was that
// you therefore cannot say flatly that changing frequency mid-QSO is right.
//
// Both are satisfied by moving as LITTLE as possible: far enough to clear the
// station on top of us, near enough to stay inside a narrowed window. 250 Hz is
// five 50 Hz slots - comfortably clear of a neighbour, and well inside any
// plausible narrowed passband.
#define QSO_MIDQSO_MOVE_MAX_HZ  250
#define QSO_MIDQSO_MOVE_LIMIT   3
// The PARTNER's AF tone - where THEIR next message will arrive. Deliberately
// NOT s_freq_hz: for a pounce we answer on a clear slot chosen by
// ft8_find_clear_tone_hz() (see ft8_screen_view.c "not the CQ station's own
// tone"), so our TX tone and their TX tone are different frequencies and were
// conflated until 2026-07-28 - ft8_qso_get_priority_freq() handed our own tone
// to the decode-priority reorder, aiming it ~250 Hz off the partner every time.
// 0 = unknown, in which case callers must fall back to no hint rather than a
// wrong one. Seeded at QSO start from the decode table, refreshed every slot we
// hear them in scan_for_response().
static int                s_partner_freq_hz;

// ---- Fox/Hound (DXpedition) session state ----------------------------------
//
// Set when a contact is started against something that looks like a Fox while
// Hound mode is enabled (see ft8_hound.h for the protocol and for why the Fox
// side cannot exist on this radio). It changes four things, all of them in
// ft8_qso.c and each marked with a comment naming this flag:
//
//   1. our R-report is sent AFTER QSY'ing onto the Fox's own frequency;
//   2. the Fox's RR73 ends the QSO - a hound never sends 73;
//   3. the busy-station hold stands down (a Fox is always working somebody, so
//      holding for a free frequency means never calling);
//   4. the final re-send and the grey-list stand down (the Fox never asks
//      again, and being ignored in a pileup is not a station that never
//      answers).
//
// s_hound_tone_hz remembers the up-band tone we called from, so the QSY is
// reversible: after the contact we go back there for the next Fox.
static bool               s_hound_active;
static int                s_hound_tone_hz;
// A tone the operator chose while a burst was on the air. Applied by
// on_tx_complete() the moment the burst ends - see ft8_qso_set_tx_tone_hz().
static int                s_pending_tone_hz;
static int64_t            s_min_scan_utc;               // pounce: don't scan before TX1 fires
static int                s_missed_slots;
static bool               s_from_cq;                    // session started as CQ-run
static ft8_tx_request_t   s_cur_req;                    // message we're currently sending
static bool               s_have_cur;                   // s_cur_req valid
static ft8_tx_request_t   s_cq_saved;                   // original CQ, to resume after a dropped QSO
static bool               s_have_cq_saved;
// --- CQ auto-stop (Don WB0LQW: "I usually send CQ 2-4 times and then pause") -
// s_cq_calls_sent counts CQ bursts actually transmitted in the current CQ
// sequence; when it reaches the cq_max_calls setting (0 = unlimited),
// rearm_current() stops re-arming and sets s_cq_exhausted, the loop listens
// through one more RX slot (an answer to the final call still starts a QSO
// normally), and advance()'s no-answer path then ends the session to IDLE.
// Every fresh CQ sequence - Call CQ, a timeout resume, a post-QSO resume -
// starts the count over.
static int                s_cq_calls_sent;
// Which cq_calls_sent value we last spent a listening slot at, so the pause
// fires once per multiple of cq_listen_every instead of latching there.
static int                s_cq_listen_done_at = -1;
static bool               s_cq_exhausted;
// Station being worked MANUALLY (step-by-step Transmit taps, no machine QSO).
// Noted on every manual arm so (a) the pileup capture doesn't list our own
// partner as a waiting caller mid-exchange, and (b) the decode list can show
// the same amber "working" highlight a machine QSO gets. Expires after
// MANUAL_TARGET_TTL_S of no manual activity; cleared on QSO completion.
#define MANUAL_TARGET_TTL_S 300
static char               s_manual_target[FT8_CALL_MAX_LEN];
static int64_t            s_manual_target_ts;
// Station we most recently COMPLETED a QSO with. Their trailing 73/RR73 (and
// a late repeat) keeps addressing us for a while after completion - exempt
// them from the pileup capture for a grace period so a finished contact never
// re-enters the pileup (Dirk DK7CVD's lingering-call report), without the
// blanket worked-before skip that also hid legitimate dupe callers whenever
// the operator had "Exclude worked before" OFF.
#define DONE_GRACE_TTL_S 180
static char               s_last_done_call[FT8_CALL_MAX_LEN];
static int64_t            s_last_done_ts;

// --- A partner who never decoded our final ----------------------------------
// Field report (Roy KI0ER, 2026-07-29, working VE3INB): the exchange completed
// from our side - we sent R73, logged the QSO and moved on - but VE3INB never
// decoded that final, so he kept sending "KI0ER VE3INB R-10" waiting for it.
// Nothing in the machine could act on that: the comeback-resume record is
// deliberately erased on completion (never auto-resume into a duplicate), and a
// completed QSO is otherwise finished. So he stayed unfinished on his side while
// we started working someone else, and every manual attempt to help him logged
// ANOTHER copy of the same QSO.
//
// Both halves are fixed here. The tone is "keep answering while they're still
// asking", which is what WSJT-X does and what actually gets us into their log:
// if the just-worked station addresses us again with a report (not RR73/73/RRR,
// which would mean they DID hear us) inside the window, re-send our final once
// more - bounded, and without logging again.
// Raised from 240 s / 3 on a field report (Roy KI0ER, 2026-08-05): a partner who
// never heard our final repeats every cycle for minutes, and 3 tries inside 4
// minutes ran out while he was still asking - at which point we went back to
// calling CQ over the top of him. A human would simply keep answering, so the
// budget is now 6 inside 5 minutes.
#define FINAL_RESEND_WINDOW_SEC 300
#define FINAL_RESEND_MAX          6
static char               s_final_call[FT8_CALL_MAX_LEN];  // who we owe a final to ('\0' = nobody)
static int                s_final_freq_hz;                 // our tone for it
static ft8_tx_kind_t      s_final_kind;                    // 73 (pounce) or RR73 (CQ-run)
static char               s_final_extra[16];               // the final's third field (matches ft8_tx_request_t)
static int                s_final_resends;

// True while a just-worked partner is still asking for our final. Set once per
// RX slot by advance(); read by rearm_current(), which is the single choke point
// for CQ arming. This is what stops us calling CQ over somebody who is waiting
// on us even after the re-send budget is spent - staying silent is the polite
// answer, and it was Roy KI0ER's own suggestion.
static bool s_final_hold = false;

// Duplicate-log guard. The log is written once at WAIT_DONE -> DONE, but that
// state can legitimately be entered twice for one contact - most easily by
// ft8_qso_notify_manual_final() when the operator takes over by hand, exactly as
// Roy did above. Same call on the same band inside this window is the same
// contact, not a second one.
#define QSO_DUP_LOG_WINDOW_SEC 600
static char               s_logged_call[FT8_CALL_MAX_LEN];
static uint32_t           s_logged_freq_hz;
static int64_t            s_logged_ts;

// ---------------------------------------------------------------------------
// RECENTLY-WORKED GRACE (Gyula HA3HZ, 2026-08-28; BD4AHS said the same on
// 2026-08-06 and was told to tick a checkbox)
//
// The decode list greys a row whenever adif_log_contains_call_on_band() says we
// have logged that station on this band - UNCONDITIONALLY. Every ENGINE path
// skipped the same station only `if (excl_worked_before && ...)`. So on the
// default the screen said "worked" in dim grey and the machine called them
// again anyway: *"When I finish a QSO and his callsign turns gray, he calls
// again shortly after - as if there was no previous completed QSO."*
//
// Two field reports of one behaviour is evidence the default is wrong, not that
// two operators misconfigured it. But the checkbox is not simply wrong either -
// an operator may legitimately want to re-work a station later, after a band
// opening changes. So the fix is bounded in TIME rather than absolute: the
// UNATTENDED pickers never re-work a station logged on this band within
// RECENT_WORKED_GRACE_SEC, whatever the checkbox says, and beyond that window
// the checkbox rules exactly as before.
//
// ⭐ There is a second, harder reason this must be unconditional. The duplicate
// guard above REFUSES TO LOG a repeat inside QSO_DUP_LOG_WINDOW_SEC - so
// without this, the machine transmits a complete exchange, keys the radio for
// ~12.6 s per burst, and then throws the result away. Working a contact we have
// already decided not to log is worse than declining to work it.
//
// ⚠ RAM-only, deliberately. It answers "did we work them in this session,
// recently", which is exactly the reported symptom; the ADIF log (via the
// checkbox) still covers everything older. A reboot clearing it is harmless
// because the grace is minutes, not days. A ring rather than one slot because
// the existing s_logged_* is a single entry - fine for the dup guard, useless
// here the moment one other station is worked in between.
// Extracted to main/ft8_recent.c so test/ft8_recent_harness.c can link the real
// functions and step the clock at will. That was not gold-plating: the on-device
// route needs a completed QSO, and in FT8 simulation mode on 2026-08-28 the
// decode ran 15.4 s against a 15 s slot, so slots overran 2x and the robot never
// saw a decode whose slot matched the current one. Six mutations of the logic are
// caught by the harness, one of which exposed a hole in the tests themselves.
#include "ft8_recent.h"

static void note_worked_now(const char *call, uint32_t freq_hz)
{
    ft8_recent_note(call, adif_log_band_for_freq(freq_hz), (int64_t)time(NULL));
}

bool ft8_qso_worked_recently(const char *call, uint32_t freq_hz)
{
    return ft8_recent_worked(call, adif_log_band_for_freq(freq_hz), (int64_t)time(NULL));
}

// Signal reports captured during the exchange for ADIF logging.
// Pounce: rst_rcvd = what they told us; rst_sent = our own locally-measured SNR
//   of them (the protocol never has us transmit a numeric report of them - TX2
//   just rogers their report back - so this is synthesized like WSJT-X does).
// CQ-run: rst_sent = our report of their signal; rst_rcvd = the value in their
//   numeric roger "R<rpt>" (or a direct report answer). The R-report is their
//   own measurement of OUR signal, NOT an echo of the report we sent - proven
//   on air 2026-07-15 (we sent -08, OS4K rogered R-06). Either field is left
//   EMPTY if the exchange completes without us ever hearing a numeric value
//   (e.g. they jump straight to RR73 after a grid answer); adif_log.c then
//   omits it. Never substitute "599" - that would be a fabricated measurement
//   uploaded to QRZ/eQSL/LoTW as if real (Roy KI0ER, 2026-07-29).
static char               s_rst_sent[8];
static char               s_rst_rcvd[8];
/* Their numeric report of us, lifted from the message a manual/pileup reply
 * was built from, so the R-report entry can record it as RST_RCVD (#292). */
static char               s_heard_their_rpt[8];
static char               s_heard_their_rpt_call[16];
// ARRL Field Day: their class+section, captured when their roger/exchange
// message is recognized (WAIT_RPT pounce path, WAIT_ROGER cqrun path). Empty
// when the QSO isn't a Field Day exchange.
static char               s_fd_their_exch[FT8_FD_EXCH_LEN];

// ---------------------------------------------------------------------------

// Classify an already-truncated-to-whole-seconds slot_sec (e.g. a heard
// station's ft8_call_t.last_utc). FT4's 7.5 s grid needs a different formula
// from FT8's plain "/15" - see the long derivation at ft8_tx.c's own
// slot_is_even() (the two are intentionally identical; kept as separate
// static copies rather than shared across translation units, same as before
// this fix). Briefly: floor(k*7.5) mod 15 is always exactly 0 (k even) or 7
// (k odd), so the truncation loses no parity information.
static inline bool slot_is_even(int64_t sec, ftx_protocol_t proto)
{
    if (proto == FTX_PROTOCOL_FT4) return (sec % 15) == 0;
    return ((sec / 15) % 2) == 0;
}

// UTC slot_sec (matching exactly what ft8_test.c's slot loop will itself
// produce as boundary_ms/1000) of the next slot boundary strictly in the
// future, optionally constrained to a parity. Works in milliseconds
// internally using proto's real period (15000 FT8 / 7500 FT4) so the result
// is always a boundary the engine actually produces - unlike naively
// stepping by a literal 15 in whole seconds, which for FT4 doesn't track the
// real 7.5 s grid at all (the bug this replaces: ft8_qso_start()'s old
// tx1_slot search could pick a "slot" that no real FT4 boundary ever lands
// on, or mis-classify a boundary it did land on, so a parity-restricted
// pounce/CQ-lock could silently target a slot that would never actually
// fire or be scanned).
static int64_t next_slot_sec(bool match_parity, bool want_even, ftx_protocol_t proto)
{
    int period_ms = (proto == FTX_PROTOCOL_FT4) ? 7500 : 15000;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms  = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t next_ms = (now_ms / period_ms) * period_ms + period_ms;
    if (match_parity) {
        while ((((next_ms / period_ms) % 2) == 0) != want_even) next_ms += period_ms;
    }
    return next_ms / 1000;
}

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
// which can also begin with 'R' for far-east locators like RE78). Also covers
// the ARRL Field Day roger form "R 16A EMA" (R followed by a space then their
// class+section) - that combination of leading R + space never occurs in a
// plain grid/report token, so no fd_mode gate is needed.
static bool is_roger_token(const char *t)
{
    if (t[0] != 'R') return false;
    char c = t[1];
    return c == '-' || c == '+' || c == 'R' || c == ' ' || (c >= '0' && c <= '9');
}

// Strip the <angle brackets> ft8_lib puts around a hash-resolved callsign
// ("<KN6LFB>" -> "KN6LFB") so token matching works on the bare call. An
// unresolved hash decodes as "<...>" and becomes "...", which can never
// match a real callsign - correct (we don't know who it is).
static void strip_hash_brackets(char *tok)
{
    size_t n = strlen(tok);
    if (n >= 2 && tok[0] == '<' && tok[n - 1] == '>') {
        memmove(tok, tok + 1, n - 2);
        tok[n - 2] = '\0';
    }
}

// Split a decoded message into its first two tokens (callsigns) and "rest"
// (everything after, trimmed of leading spaces, verbatim - may itself
// contain spaces, e.g. an ARRL Field Day "R 16A EMA" exchange). Replaces a
// fixed 3-token sscanf, which truncated multi-word third fields.
// Hash brackets are stripped from both callsign tokens (see above).
static bool split_msg3(const char *text, char *tok1, size_t cap1,
                       char *tok2, size_t cap2, char *rest, size_t cap_rest)
{
    const char *p = text;
    while (*p == ' ') p++;
    const char *s1 = p;
    while (*p && *p != ' ') p++;
    size_t l1 = (size_t)(p - s1);
    if (l1 == 0 || l1 >= cap1) return false;
    memcpy(tok1, s1, l1); tok1[l1] = '\0';

    while (*p == ' ') p++;
    const char *s2 = p;
    while (*p && *p != ' ') p++;
    size_t l2 = (size_t)(p - s2);
    if (l2 == 0 || l2 >= cap2) return false;
    memcpy(tok2, s2, l2); tok2[l2] = '\0';

    while (*p == ' ') p++;
    snprintf(rest, cap_rest, "%s", p);

    strip_hash_brackets(tok1);
    strip_hash_brackets(tok2);
    return true;
}

// Format a coarse SNR into an FT8 report token: "-07", "+02", "-15".
static void fmt_report(int snr_db, char *out, size_t len)
{
    if (snr_db < RPT_MIN_DB) snr_db = RPT_MIN_DB;
    if (snr_db > RPT_MAX_DB) snr_db = RPT_MAX_DB;
    snprintf(out, len, "%+03d", snr_db);
}

// Public wrapper so UI code (ft8_pileup_modal.c) formats reports with the
// exact same clamping/format convention instead of growing another private
// copy (ft8_sim.c already has one - don't add a third).
void ft8_qso_fmt_report(int snr_db, char *out, size_t len)
{
    fmt_report(snr_db, out, len);
}

// Build "R<report>" for TX2. Their report is e.g. "-10"; pass through if it
// already starts with 'R'.
static void make_roger(const char *their_report, char *out, size_t len)
{
    if (their_report[0] == 'R') snprintf(out, len, "%s", their_report);
    else                        snprintf(out, len, "R%s", their_report);
}

// --- Intelligent manual "Transmit" (operator request 2026-07-22, Roy KI0ER)-
// The plain Transmit button used to always fire TX1 (our grid) regardless of
// what the tapped station last said, so a half-finished QSO could never be
// nudged forward by hand. This derives the correct NEXT outgoing message
// purely from the decoded row relative to our own call - exactly how WSJT-X's
// double-click generates the next standard message - so Transmit advances the
// sequence step by step:
//
//   their CQ / not-to-us   -> TX1 grid       "<them> <me> <grid>"
//   <me> <them> <grid>     -> signal report  "<them> <me> <snr>"   (they answered our CQ)
//   <me> <them> <rpt>      -> R + OUR report  "<them> <me> R<snr>"  (they reported us)
//   <me> <them> R<rpt>     -> RR73            "<them> <me> RR73"
//   <me> <them> RR73|73    -> 73              "<them> <me> 73"
//
// The R<snr>/report value is OUR locally-measured SNR of them (heard->last_snr_db),
// never an echo of the number they sent us - the same convention the automatic
// machine uses (see WAIT_RPT/cqrun_answer). Field Day mode keeps the plain TX1
// build (its class+section exchange isn't mirrored here); Auto Pounce still
// runs the full FD sequence. *is_fresh_grid, if non-NULL, is set true only for
// the TX1-grid case - the one where "Auto Pounce" still makes sense.
bool ft8_qso_build_manual_reply(const ft8_call_t *heard, int reply_freq_hz,
                                ft8_tx_request_t *out, bool *is_fresh_grid,
                                char *err, size_t err_len)
{
    if (is_fresh_grid) *is_fresh_grid = true;
    if (err && err_len) err[0] = '\0';
    if (!heard || !out) {
        if (err && err_len) snprintf(err, err_len, "No station selected");
        return false;
    }
    if (!load_my_call(err, err_len)) return false;   // populates s_my_call

    ft8_tx_kind_t kind  = FT8_TX_KIND_REPLY;
    const char   *extra = NULL;                      // NULL => my_grid (TX1)
    char          extra_buf[16] = {0};
    bool          fresh_grid = true;

    qmx_settings_t qs;
    settings_load_all(&qs);
    bool fd_mode = qs.field_day_en && qs.fd_class[0] && qs.fd_section[0];

    // Skip-TX1 applies to the manual Transmit exactly like WSJT-X applies
    // "Skip Tx1" to a double-click: a fresh CQ answer opens with our signal
    // report instead of the grid. ft8_qso_start() already honours a
    // report-carrying REPLY (arms unchanged, starts in WAIT_ROGER), so Auto
    // Pounce from the same modal stays consistent. Field Day keeps grid TX1
    // (the FD exchange replaces the report step entirely). Field-reported by
    // the operator: Transmit on a CQ row sent TX1 despite Skip-TX1 checked.
    if (!fd_mode && qs.ft8_filters.skip_tx1) {
        fmt_report(heard->last_snr_db, extra_buf, sizeof(extra_buf));
        extra = extra_buf;
    }

    char tok1[16] = {0}, tok2[24] = {0}, rest[40] = {0};
    bool parsed = split_msg3(heard->last_text, tok1, sizeof(tok1),
                             tok2, sizeof(tok2), rest, sizeof(rest));

    // Only advance the ladder when the message is addressed to us AND we're not
    // in Field Day mode (its exchange uses class+section, handled by Pounce).
    if (!fd_mode && parsed && s_my_call[0] &&
        strcmp(tok1, s_my_call) == 0 && rest[0]) {
        fresh_grid = false;
        if (strcmp(rest, "RR73") == 0 || strcmp(rest, "73") == 0) {
            kind = FT8_TX_KIND_73;                       // they signed off -> 73
            snprintf(extra_buf, sizeof(extra_buf), "73");
            extra = extra_buf;
        } else if (is_roger_token(rest)) {
            kind = FT8_TX_KIND_73;                       // they rogered us -> RR73
            snprintf(extra_buf, sizeof(extra_buf), "RR73");
            extra = extra_buf;
        } else if (rest[0] == '-' || rest[0] == '+') {
            char myrpt[8];                               // they reported us -> R<our rpt>
            fmt_report(heard->last_snr_db, myrpt, sizeof(myrpt));
            make_roger(myrpt, extra_buf, sizeof(extra_buf));
            kind  = FT8_TX_KIND_ROGER_RPT;
            extra = extra_buf;
            /* ⭐ THIS is RST_RCVD - their own measurement of us - and it was
             * being thrown away (#292, Gyula HA3HZ).
             *
             * A QSO entered here skips straight to TX2 because, as #234's own
             * comment puts it, "the partner's OWN last message already reported
             * us". WAIT_RPT - the state that normally records their report -
             * therefore never runs, and if they then jump to RR73, WAIT_ROGER
             * never runs either. The QSO logs with RST_SENT and an empty
             * RST_RCVD, which the viewer shows as a dash.
             *
             * From his log: decoded 'HA3HZ UN6GO -02' ... "Logged QSO #157:
             * UN6GO (-09/)". Their -02 was in the very message that started the
             * QSO. 2 of his 24 logged QSOs lost it this way.
             *
             * The identical fix already existed on the CQ-RUN entry, with the
             * same reasoning written out - and was never applied to this
             * sibling path. */
            strncpy(s_heard_their_rpt, rest, sizeof(s_heard_their_rpt) - 1);
            s_heard_their_rpt[sizeof(s_heard_their_rpt) - 1] = '\0';
            /* Tagged with WHOSE report it is: this is a file-static read a
             * moment later by ft8_qso_start(), and a preview build for a
             * different row in between would otherwise hand the wrong
             * station's number to the log. */
            strncpy(s_heard_their_rpt_call, heard->call, sizeof(s_heard_their_rpt_call) - 1);
            s_heard_their_rpt_call[sizeof(s_heard_their_rpt_call) - 1] = '\0';
        } else {
            fmt_report(heard->last_snr_db, extra_buf, sizeof(extra_buf)); // grid -> our report
            kind  = FT8_TX_KIND_REPLY;
            extra = extra_buf;
        }
    }

    if (is_fresh_grid) *is_fresh_grid = fresh_grid;

    ESP_LOGI(TAG, "manual reply to %s: heard '%s' -> %s%s",
             heard->call, heard->last_text,
             extra ? extra : "(grid)", fresh_grid ? " [fresh]" : "");

    return ft8_tx_build_request(kind, heard->call, reply_freq_hz,
                                heard->last_utc, extra, out, err, err_len);
}

// --- DT-follow-partner (operator request 2026-07-19) -----------------------
// When a QSO partner transmits significantly off the band's timing (a weak,
// badly-clocked or deliberately-offset station), shift OUR TX to land on THEIR
// beat (ft8_tx_set_follow_offset_ms) so they copy us better than a UTC burst
// sitting at the edge of their decode window. The partner's raw slot DT minus
// the band consensus (ft8_get_last_timing_ms) gives their true offset from the
// band, and crucially cancels our common ~560 ms RX audio latency. Engaged/
// updated each time we hear them, reset to 0 the moment the QSO ends. Works both
// directions (we called them, or they answered our CQ). Threshold = operator's.
#define DT_FOLLOW_THRESHOLD_MS 200

static void clear_dt_follow(void)
{
    if (ft8_tx_get_follow_offset_ms() != 0) {
        ft8_tx_set_follow_offset_ms(0);
        ESP_LOGI(TAG, "DT-follow: back to UTC/GPS beat");
    }
}

static void update_dt_follow(const char *target)
{
    if (!target || !target[0]) return;
    int consensus;
    if (!ft8_get_last_timing_ms(&consensus)) return;   // no band reference yet

    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, target) != 0) continue;
        int excess = (int)snap[i].last_dt_ms - consensus;   // partner's offset from the band
        if (excess > DT_FOLLOW_THRESHOLD_MS || excess < -DT_FOLLOW_THRESHOLD_MS) {
            if (ft8_tx_get_follow_offset_ms() != excess) {
                ft8_tx_set_follow_offset_ms(excess);
                ft8_status_set("DT-follow %s: TX %+d ms", target, excess);
                ESP_LOGI(TAG, "DT-follow %s: partner_dt=%d consensus=%d -> TX %+d ms",
                         target, (int)snap[i].last_dt_ms, consensus, excess);
            }
        } else {
            clear_dt_follow();   // partner is on the band's beat -> normal
        }
        return;
    }
    // target not in this snapshot -> keep the current offset; re-hear updates it.
}

// Scan the ft8_screen table for a message FROM s_target TO s_my_call decoded
// in slot_sec. Fills one of report_buf / *got_rr73 / *got_73, and *snr_db_out
// with OUR locally-measured SNR of their signal (independent of any numeric
// report token in the text) - this is the pounce-side equivalent of the SNR
// cqrun_answer() already captures for CQ-run, used as RST_SENT.
static bool scan_for_response(int64_t slot_sec,
                              char *report_buf, size_t report_cap,
                              bool *got_rr73, bool *got_73, int *snr_db_out)
{
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, s_target) != 0) continue;
        if (snap[i].last_utc != slot_sec) continue;

        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue; // not addressed to us
        if (strcmp(tok2, s_target)  != 0) continue; // not from them

        if (snr_db_out) *snr_db_out = snap[i].last_snr_db;
        // Refresh the partner's own tone while we can actually see it. Their AF
        // normally holds for the whole exchange, but re-reading it each slot
        // costs nothing here (we already have the row) and tracks a partner who
        // does move. Plain int store - same unlocked-on-the-decode-task
        // convention as s_rst_sent below.
        if (snap[i].last_freq > 0) s_partner_freq_hz = (int)snap[i].last_freq;
        if (strcmp(rest, "RR73") == 0) { *got_rr73 = true; return true; }
        if (strcmp(rest, "73")   == 0) { *got_73   = true; return true; }
        if (rest[0] != '\0') {
            snprintf(report_buf, report_cap, "%s", rest);
            return true;
        }
    }
    return false;
}

// Look up a station's last-heard AF tone in the decode table. Returns 0 if the
// call isn't there (caller must treat that as "no hint", never as a frequency).
// The ~11 KB snapshot is heap-allocated in PSRAM, NOT a stack local: this is
// reached from ft8_qso_start() on the LVGL event-callback's ~8 KB task stack,
// where an 11 KB frame overflows at the prologue - the v0.20.1 pounce crash.
static int partner_tone_hz(const char *call)
{
    if (!call || !call[0]) return 0;
    ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) return 0;
    int n = 0, hz = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, call) == 0) {
            if (snap[i].last_freq > 0) hz = (int)snap[i].last_freq;
            break;
        }
    }
    heap_caps_free(snap);
    return hz;
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

bool ft8_filter_match(const char *text, const ft8_filters_t *f)
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
        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;  // not addressed to us
        if (strcmp(tok2, s_my_call) == 0) continue;  // avoid MYCALL MYCALL loops
        // Empty third field is ACCEPTED: a nonstandard-callsign answer
        // ("<MYCALL> PJ4/K1ABC", i3=4) has no room for a grid - it's still a
        // real answer to our CQ. Standard messages always carry a third
        // field, so this only ever admits the nonstd case. cqrun_answer()
        // doesn't need `rest` (it sends OUR measured report either way).
        if (!ft8_filter_match(snap[i].last_text, &qs.ft8_filters)) continue;
        // Worked-before: tok2 is the answering station's callsign. If we've
        // logged them already ON THIS BAND and the operator enabled the filter,
        // ignore their answer so the CQ keeps running for someone new (same rule
        // the robot applies to CQ callers). Band-aware: a new band is a new slot.
        if (qs.ft8_filters.excl_worked_before &&
            adif_log_contains_call_on_band(tok2, cat_get_frequency())) continue;
        // ...and never inside the grace window, checkbox or not: the duplicate
        // guard would refuse to LOG the result, so answering would key the radio
        // through a whole exchange and throw it away (Gyula HA3HZ, 2026-08-28).
        if (ft8_qso_worked_recently(tok2, cat_get_frequency())) continue;
        if (snap[i].last_snr_db > best_snr) {
            best_snr = snap[i].last_snr_db;
            best_idx = i;
        }
    }

    if (best_idx < 0) return false;

    char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
    split_msg3(snap[best_idx].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest));
    snprintf(caller_buf, caller_cap, "%s", tok2);
    if (caller_freq_out) *caller_freq_out = snap[best_idx].last_freq;
    if (caller_snr_out)  *caller_snr_out  = snap[best_idx].last_snr_db;
    if (strcmp(rest, "RR73") == 0) { *got_rr73 = true; return true; }
    if (strcmp(rest, "73")   == 0) { *got_73   = true; return true; }
    snprintf(report_buf, report_cap, "%s", rest);
    return true;
}

// ---------------------------------------------------------------------------
// Outgoing-message bookkeeping
// ---------------------------------------------------------------------------

// Make req the current outgoing message and move to st. Resets the miss
// counter (this is only ever called on progress / start). Arming itself is
// deferred to on_tx_complete() / the idle fallback.
// --- "they're working someone else" hold (Roy KI0ER, 2026-07-27) -----------
// On a busy band several stations answer the same CQ and the caller picks one.
// If that isn't us, we used to keep re-sending TX1 at them for QSO_TIMEOUT_SLOTS
// slots while they were visibly mid-exchange with a third party - six ~12.6 s
// full-power bursts nobody could answer - and then grey-list them, which is a
// false positive: the grey-list is for stations that can't HEAR us, not ones
// that are merely busy. So while their latest message is addressed to somebody
// else we hold: no transmission, and no missed-slot count (so no timeout and no
// grey-listing). When they sign off (73/RR73) or CQ again, we resume.
//
// Bounded on purpose. Holding forever would be its own bug - a station that
// simply vanishes mid-sentence must still time out - and an unbounded hold that
// kept re-transmitting would be WORSE for battery than timing out. 24 slots is
// ~6 min, comfortably longer than a full exchange plus slack.
#define QSO_BUSY_HOLD_MAX_SLOTS 24
static int  s_busy_holds;              // consecutive slots held; 0 = not holding
static char s_busy_with[FT8_CALL_MAX_LEN];  // who they're working, for the status line

// True if `call`'s most recent decoded message is addressed to a THIRD party.
// Conservative: anything we can't read as "busy with someone else" returns
// false, so an unparseable or absent message never stalls a QSO.
// PSRAM snapshot, never a stack local - this is reachable from the capture task
// via on_tx_complete() and from the decode task via advance(); heaping it keeps
// it safe if it ever gets called from a small stack too (the v0.20.1 crash).
static bool partner_busy_with(const char *call, char *with, size_t with_sz)
{
    if (!call || !call[0]) return false;
    ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) return false;
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    bool busy = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, call) != 0) continue;
        const char *text = snap[i].last_text;
        // A CQ means they're free and looking - engage, don't hold.
        if (strncmp(text, "CQ ", 3) == 0 || strcmp(text, "CQ") == 0) break;

        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(text, tok1, sizeof tok1, tok2, sizeof tok2, rest, sizeof rest))
            break;
        if (!tok1[0]) break;
        if (strcmp(tok1, s_my_call) == 0) break;   // addressed to US - engage
        // Signing off with a third party: the frequency is about to be free, so
        // don't hold on this - the next slot will show a CQ or a reply to us.
        if (strcmp(rest, "73") == 0 || strcmp(rest, "RR73") == 0) break;
        busy = true;
        if (with && with_sz) snprintf(with, with_sz, "%s", tok1);
        break;
    }
    heap_caps_free(snap);
    return busy;
}

static void set_current(const ft8_tx_request_t *req, ft8_qso_state_t st)
{
    lock();
    if (req) { s_cur_req = *req; s_have_cur = true; }
    s_state        = st;
    s_missed_slots = 0;
    s_busy_holds   = 0;    // progress - any hold is over
    s_busy_with[0] = '\0';
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
    int cq_sent = s_cq_calls_sent;
    unlock();

    if (!have || (!one_shot && !repeating)) return;

    // CQ auto-stop: past the limit, stop re-arming but stay in CQ state so
    // the RX slot after the final call is still scanned for an answer;
    // advance()'s no-answer path sees s_cq_exhausted and ends the session.
    // This is the single choke point for ALL CQ arming (on_tx_complete and
    // the arm_current_if_idle safety nets both come through here).
    // Somebody we just worked is still asking for our final: do not start
    // calling CQ over the top of him. advance() sets this once per RX slot and
    // clears it as soon as he stops (or the window closes), at which point the
    // CQ resumes through the normal arm_current_if_idle() safety net.
    if (st == FT8_QSO_CQ && s_final_hold) {
        ESP_LOGI(TAG, "holding CQ: %s is still asking for our final", s_final_call);
        return;
    }

    if (st == FT8_QSO_CQ) {
        qmx_settings_t qs;
        settings_load_all(&qs);
        if (qs.cq_max_calls > 0 && cq_sent >= qs.cq_max_calls) {
            lock(); s_cq_exhausted = true; unlock();
            ft8_status_set("CQ %d of %d sent - listening", cq_sent, qs.cq_max_calls);
            ESP_LOGI(TAG, "CQ auto-stop: %d of %d sent - not re-arming", cq_sent, qs.cq_max_calls);
            return;
        }
        // Listening slot (Roy KI0ER): while transmitting we are deaf to our own
        // time window, so its occupancy picture goes stale exactly where we need
        // it to be fresh. Every N calls, skip one transmission and listen.
        //
        // The guard matters: cq_calls_sent does NOT advance on a slot we skip,
        // so without remembering which count we already paused at, the run would
        // stop at N and never call again.
        if (qs.cq_listen_every > 0 && cq_sent > 0 &&
            (cq_sent % qs.cq_listen_every) == 0 && s_cq_listen_done_at != cq_sent) {
            lock(); s_cq_listen_done_at = cq_sent; unlock();
            ft8_status_set("listening (after %d CQ calls)", cq_sent);
            ESP_LOGI(TAG, "CQ listening slot after %d calls - skipping one transmission", cq_sent);
            return;
        }
    }

    // Don't key up at a partner who is visibly working somebody else - this is
    // where the wasted transmissions actually get saved (see the hold comment
    // above). Deliberately NOT applied to:
    //   - WAIT_DONE, so our closing 73 always goes out and the QSO completes;
    //   - CQ, where there's no specific station to be busy;
    //   - CQ-run sessions (from_cq), where the partner answered US - if they
    //     wander off to someone else that's a lost QSO which should time out
    //     normally, not something to wait through.
    {
        lock();
        bool pounce_wait = !s_from_cq && s_target[0] &&
                           (st == FT8_QSO_WAIT_RPT || st == FT8_QSO_WAIT_ROGER ||
                            st == FT8_QSO_WAIT_RR73);
        char tgt[FT8_CALL_MAX_LEN];
        snprintf(tgt, sizeof tgt, "%s", s_target);
        int holds = s_busy_holds;
        unlock();

        if (pounce_wait && holds > 0 && holds <= QSO_BUSY_HOLD_MAX_SLOTS) {
            ESP_LOGI(TAG, "holding TX: %s is working %s (%d/%d)",
                     tgt, s_busy_with[0] ? s_busy_with : "someone else",
                     holds, QSO_BUSY_HOLD_MAX_SLOTS);
            return;
        }
    }

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

// QSO progressed: the ARMED request (if any) still carries the PREVIOUS
// step's message - on_tx_complete() re-armed it right after our last burst.
// Replace it with the fresh s_cur_req now, so the slot loop's hold-for-decode
// gate (ft8_test.c) can fire the NEW message in this same slot instead of
// re-sending the stale one (the "everything goes twice" bug). ft8_tx_arm()
// overwrites an ARMED request but refuses an ACTIVE one, so if a burst is
// already on-air this degrades to the old deferred-arming behaviour
// (on_tx_complete() arms the fresh content after the burst).
static void arm_current_replacing_armed(void)
{
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_ACTIVE) rearm_current();
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

// Same as send_next(), but builds an ARRL Field Day (class+section) message
// instead of the grid/report-based standard encoding.
static bool send_next_fd(ft8_tx_kind_t kind, const char *target, int freq,
                         int64_t slot_sec, const char *class_section, ft8_qso_state_t st)
{
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request_fd(kind, target, freq, slot_sec, class_section,
                                 &req, err, sizeof(err))) {
        ESP_LOGE(TAG, "send_next_fd build failed (extra='%s'): %s", class_section, err);
        return false;
    }
    set_current(&req, st);
    return true;
}

// The partner just repeated their previous message - they haven't copied our
// report yet - but we re-heard them THIS slot with a fresh SNR. Like WSJT-X,
// bring the report we keep re-sending (and the RST_SENT we'll log) up to that
// newest measurement, so the value that finally gets through to them - and into
// both stations' logs - is our most current reading, not the stale first one.
// Only rebuilds/re-arms when the value actually changed, so an unchanged SNR
// costs nothing. as_roger => TX2's "R<rpt>" (pounce, WAIT_RR73); otherwise a
// bare "<rpt>" (CQ-run, WAIT_ROGER). Never called in Field Day mode - that
// exchange is a fixed class+section, not a signal report.
static void refresh_our_report(int fresh_snr, bool as_roger,
                               const char *target, int freq, int64_t slot_sec)
{
    char fresh_rpt[8];
    fmt_report(fresh_snr, fresh_rpt, sizeof(fresh_rpt));
    if (strcmp(fresh_rpt, s_rst_sent) == 0) return;   // unchanged - nothing to do

    char extra[16];
    if (as_roger) make_roger(fresh_rpt, extra, sizeof(extra));
    else          snprintf(extra, sizeof(extra), "%s", fresh_rpt);

    ft8_tx_kind_t kind = as_roger ? FT8_TX_KIND_ROGER_RPT : FT8_TX_KIND_REPLY;
    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request(kind, target, freq, slot_sec, extra,
                              &req, err, sizeof(err))) {
        ESP_LOGW(TAG, "report refresh build failed (extra='%s'): %s", extra, err);
        return;
    }
    // Adopt as the current outgoing message WITHOUT touching s_state or
    // s_missed_slots: this is a fresher report on the SAME step, not progress,
    // so the QSO timeout must keep counting (set_current() would zero it).
    lock();
    s_cur_req  = req;
    s_have_cur = true;
    unlock();

    strncpy(s_rst_sent, fresh_rpt, sizeof(s_rst_sent) - 1);
    s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
    ESP_LOGI(TAG, "report refreshed to %s (re-heard %s) - re-sending '%s'",
             fresh_rpt, target, req.display_text);
    arm_current_replacing_armed();
}

// The partner was not heard ADDRESSING US this slot, but we may well have heard
// them anyway - calling CQ again, or working somebody else - and that decode
// carries a newer measurement of their signal than the one baked into the
// message we are about to re-send.
//
// Operator, 2026-08-26: "answering a CQ'ing station sends the SNR report
// alright - but if the station does not reply immediately then I should call
// with a new fresh (and often different) SNR report. Now it stays on the
// original." Quite right: a report is a measurement, and re-sending a stale one
// every 15 seconds means the number that eventually gets through - and into both
// stations' logs - describes a signal from minutes ago.
//
// refresh_our_report() covers the case where they answer us and repeat
// themselves; this covers the case where they say nothing to us at all, which is
// the one that actually persists, because that is what a station working down a
// pileup looks like from here.
//
// ⛔ Only acts when our outgoing message ALREADY carries a report. A plain
// pounce TX1 is our GRID, and turning that into a report because we re-heard
// them would change which message we are sending rather than just its number.
// s_rst_sent is exactly that flag: set when we sent a report, empty otherwise.
static void refresh_report_from_heard(bool as_roger, const char *target,
                                      int freq, int64_t slot_sec)
{
    if (!s_rst_sent[0] || !target || !target[0]) return;
    ft8_call_t heard;
    if (!ft8_screen_find_call(target, &heard)) return;
    // Only a decode from THIS slot is news. An older row would re-assert the
    // number we already sent, and refresh_our_report() would drop it anyway -
    // this just says so where it is cheap.
    if (heard.last_utc != slot_sec) return;
    refresh_our_report(heard.last_snr_db, as_roger, target, freq, slot_sec);
}

// Another station has drifted onto our tone since we picked it
// (ft8_tx_is_clashing() true). Re-scan for the nearest still-clear 50 Hz slot
// and move there instead of just flagging "FREQ BUSY" and transmitting over
// whoever is legitimately there.
//
// Called from TWO places, and the distinction is the whole point (#201):
//
//   is_cq=true  - the CQ no-answer path, as before.
//   is_cq=false - a POUNCE that has not transmitted yet, i.e. TX1 only.
//
// ⚠ THE ORIGINAL REASON FOR "NEVER MID-EXCHANGE" IS FALSE, and it was mine to
// repeat. Roy KI0ER (2026-08-20): "The other station's software is not and
// should not be tracking my offset tone. It does not matter where in the
// waterfall I show up; their software is decoding and matching the TEXT of my
// message to their callsign." He is right - FT8 decoders work the whole
// passband and match on callsign, so each message in a QSO may ride a different
// tone. Operators move deliberately in WSJT-X to escape QRM or QSB mid-QSO.
//
// ⚠ But there IS a real constraint, from Gyula HA3HZ in the same thread: a
// partner who has NARROWED their receive bandwidth (MSHV does this to keep
// decode time inside the reply window) may simply not hear a station that
// jumped far away. So "move freely" is wrong too - what is safe is moving as
// LITTLE as possible, which is exactly what ft8_find_clear_tone_hz_near() does.
//
// The one hard rule both agree on: never change tone DURING a burst. Roy KI0ER hit exactly that hole - answering a CQ, TX HOLD off, fresh
// EVEN+ODD maps with green slots visible, and the reply still went out on an
// occupied offset showing "FREQ BUSY", because the tone was chosen once when
// the reply was armed and nothing could move it in the ~15 s before the burst.
//
// The caller owns the "have we transmitted yet" test (s_tx_sent), not this
// function - it is also reached from the CQ path, where the rule is different.
static void relocate_tone_if_clashing(bool is_cq)
{
    if (!ft8_tx_is_clashing()) return;

    // TX hold means the operator picked this slot deliberately and wants to stay
    // there (WSJT-X's "Hold Tx Freq"). The clash is still reported on the TX
    // status line - it just isn't acted on behind their back.
    if (ft8_tx_get_tone_hold()) return;

    // Never move a burst that is already on the air. disarm/re-arm cannot
    // recall a keyed transmission, and half-moving one would put the tone and
    // the armed request out of step.
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE) return;

    lock();
    int old_freq = s_freq_hz;
    unlock();

    int new_freq = ft8_find_clear_tone_hz_near(old_freq);
    if (new_freq == old_freq) return;   // band fully packed - nowhere clearer to go

    // #219: mid-exchange, the move must be SMALL and it must not repeat
    // forever. Before our first burst neither limit applies - nobody is
    // listening for us yet, so the best available slot is simply the best one.
    const bool mid_qso = (!is_cq && s_tx_sent);
    if (mid_qso) {
        int delta = new_freq - old_freq;
        if (delta < 0) delta = -delta;
        if (delta > QSO_MIDQSO_MOVE_MAX_HZ) {
            // Staying put and showing FREQ BUSY beats going somewhere the
            // partner may not be listening (Gyula HA3HZ).
            ESP_LOGI(TAG, "clash at %d Hz: nearest clear slot is %d Hz away - "
                          "too far to move mid-QSO, staying put", old_freq, delta);
            return;
        }
        if (s_midqso_moves >= QSO_MIDQSO_MOVE_LIMIT) {
            ESP_LOGI(TAG, "clash at %d Hz: already moved %d times this QSO - staying put",
                     old_freq, s_midqso_moves);
            return;
        }
        s_midqso_moves++;
    }

    ESP_LOGI(TAG, "%s tone %d Hz is busy - moving to %d Hz",
             is_cq ? "CQ" : mid_qso ? "mid-QSO" : "TX1", old_freq, new_freq);

    lock();
    s_freq_hz               = new_freq;
    s_cur_req.audio_freq_hz = new_freq;
    // CQ only: s_cq_saved is the CQ to resume after a QSO times out. A pounce
    // must not rewrite it, or a later resume-CQ would come back on a tone that
    // was picked for somebody else's exchange.
    if (is_cq) s_cq_saved.audio_freq_hz = new_freq;
    unlock();

    ft8_tx_disarm();   // cancel the stale-frequency ARMED request
    arm_current_if_idle();
    ft8_status_set("%s: moved off busy tone -> %d Hz", is_cq ? "CQ" : "TX", new_freq);
    ESP_LOGI(TAG, "%s tone clash at %d Hz - relocated to %d Hz",
             is_cq ? "CQ" : "TX", old_freq, new_freq);
}

// --- Broken-QSO resume (operator request 2026-07-16) -----------------------
// When an exchange is abandoned (timeout, or a manual abort mid-exchange) the
// essentials are kept for QSO_RESUME_WINDOW_SEC so a partner who faded and
// came back is CONTINUED where the exchange stopped - their R-report/RR73/73
// picked up mid-flow - instead of restarted from TX1. Two triggers:
// re-pouncing them by hand (ft8_qso_start), and automatically when a message
// addressed to us from them decodes while we're not busy (ft8_qso_advance).
// The auto path only ever re-engages a station the operator already chose to
// work, within the window - not a new unattended-TX category.
#define QSO_RESUME_WINDOW_SEC 300
static char             s_resume_call[FT8_CALL_MAX_LEN];
static ft8_qso_state_t  s_resume_state;
static ft8_tx_request_t s_resume_req;       // the message we were repeating
static bool             s_resume_have_req;
static char             s_resume_rst_sent[8], s_resume_rst_rcvd[8];
static char             s_resume_fd_exch[FT8_FD_EXCH_LEN];
static bool             s_resume_from_cq;
static int              s_resume_freq_hz;
static int              s_resume_partner_hz;
static int64_t          s_resume_at;        // time(NULL) at abandonment

// True while we're draining the pileup (auto-work-pileup): the current QSO was
// started by try_start_pileup_pounce(). Lets a timed-out pileup pounce advance
// to the NEXT waiting station instead of going sticky. Cleared whenever any
// non-pileup QSO starts (ft8_qso_start/_cq) or on a manual abort.
static bool             s_pileup_active;

// Snapshot the current exchange as resumable. Call under lock(), BEFORE the
// abandon path mutates state. Only exchange states are worth resuming.
static void resume_record_save_locked(void)
{
    if (!s_target[0] || !s_have_cur) return;
    if (s_state != FT8_QSO_WAIT_RPT && s_state != FT8_QSO_WAIT_ROGER &&
        s_state != FT8_QSO_WAIT_RR73) return;
    strncpy(s_resume_call, s_target, sizeof(s_resume_call) - 1);
    s_resume_call[sizeof(s_resume_call) - 1] = '\0';
    s_resume_state    = s_state;
    s_resume_req      = s_cur_req;
    s_resume_have_req = true;
    memcpy(s_resume_rst_sent, s_rst_sent, sizeof(s_resume_rst_sent));
    memcpy(s_resume_rst_rcvd, s_rst_rcvd, sizeof(s_resume_rst_rcvd));
    memcpy(s_resume_fd_exch,  s_fd_their_exch, sizeof(s_resume_fd_exch));
    s_resume_from_cq  = s_from_cq;
    s_resume_freq_hz  = s_freq_hz;
    s_resume_partner_hz = s_partner_freq_hz;
    s_resume_at       = (int64_t)time(NULL);
}

static bool resume_record_fresh(const char *call)
{
    if (!s_resume_call[0] || !s_resume_have_req || !call || !call[0]) return false;
    if (strcmp(s_resume_call, call) != 0) return false;
    return ((int64_t)time(NULL) - s_resume_at) <= QSO_RESUME_WINDOW_SEC;
}

// Restore the abandoned exchange and re-arm its last outgoing message; the
// normal advance() flow then processes whatever the partner sends next (an
// R-report/RR73 heard this very slot advances immediately). One-shot: the
// record is consumed. Caller ensures the machine is idle/interruptible.
static void resume_restore(void)
{
    lock();
    strncpy(s_target, s_resume_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_state        = s_resume_state;
    s_cur_req      = s_resume_req;
    s_have_cur     = true;
    memcpy(s_rst_sent, s_resume_rst_sent, sizeof(s_rst_sent));
    memcpy(s_rst_rcvd, s_resume_rst_rcvd, sizeof(s_rst_rcvd));
    memcpy(s_fd_their_exch, s_resume_fd_exch, sizeof(s_fd_their_exch));
    s_from_cq      = s_resume_from_cq;
    s_freq_hz      = s_resume_freq_hz;
    s_partner_freq_hz = s_resume_partner_hz;
    s_missed_slots = 0;
    s_min_scan_utc = 0;             // partner is audible NOW - scan immediately
    s_resume_call[0]  = '\0';
    s_resume_have_req = false;
    unlock();
}

// Did the abandoned partner transmit a message addressed to us THIS slot?
// (Runs on the decode task - the ~11 KB stack snapshot is fine there, same
// pattern as capture_pileup_callers.)
static bool resume_comeback_heard(int64_t slot_sec)
{
    if (!s_my_call[0]) return false;
    if (!resume_record_fresh(s_resume_call)) return false;
    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec) continue;
        char t1[16], t2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, t1, sizeof(t1), t2, sizeof(t2),
                        rest, sizeof(rest))) continue;
        if (strcmp(t1, s_my_call) != 0) continue;
        if (strcmp(t2, s_resume_call) != 0) continue;
        return true;
    }
    return false;
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

    // Remember the half-finished exchange so a comeback (manual re-pounce or
    // the auto-resume scan in advance()) continues it instead of restarting.
    lock();
    resume_record_save_locked();
    unlock();

    clear_dt_follow();   // QSO over - back to the UTC/GPS beat

    // Grey-listing (opt-in): a timed-out exchange counts as a failed attempt
    // at this station; after GREY_FAIL_LIMIT of them the auto pickers (robot,
    // auto-work pileup) stop re-calling it (Roy KI0ER: the robot re-tried the
    // same deaf station all night). Manual pounces stay possible after an
    // explicit "Clear from grey-list".
    // HOUND (rule 4): no strike against a Fox. Going unanswered for many slots is
    // the ordinary experience of a pileup - hundreds of hounds, one Fox, five
    // contacts a slot - and grey-listing the DXpedition you are trying to work
    // would be the exact opposite of what the operator wants.
    {
        qmx_settings_t gq;
        settings_load_all(&gq);
        if (gq.greylist_en && tgt[0] && !s_hound_active) ft8_greylist_note_timeout(tgt);
    }

    if (from_cq && s_have_cq_saved) {
        // Drop the half-finished QSO and go back to calling CQ on the frequency.
        lock();
        s_cur_req      = s_cq_saved;
        s_have_cur     = true;
        s_state        = FT8_QSO_CQ;
        s_target[0]    = '\0';
        s_missed_slots = 0;
        s_cq_calls_sent = 0; s_cq_listen_done_at = -1;   // resumed CQ = fresh sequence for the auto-stop count
        s_cq_exhausted  = false;
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
    s_partner_freq_hz = 0;
    s_busy_holds    = 0;
    s_busy_with[0]  = '\0';
    s_rst_sent[0]   = '\0';
    s_rst_rcvd[0]   = '\0';
    s_fd_their_exch[0] = '\0';
}

// Whether the CURRENT QSO was started by the auto-answer robot rather than a
// human tap. The busy-station hold treats the two differently (Roy KI0ER,
// 2026-08-07): a deliberate pounce means the operator wants THAT station and
// will happily wait out their other QSO - but the robot picked its target off a
// list, so when that target turns out to be mid-QSO with someone else, waiting
// is pure loss: move on and let the robot pick another CQ caller.
static bool s_robot_started = false;

void ft8_qso_mark_robot_started(void)
{
    s_robot_started = true;
}

bool ft8_qso_start(const ft8_tx_request_t *tx1_req, char *err, size_t err_len)
{
    s_tx_sent = false;   // #201: a fresh exchange has aired nothing yet
    s_midqso_moves = 0;  // #219
    if (!tx1_req || !tx1_req->target_call[0]) {
        if (err) snprintf(err, err_len, "No target callsign");
        return false;
    }
    s_robot_started = false;   // set again by the robot AFTER a successful start
    // Any explicitly-started QSO (manual tap, robot) ends pileup-drain tracking;
    // try_start_pileup_pounce() re-sets it right after this returns for the
    // pileup case. Scopes the timed-out-pileup auto-advance to pileup pounces
    // only, so a manual pounce timeout still goes sticky for the operator.
    s_pileup_active = false;
    clear_dt_follow();   // fresh QSO - engages again when we hear this partner

    // Refuse to clobber an exchange already in progress (CQ loop or any WAIT_*
    // state) - a tap on a different row used to silently overwrite s_target /
    // s_cur_req out from under the active exchange, leaving a stale message
    // (e.g. their grid re-armed with a bogus "R" prefix mid-exchange). The
    // operator must finish or ft8_qso_abort() the current one first.
    char busy_target[FT8_CALL_MAX_LEN];
    if (ft8_qso_is_busy(busy_target, sizeof(busy_target))) {
        if (err) {
            if (busy_target[0]) snprintf(err, err_len, "Busy: working %s", busy_target);
            else                snprintf(err, err_len, "Busy: calling CQ");
        }
        return false;
    }

    if (!load_my_call(err, err_len)) return false;

    // RESUME: re-pouncing a partner we abandoned within the last few minutes
    // continues the exchange where it stopped (re-arms the last message we
    // were sending; their next R-report/RR73/73 advances normally) instead of
    // restarting from TX1 - "just finish QSOs if they get broken up"
    // (operator request 2026-07-16).
    if (resume_record_fresh(tx1_req->target_call)) {
        char who[FT8_CALL_MAX_LEN];
        strncpy(who, s_resume_call, sizeof(who) - 1);
        who[sizeof(who) - 1] = '\0';
        resume_restore();
        ft8_pileup_remove(who);
        arm_current_if_idle();
        ft8_status_set("QSO %s: resuming where we left off", who);
        ESP_LOGI(TAG, "resume (manual): %s - continuing exchange", who);
        return true;
    }

    // "Skip TX1" (settings drawer toggle): instead of exchanging grids first,
    // jump straight to sending a signal report - the same message shape/state
    // cqrun_answer() already uses for its own first reply (FT8_TX_KIND_REPLY
    // with extra=report, landing in WAIT_ROGER). Their SNR/last-heard-slot come
    // from the same ft8_screen snapshot scan_for_response() uses; if they've
    // since aged out of the decode table (race with a stale row tap), fall
    // back to the normal grid-based TX1 rather than risk a wrong-parity or
    // bogus-report first message.
    qmx_settings_t qs;
    settings_load_all(&qs);

    ft8_tx_request_t req_to_arm  = *tx1_req;
    ft8_qso_state_t  start_state = FT8_QSO_WAIT_RPT;
    char             first_rpt[8] = "";   // filled from a real measurement below before any use
    bool             skip_applied = false;

    // A REPLY whose third field is already a signal report ("+NN"/"-NN" in
    // extra_field) was deliberately built report-first — the pileup modal
    // does this, because a pileup entry called US (we're the CQ-side
    // station, so grid TX1 would be the wrong role) and has usually aged
    // out of the live decode table, meaning the skip_tx1 scan below would
    // miss and silently fall back to grid (Ken KF0AYY field report,
    // 2026-07-15). Honour it as-is, regardless of the skip_tx1 toggle:
    // arm unchanged and start in WAIT_ROGER, exactly like cqrun_answer()'s
    // first reply. Every other builder passes extra=NULL for REPLY (grid),
    // so this can't misfire on a decode-list or robot pounce.
    int partner_hz = 0;   // their AF tone, for the decode-priority hint

    // Is the target a Fox? Decided here, before the Skip-TX1 branch below, because
    // it has to VETO it. Fox/Hound's exchange is fixed - grid, their report, our
    // R-report ON THEIR FREQUENCY, their RR73 - and Skip TX1 replaces the opening
    // grid with a report, which puts the contact on the CQ-run ladder instead: no
    // QSY step exists there, so our closing message goes out from the calling tone
    // where the Fox is not listening.
    //
    // Not theoretical. On the bench (2026-08-10) a hound contact with the phantom
    // Fox ran to completion and logged, entirely from 1350 Hz, having never
    // QSY'd - a green result that was not F/H at all. Skip TX1 was on, which many
    // operators will have.
    int  fox_probe_hz = partner_tone_hz(tx1_req->target_call);
    bool hound = ft8_hound_enabled(ft8_hound_mode()) &&
                 fox_probe_hz > 0 && fox_probe_hz < FT8_HOUND_FOX_MAX_HZ;

    size_t pre_rpt_len = strnlen(tx1_req->extra_field, sizeof(tx1_req->extra_field));
    if (tx1_req->kind == FT8_TX_KIND_REPLY &&
        (tx1_req->extra_field[0] == '+' || tx1_req->extra_field[0] == '-') &&
        pre_rpt_len < sizeof(first_rpt)) {
        memcpy(first_rpt, tx1_req->extra_field, pre_rpt_len);
        first_rpt[pre_rpt_len] = '\0';
        start_state  = FT8_QSO_WAIT_ROGER;
        skip_applied = true;
    } else if (tx1_req->kind == FT8_TX_KIND_ROGER_RPT &&
               tx1_req->extra_field[0] == 'R' &&
               (tx1_req->extra_field[1] == '+' || tx1_req->extra_field[1] == '-') &&
               pre_rpt_len - 1 < sizeof(first_rpt)) {
        // #234 (Roy KI0ER, working K7FD): a pre-built ROGER-REPORT request -
        // ft8_qso_build_manual_reply()/the auto-pileup drain skip straight to
        // TX2 because the partner's OWN last message already reported us, so
        // there is nothing left to wait for at TX1 - fell all the way through
        // to the unconditional default below, WAIT_RPT. WAIT_RPT is "waiting
        // for THEIR FIRST report"; it has no branch for "they already
        // rogered us", only an explicit got_rr73/got_73 skip-ahead. So the
        // very next decode from K7FD - a bare "RRR", carrying no report at
        // all - fell into WAIT_RPT's default case, which re-measures OUR
        // current SNR of them and re-sends ANOTHER R-report, forever. WSJT-X
        // sends 73 after an RRR; ours just kept saying R-<latest SNR> every
        // slot for 10+ minutes, and the QSO never logged.
        //
        // The correct next state, same as the REPLY/WAIT_ROGER branch above,
        // is the one that already expects RR73/RRR/73 and already replies
        // with "73" - WAIT_RR73's handler has carried this exact fix for a
        // BARE "RRR" since 2026-07-27 (Roy KI0ER, NH6L). Skip the leading
        // 'R' so first_rpt matches the plain numeric form ("-20", not
        // "R-20") that ADIF's RST_SENT and every other caller of first_rpt
        // already expect.
        memcpy(first_rpt, tx1_req->extra_field + 1, pre_rpt_len - 1);
        first_rpt[pre_rpt_len - 1] = '\0';
        start_state  = FT8_QSO_WAIT_RR73;
        skip_applied = true;
    } else if (qs.ft8_filters.skip_tx1 && hound) {
        // Skip TX1 vetoed - see the note above. Say so, or this looks like the
        // toggle being ignored at random.
        ESP_LOGI(TAG, "Skip TX1 ignored for %s: a Fox needs the standard "
                      "grid -> report -> R-report exchange", tx1_req->target_call);
    } else if (qs.ft8_filters.skip_tx1) {
        // The decode-table snapshot is ~11 KB (FT8_CALL_TABLE_SIZE * sizeof(
        // ft8_call_t)) and MUST NOT be a stack local here: ft8_qso_start() runs
        // on the LVGL event-callback's small (~8 KB) task stack when a pounce is
        // confirmed (pounce_btn_cb -> ft8_qso_start), and an ~11 KB stack frame
        // overflows it at the prologue -> "Stack protection fault" crash on
        // EVERY pounce, whether or not skip_tx1 is set (the frame is reserved
        // unconditionally). Heap it in PSRAM instead. (Same rule as ft8_tx.c's
        // ft8_find_clear_tone snapshot - see CLAUDE.md "Audit every malloc under
        // ~16 KB ... it's silently going to internal RAM".)
        ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (snap) {
            int n = 0;
            ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
            int     snr = 0;
            int64_t their_last_utc = 0;
            bool    found_target = false;
            for (int i = 0; i < n; i++) {
                if (strcmp(snap[i].call, tx1_req->target_call) == 0) {
                    snr            = snap[i].last_snr_db;
                    their_last_utc = snap[i].last_utc;
                    // Free while we're already holding the row - saves the
                    // partner_tone_hz() fallback scan below.
                    if (snap[i].last_freq > 0) partner_hz = (int)snap[i].last_freq;
                    found_target   = true;
                    break;
                }
            }
            heap_caps_free(snap);
            if (found_target) {
                fmt_report(snr, first_rpt, sizeof(first_rpt));
                char build_err[64];
                if (ft8_tx_build_request(FT8_TX_KIND_REPLY, tx1_req->target_call,
                                         tx1_req->audio_freq_hz, their_last_utc,
                                         first_rpt, &req_to_arm, build_err, sizeof(build_err))) {
                    start_state  = FT8_QSO_WAIT_ROGER;
                    skip_applied = true;
                } else {
                    ESP_LOGW(TAG, "skip_tx1: build failed (%s) - falling back to grid TX1", build_err);
                }
            } else {
                ESP_LOGW(TAG, "skip_tx1: %s not in decode table - falling back to grid TX1",
                         tx1_req->target_call);
            }
        } else {
            ESP_LOGW(TAG, "skip_tx1: snapshot alloc failed - falling back to grid TX1");
        }
    }

    // Paths that took no snapshot above (plain grid TX1, or a pre-set report)
    // still need the partner's tone for the decode-priority hint.
    if (partner_hz <= 0) partner_hz = partner_tone_hz(tx1_req->target_call);

    char arm_err[64];
    if (!ft8_tx_arm(&req_to_arm, arm_err, sizeof(arm_err))) {
        if (err) snprintf(err, err_len, "%s", arm_err);
        return false;
    }

    // Earliest valid RX slot to scan: one slot after our first message fires.
    // Computed in ms internally (next_slot_sec) using the armed request's own
    // protocol period, so this lands on a boundary the engine will actually
    // produce - see that function's comment for the FT4 bug this fixes. Also
    // gates the skip-TX1 WAIT_ROGER path the same way WAIT_RPT is gated below
    // (see the ft8_qso_advance() early-return) - without it, a decode cycle
    // landing between arming and the first burst actually firing would count
    // as a missed roger before we've transmitted a single message.
    int64_t tx1_slot = next_slot_sec(req_to_arm.use_parity, req_to_arm.want_even_slot,
                                     req_to_arm.protocol);

    // `hound` was decided above (it had to veto Skip TX1). Re-check against the
    // tone we finally settled on, in case partner_hz came from somewhere other
    // than the probe - the flag must agree with the frequency we will QSY onto.
    if (hound && !(partner_hz > 0 && partner_hz < FT8_HOUND_FOX_MAX_HZ)) {
        ESP_LOGW(TAG, "hound: %s no longer looks like a Fox (tone %d Hz) - "
                      "running an ordinary exchange", tx1_req->target_call, partner_hz);
        hound = false;
    }

    lock();
    s_state         = start_state;
    strncpy(s_target, tx1_req->target_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_freq_hz       = req_to_arm.audio_freq_hz;
    s_partner_freq_hz = partner_hz;
    s_hound_active  = hound;
    s_hound_tone_hz = hound ? req_to_arm.audio_freq_hz : 0;
    s_min_scan_utc  = tx1_slot + 15;
    s_missed_slots  = 0;
    s_busy_holds    = 0;    // a fresh pounce is never mid-hold
    s_busy_with[0]  = '\0';
    s_from_cq       = false;
    s_cur_req       = req_to_arm;   // re-send each cycle until they reply
    s_have_cur      = true;
    s_have_cq_saved = false;
    if (skip_applied) {
        // We sent them a numeric report directly. RST_RCVD stays EMPTY until
        // their roger "R<rpt>" arrives carrying their own measurement of us
        // (not an echo of ours) - see the WAIT_ROGER handler. Empty means
        // "never received", and adif_log.c omits the field rather than
        // inventing a value.
        strncpy(s_rst_sent, first_rpt, sizeof(s_rst_sent) - 1);
        s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
        /* If we got here because they had ALREADY reported us (the #234
         * R-report entry), that is their measurement of us and belongs in
         * the log NOW: WAIT_RPT never runs to collect it, and a partner who
         * jumps straight to RR73 skips WAIT_ROGER too (#292). Still empty for
         * a genuine skip-TX1 start, where they have told us nothing yet. */
        if (start_state == FT8_QSO_WAIT_RR73 && s_heard_their_rpt[0] &&
            strcmp(s_heard_their_rpt_call, tx1_req->target_call) == 0) {
            strncpy(s_rst_rcvd, s_heard_their_rpt, sizeof(s_rst_rcvd) - 1);
            s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
        } else {
            s_rst_rcvd[0] = '\0';
        }
    } else {
        // Normal pounce: we receive their report (RST_RCVD); we never give our
        // own numeric report in TX1/TX2, so RST_SENT stays empty and is omitted.
        s_rst_sent[0] = '\0';
        s_rst_rcvd[0] = '\0';
    }
    s_fd_their_exch[0] = '\0';
    unlock();

    // We're committing to working them now - take them out of the pileup
    // list (harmless no-op if they weren't in it, e.g. a direct tap on a
    // still-live decode row rather than the pileup list itself).
    ft8_pileup_remove(tx1_req->target_call);

    // Both tones are logged deliberately: ours (where we transmit) and theirs
    // (where we listen, and the decode-priority hint). They are DIFFERENT by
    // design - conflating them was the 2026-07-28 priority-hint bug - so a log
    // showing only one number cannot tell you whether the hint is sane.
    if (skip_applied && start_state == FT8_QSO_WAIT_RR73) {
        // #234: the ROGER_RPT-triggered start above - we've already been
        // given their report, so what we sent is R<report>, and what we're
        // waiting for is RR73/RRR/73, not "a roger". Distinct wording so a
        // future log-reader isn't misled the same way this bug's own log
        // line ("started QSO (pounce)") gave no hint anything was off.
        ft8_status_set("QSO %s: sent R%s - waiting for RR73", tx1_req->target_call, first_rpt);
        ESP_LOGI(TAG, "started QSO (pounce, R-report): %s our_tone=%d Hz their_tone=%d Hz report=R%s, min_scan=%lld",
                 tx1_req->target_call, req_to_arm.audio_freq_hz, partner_hz, first_rpt,
                 (long long)(tx1_slot + 15));
    } else if (skip_applied) {
        ft8_status_set("QSO %s: sent report %s - waiting for roger", tx1_req->target_call, first_rpt);
        ESP_LOGI(TAG, "started QSO (pounce, skip-TX1): %s our_tone=%d Hz their_tone=%d Hz report=%s, min_scan=%lld",
                 tx1_req->target_call, req_to_arm.audio_freq_hz, partner_hz, first_rpt,
                 (long long)(tx1_slot + 15));
    } else {
        ft8_status_set("QSO %s: TX1 sent - waiting for report", tx1_req->target_call);
        ESP_LOGI(TAG, "started QSO (pounce): %s our_tone=%d Hz their_tone=%d Hz, min_scan=%lld",
                 tx1_req->target_call, req_to_arm.audio_freq_hz, partner_hz,
                 (long long)(tx1_slot + 15));
    }
    if (partner_hz <= 0)
        ESP_LOGW(TAG, "%s has no tone in the decode table - decode-priority hint disabled for this QSO",
                 tx1_req->target_call);
    return true;
}

bool ft8_qso_start_cq(const ft8_tx_request_t *cq_req, char *err, size_t err_len)
{
    s_tx_sent = false;   // #201: a fresh exchange has aired nothing yet
    s_midqso_moves = 0;  // #219
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
        int64_t next_slot = next_slot_sec(false, false, req_copy.protocol);
        req_copy.use_parity     = true;
        req_copy.want_even_slot = slot_is_even(next_slot, req_copy.protocol);
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
    s_partner_freq_hz = 0;     // no partner yet; don't leak a previous pounce's tone
    s_hound_active  = false;   // calling CQ is never a hound flow
    s_cur_req       = req_copy;
    s_have_cur      = true;
    s_cq_saved      = req_copy;
    s_have_cq_saved = true;
    s_from_cq       = true;
    s_missed_slots  = 0;
    s_cq_calls_sent = 0; s_cq_listen_done_at = -1;       // fresh CQ sequence - auto-stop count restarts
    s_cq_exhausted  = false;
    s_pileup_active = false;   // starting CQ is not part of a pileup drain
    clear_dt_follow();         // no partner yet - transmit CQ on the UTC/GPS beat
    // CQ-run: RST_SENT is set in cqrun_answer, RST_RCVD when their report
    // answer (cqrun_answer) or numeric roger (WAIT_ROGER) arrives carrying
    // their actual measurement of us. Both start empty = "not exchanged".
    s_rst_sent[0] = '\0';
    s_rst_rcvd[0] = '\0';
    s_fd_their_exch[0] = '\0';
    unlock();

    ft8_status_set("CQ: calling - listening for answers");
    ESP_LOGI(TAG, "CQ loop started @ %d Hz", cq_req->audio_freq_hz);
    return true;
}

// Build + adopt our reply to a station that answered our CQ (CQ-run). They
// sent their grid (or a report); we answer with a signal report and wait for
// their roger. If they jumped straight to RR73/73, we just send 73.
static void cqrun_answer(const char *caller, int caller_freq, int caller_snr,
                         const char *report, int64_t slot_sec,
                         bool got_rr73, bool got_73)
{
    // Stay on our own CQ tone for the entire exchange — the answering station
    // uses their own separate tone; switching to caller_freq would put us on
    // their (occupied) frequency and trigger a false clash warning.
    lock();
    strncpy(s_target, caller, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    int our_freq = s_freq_hz;   // already set to our CQ tone in ft8_qso_start_cq()
    unlock();

    // A station just answered our CQ - if they're off the band's beat, follow it.
    update_dt_follow(caller);

    // If their answer carried a numeric report of us instead of a grid
    // (OS4K opened with 'OZ1LAV OS4K -06'), that's RST_RCVD - capture it now
    // so it's logged even if they later jump straight to RR73.
    if (report && (report[0] == '+' || report[0] == '-') &&
        report[1] >= '0' && report[1] <= '9') {
        strncpy(s_rst_rcvd, report, sizeof(s_rst_rcvd) - 1);
        s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
    }

    // We're committing to working them now - take them out of the pileup
    // list (harmless no-op if they were never in it, e.g. an unfiltered
    // caller who answered on the very first slot of the CQ).
    ft8_pileup_remove(caller);

    qmx_settings_t qs;
    settings_load_all(&qs);
    bool fd_mode = qs.field_day_en && qs.fd_class[0] && qs.fd_section[0];

    bool ok;
    char rpt[8];
    fmt_report(caller_snr, rpt, sizeof(rpt));

    if (got_rr73 || got_73) {
        ok = send_next(FT8_TX_KIND_73, caller, our_freq, slot_sec, "73",
                       FT8_QSO_WAIT_DONE);
        if (ok) {
            strncpy(s_rst_sent, rpt, sizeof(s_rst_sent) - 1);
            s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
            ft8_status_set("QSO %s: sending 73 (their SNR %s)", caller, rpt);
        }
    } else if (fd_mode) {
        // Field Day: our first reply carries our own class+section (no "R" -
        // this is the first time we're sending it), not a numeric report.
        char exch[FT8_FD_EXCH_LEN];
        snprintf(exch, sizeof(exch), "%s %s", qs.fd_class, qs.fd_section);
        ok = send_next_fd(FT8_TX_KIND_REPLY, caller, our_freq, slot_sec, exch,
                          FT8_QSO_WAIT_ROGER);
        if (ok) ft8_status_set("QSO %s: answered - sending %s", caller, exch);
    } else if (report && (report[0] == '+' || report[0] == '-') &&
               report[1] >= '0' && report[1] <= '9') {
        // ⭐ THEY SKIPPED THE GRID AND REPORTED US STRAIGHT AWAY, so the ladder
        // is one rung further on than the plain case below: answer with
        // R<our report>, which both acknowledges theirs and gives ours, and wait
        // for RR73 rather than for a roger we have already been sent.
        //
        // Gyula HA3HZ: "When the partner replies to a CQ, he does not send a
        // Grid, but a report (because he is already familiar), then my reply is
        // also a report and not an acknowledgement of the report. This happened
        // twice." Sending a bare report back reads as ignoring what they said,
        // and costs a whole cycle.
        //
        // Note the block above ALREADY recognised this case to capture RST_RCVD -
        // the information was there and the reply simply did not use it. And
        // ft8_qso_build_manual_reply() has always got this right, so the manual
        // Transmit behaved correctly while the automatic CQ-run did not: the same
        // manual-right/automatic-wrong split as the pileup grid bug (#134).
        char roger[16];
        make_roger(rpt, roger, sizeof(roger));
        ok = send_next(FT8_TX_KIND_ROGER_RPT, caller, our_freq, slot_sec, roger,
                       FT8_QSO_WAIT_RR73);
        if (ok) {
            strncpy(s_rst_sent, rpt, sizeof(s_rst_sent) - 1);
            s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
            ft8_status_set("QSO %s: they reported %s - sending %s", caller,
                           report, roger);
        }
    } else {
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
        s_cq_calls_sent = 0; s_cq_listen_done_at = -1;   // fresh sequence for the auto-stop count
        s_cq_exhausted  = false;
        unlock();
    }
    arm_current_if_idle();
}

// Passive pileup capture: every station that addressed a message to us this
// slot, other than whoever we're actively working right now, gets upserted
// into the pileup list (ft8_pileup.c). Runs unconditionally regardless of
// QSO state - a caller answering our CQ while we're already mid-exchange
// with someone else is exactly the "gave up waiting" scenario the pileup
// list exists for, and they'd otherwise never get tracked (advance()'s
// per-state scans only ever look for messages FROM the current target). No
// TX ever results from this - purely a background list update.
static void capture_pileup_callers(int64_t slot_sec)
{
    if (!s_my_call[0]) return;   // nothing to scan for until identity is loaded

    char cur_target[FT8_CALL_MAX_LEN];
    lock();
    strncpy(cur_target, s_target, sizeof(cur_target) - 1);
    cur_target[sizeof(cur_target) - 1] = '\0';
    unlock();

    qmx_settings_t qs_cap;
    settings_load_all(&qs_cap);

    ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    for (int i = 0; i < n; i++) {
        if (snap[i].last_utc != slot_sec) continue;
        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2), rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;              // not addressed to us
        if (strcmp(tok2, s_my_call) == 0) continue;              // avoid MYCALL MYCALL loops
        if (cur_target[0] && strcmp(tok2, cur_target) == 0) continue;  // already our active partner
        // Manually-worked partner (step-by-step Transmit, no machine QSO):
        // their exchange messages address us every cycle, but they're being
        // worked, not waiting - don't list them in the pileup. Same for the
        // station we JUST completed with: their trailing 73/RR73 keeps
        // addressing us after completion (Dirk DK7CVD's lingering-call case).
        int64_t now_s = time(NULL);
        lock();
        bool exempt = (s_manual_target[0] &&
                       strcmp(tok2, s_manual_target) == 0 &&
                       (now_s - s_manual_target_ts) < MANUAL_TARGET_TTL_S) ||
                      (s_last_done_call[0] &&
                       strcmp(tok2, s_last_done_call) == 0 &&
                       (now_s - s_last_done_ts) < DONE_GRACE_TTL_S);
        unlock();
        if (exempt) continue;
        // Worked-before is only a pileup exclusion when the operator has
        // asked for it ("Exclude worked before") - matching what the CQ-run
        // answer picker does. The old unconditional skip silently hid
        // legitimate dupe callers from the pileup while the machine was
        // perfectly willing to WORK them (filter off) - inconsistent, and in
        // sim practice sessions it emptied the pileup almost entirely.
        if (qs_cap.ft8_filters.excl_worked_before &&
            adif_log_contains_call_on_band(tok2, cat_get_frequency())) continue;
        ft8_pileup_note_caller(tok2, snap[i].last_snr_db, snap[i].last_freq, slot_sec);
    }
}

// Auto-work-pileup: we've just finished (or timed out of) a QSO and the
// operator enabled "Auto-work pileup". Pick the strongest waiting caller from
// the pileup list and pounce them, exactly the way the robot answers a CQ - the
// TX1 is built the same, and ft8_qso_start() then applies Skip-TX1, the resume
// window, the busy-guard and identity checks centrally, so this inherits all of
// them for free. Returns true if a pounce was started (state now WAIT_*), false
// if disabled / nobody eligible / build refused (caller falls back to CQ/idle).
// Strongest-SNR first: best chance of a clean completion, standard pileup order.
static bool try_start_pileup_pounce(void)
{
    qmx_settings_t qs;
    settings_load_all(&qs);
    if (!qs.ft8_filters.auto_pileup)            return false;
    // Manual pick wins over auto-work-pileup. Both decide WHO to work next, and
    // an operator who asked to choose has not asked to have the strongest caller
    // chosen for them a moment later - leaving both live would quietly defeat the
    // setting they just turned on.
    if (qs.ft8_filters.cq_manual_pick)          return false;
    if (!qs.my_callsign[0] || !qs.my_grid[0])   return false;

    ft8_pileup_entry_t pile[FT8_PILEUP_MAX];
    int n = ft8_pileup_get_all(pile, FT8_PILEUP_MAX);
    if (n <= 0) return false;

    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!pile[i].call[0]) continue;
        // Skip anyone already worked on this band if that filter is on - same
        // rule the robot enforces, so auto-work never re-calls a logged station.
        if (qs.ft8_filters.excl_worked_before &&
            adif_log_contains_call_on_band(pile[i].call, cat_get_frequency())) continue;
        // ...and never inside the grace window, checkbox or not. Deliberately
        // NOT applied to capture_pileup_callers() above: a recently-worked
        // caller stays VISIBLE in the pileup so the operator can still pick
        // them by hand - this only stops the machine doing it unattended.
        if (ft8_qso_worked_recently(pile[i].call, cat_get_frequency())) continue;
        // Grey-listed (repeated failed pounces) - don't auto-drain into them.
        if (qs.greylist_en && ft8_greylist_contains(pile[i].call)) continue;
        if (best < 0 || pile[i].snr_db > pile[best].snr_db) best = i;
    }

    /* ⛔ AND NOT WHILE THEY ARE MID-QSO WITH SOMEBODY ELSE (Gyula HA3HZ,
     * 2026-08-30: "I did not call and did not mark YL3PK before, but the
     * software called him").
     *
     * He HAD been called - 'HA3HZ YL3PK KO27', legitimately putting YL3PK in the
     * pileup - but the drain reached him 129 s later, by which time he was
     * working ER1SKI/P. We then called him five times over.
     *
     * The information was right there and was used for the wrong thing: the
     * drain reads their last message to CHOOSE THE MESSAGE FORMAT, logging
     * "last heard '...' is not to us - report-first", and never asks whether that
     * means it should call at all. partner_busy_with() already existed and was
     * consulted once a QSO was RUNNING; it was never consulted when picking.
     *
     * ⚠ CHECKED ON THE WINNER, NOT INSIDE THE SELECTION LOOP. partner_busy_with()
     * allocates ~11 KB of PSRAM and snapshots the whole call table under the
     * screen mutex; calling it for every one of up to FT8_PILEUP_MAX entries
     * would do that twelve times in a burst on the decode task, which is the
     * shape CLAUDE.md warns about for the cyan flash. Exclude-and-repick costs
     * ONE call in the ordinary case and never more than it would have.
     *
     * The deeper cause is a premise that stopped being true: ft8_pileup.h has no
     * expiry and justifies it as "nothing in this module ever arms a TX", which
     * was sound while a human chose from the list and could see they were busy.
     * Auto-work-pileup was built on top later without revisiting that. */
    while (best >= 0) {
        char busy_with[FT8_CALL_MAX_LEN] = {0};
        if (!partner_busy_with(pile[best].call, busy_with, sizeof busy_with)) break;
        ESP_LOGI(TAG, "auto-pileup: skipping %s - busy with %s",
                 pile[best].call, busy_with);
        pile[best].call[0] = '\0';          /* take them out of THIS pass only */
        best = -1;
        for (int i = 0; i < n; i++) {
            if (!pile[i].call[0]) continue;
            if (qs.ft8_filters.excl_worked_before &&
                adif_log_contains_call_on_band(pile[i].call, cat_get_frequency())) continue;
            if (ft8_qso_worked_recently(pile[i].call, cat_get_frequency())) continue;
            if (qs.greylist_en && ft8_greylist_contains(pile[i].call)) continue;
            if (best < 0 || pile[i].snr_db > pile[best].snr_db) best = i;
        }
    }
    if (best < 0) return false;

    // Reply on a clear tone (not their own), parity derived from the slot we last
    // heard them call us in (parity is periodic, so a several-minute-old
    // last_seen still gives the correct TX parity).
    int reply_freq_hz = ft8_tx_pick_tone_hz();
    ft8_tx_request_t req;
    char err[64];
    // REPORT-FIRST, not grid. Everyone in the pileup CALLED US - they already
    // have our grid from the CQ that put them there - so they are waiting for a
    // signal report, and sending "<them> <me> <grid>" makes their software give
    // up on the contact. Roy KI0ER lost QSOs to exactly this: the first caller
    // worked fine, the second (picked up from the pileup after that QSO logged)
    // was answered with a grid.
    //
    // Passing the report as `extra` is the whole fix: ft8_qso_start() already
    // detects a REPLY whose third field is a "+NN"/"-NN" token, arms it
    // unchanged and starts in WAIT_ROGER - the same shape and state
    // cqrun_answer() uses. That path was added for the pileup MODAL (Ken
    // KF0AYY, 2026-07-15) and this automatic drain was never moved onto it.
    // ⭐ BUILD THE REPLY FROM WHAT THEY ACTUALLY SENT, through the one builder
    // that has been right every time (#172).
    //
    // This used to always send a BARE report and start in WAIT_ROGER. That is
    // correct for a caller who sent a grid, and wrong for the experienced
    // operator who reported US instead - he needs R<report> and WAIT_RR73. It
    // could not be a one-line branch either, because ft8_pileup_entry_t records
    // call/snr/freq/last_seen and NOT what they sent, so the drain had no way to
    // tell the two apart.
    //
    // So look them up in the heard table and hand it to
    // ft8_qso_build_manual_reply(), which derives the whole ladder from
    // last_text. This removes the SECOND message-building path rather than
    // patching it a third time - the same shape had already been fixed in the
    // pileup modal (#134, sent a grid) and in cqrun_answer() (#167, sent a bare
    // report), and each fix left the others standing. One path cannot disagree
    // with itself.
    //
    // Safe from the decode task: that function touches no LVGL objects (its
    // header note describes its original caller, not a hard constraint) and its
    // one qmx_settings_t is the same load this function already does.
    // ⚠ ONLY build from the heard text if it is ADDRESSED TO US - found by testing
    // this in sim, where it was masked by a setting.
    //
    // The pileup exists because these stations called US. But the heard table
    // holds their LATEST message, and a caller who gave up has usually gone back
    // to calling CQ - so building from last_text would answer a pileup caller
    // with a grid TX1, which is exactly the #134 bug the report-first logic was
    // written to prevent ("Roy KI0ER lost QSOs to exactly this"). The sim run
    // showed 'auto-pileup N5XYZ: replying to CQ N5XYZ EM12' and still produced a
    // report - but only because Skip-TX1 happened to be ON, which turns a CQ
    // answer into a report anyway. With it off the grid would have gone out.
    //
    // So: their message to us decides the rung (grid -> report, report ->
    // R-report, which is the whole point of #172); anything else falls through to
    // report-first, which is the right default for someone who called us.
    ft8_call_t heard;
    bool heard_to_us = false;
    if (ft8_screen_find_call(pile[best].call, &heard)) {
        char h1[16], h2[16], hrest[FT8_FD_EXCH_LEN];
        heard_to_us = split_msg3(heard.last_text, h1, sizeof(h1), h2, sizeof(h2),
                                 hrest, sizeof(hrest)) &&
                      s_my_call[0] && strcmp(h1, s_my_call) == 0;
        if (!heard_to_us) {
            ESP_LOGI(TAG, "auto-pileup %s: last heard '%s' is not to us - "
                          "report-first", pile[best].call, heard.last_text);
        }
    }
    if (heard_to_us) {
        if (!ft8_qso_build_manual_reply(&heard, reply_freq_hz, &req, NULL, err, sizeof(err))) {
            ESP_LOGW(TAG, "auto-pileup build_manual_reply(%s) failed: %s",
                     pile[best].call, err);
            return false;
        }
        ESP_LOGI(TAG, "auto-pileup %s: replying to '%s'", pile[best].call, heard.last_text);
    } else {
        // Aged out of the heard table (60 s) while still queued in the pileup,
        // which is normal for a station waiting several minutes. Fall back to
        // report-first: everyone in the pileup CALLED US, so they already have
        // our grid and are waiting on a report. Roy KI0ER lost QSOs to a grid
        // being sent here.
        char pile_rpt[8];
        fmt_report(pile[best].snr_db, pile_rpt, sizeof(pile_rpt));
        if (!ft8_tx_build_request(FT8_TX_KIND_REPLY, pile[best].call, reply_freq_hz,
                                  pile[best].last_seen_utc, pile_rpt, &req, err, sizeof(err))) {
            ESP_LOGW(TAG, "auto-pileup build_request(%s) failed: %s", pile[best].call, err);
            return false;
        }
        ESP_LOGI(TAG, "auto-pileup %s: not in heard table, report-first fallback",
                 pile[best].call);
    }
    // Carry the CQ-run context ACROSS the start, the same way s_pileup_active is
    // re-set below, because ft8_qso_start() clears all of it (it cannot know an
    // automatic drain from a manual pounce).
    //
    // Without this, draining the pileup ENDED the CQ run: every drained contact
    // was marked "not from CQ", so when the pileup finally emptied the machine
    // fell to IDLE and stopped calling. During an activation that is the wrong
    // default - the operator decides when to stop calling, or the "CQ stop
    // after N calls" setting does. Nothing should silently stop the run just
    // because the queue drained. (Operator's call, 2026-08-15.)
    //
    // Only restored when we WERE in a CQ run: a pileup drained from IDLE must
    // not invent a CQ to resume into.
    lock();
    bool             was_from_cq   = s_from_cq;
    bool             had_cq_saved  = s_have_cq_saved;
    ft8_tx_request_t cq_ctx        = s_cq_saved;
    unlock();

    if (!ft8_qso_start(&req, err, sizeof(err))) {   // clears s_pileup_active
        ESP_LOGW(TAG, "auto-pileup ft8_qso_start(%s) refused: %s", pile[best].call, err);
        return false;
    }
    lock();
    s_pileup_active = true;   // set AFTER ft8_qso_start (which clears it)
    if (was_from_cq && had_cq_saved) {
        s_from_cq       = true;
        s_cq_saved      = cq_ctx;
        s_have_cq_saved = true;
    }
    unlock();
    ESP_LOGI(TAG, "auto-pileup: working %s (snr=%d, %d waiting)",
             pile[best].call, pile[best].snr_db, n);
    ft8_status_set("Pileup: working %s", pile[best].call);
    return true;
}

// Is the station we just finished with STILL asking us for the final? True only
// for a message addressed to us whose third field is a report or R-report -
// RR73/73/RRR all mean they heard us and are done, and anything else (a grid, a
// CQ) isn't them chasing this QSO.
static bool partner_still_awaiting_final(int64_t slot_sec, const char *call)
{
    ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) return false;   // PSRAM, not this task's stack (~11 KB)
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    bool asking = false;
    for (int i = 0; i < n && !asking; i++) {
        if (strcmp(snap[i].call, call) != 0) continue;
        if (snap[i].last_utc != slot_sec) continue;
        char tok1[16], tok2[16], rest[FT8_FD_EXCH_LEN];
        if (!split_msg3(snap[i].last_text, tok1, sizeof(tok1), tok2, sizeof(tok2),
                        rest, sizeof(rest))) continue;
        if (strcmp(tok1, s_my_call) != 0) continue;          // not addressed to us
        if (!strcmp(rest, "RR73") || !strcmp(rest, "73") ||
            !strcmp(rest, "RRR")) continue;                  // they DID hear us
        // A report ("-10") or a roger'd report ("R-10") - they're still waiting.
        const char *p = (rest[0] == 'R') ? rest + 1 : rest;
        if (*p == '+' || *p == '-') asking = true;
    }
    free(snap);
    return asking;
}

// Re-send the final to a just-worked partner who is still asking for it. Does
// NOT touch s_state or s_cur_req: it builds and arms one message directly, so
// whatever we were doing (idle, or an armed CQ) resumes by itself on the next
// on_tx_complete() re-arm. Nothing is logged - the QSO already was.
static bool final_resend_if_still_asked(int64_t slot_sec)
{
    if (!s_final_call[0]) return false;
    int64_t now_s = (int64_t)time(NULL);
    if ((now_s - s_last_done_ts) > FINAL_RESEND_WINDOW_SEC) {
        s_final_call[0] = '\0';       // window closed - stop watching for them
        return false;
    }
    if (s_final_resends >= FINAL_RESEND_MAX) return false;
    // Mid-burst: leave it alone and catch them on their next repeat.
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE) return false;
    if (!partner_still_awaiting_final(slot_sec, s_final_call)) return false;

    ft8_tx_request_t req;
    char err[64];
    if (!ft8_tx_build_request(s_final_kind, s_final_call, s_final_freq_hz,
                              slot_sec, s_final_extra, &req, err, sizeof(err))) {
        ESP_LOGW(TAG, "final re-send build failed for %s: %s", s_final_call, err);
        s_final_call[0] = '\0';
        return false;
    }
    if (!ft8_tx_arm(&req, err, sizeof(err))) {
        ESP_LOGW(TAG, "final re-send arm failed for %s: %s", s_final_call, err);
        return false;
    }
    s_final_resends++;
    ft8_status_set("QSO %s: never heard our %s - re-sending (%d/%d)",
                   s_final_call, s_final_extra, s_final_resends, FINAL_RESEND_MAX);
    ESP_LOGI(TAG, "%s still asking after completion - re-sending %s (%d/%d)",
             s_final_call, s_final_extra, s_final_resends, FINAL_RESEND_MAX);
    return true;
}

void ft8_qso_advance(int64_t slot_sec)
{
    capture_pileup_callers(slot_sec);

    // Before anything else can start a NEW contact this slot: finish the last
    // one properly if the partner is still waiting on our final. Runs only when
    // we aren't mid-exchange with someone else, and returns so this slot belongs
    // to them rather than to the next CQ or pileup pounce.
    {
        lock();
        ft8_qso_state_t stf = s_state;
        unlock();
        if (stf == FT8_QSO_IDLE || stf == FT8_QSO_DONE || stf == FT8_QSO_CQ) {
            // Is a just-worked partner STILL asking for our final? Evaluated
            // regardless of whether we have re-sends left, because the answer
            // also decides whether we are allowed to start talking to anyone
            // else this slot.
            int64_t now_s = (int64_t)time(NULL);
            bool asking = s_final_call[0] &&
                          (now_s - s_last_done_ts) <= FINAL_RESEND_WINDOW_SEC &&
                          partner_still_awaiting_final(slot_sec, s_final_call);
            s_final_hold = asking;

            if (asking) {
                if (final_resend_if_still_asked(slot_sec)) return;

                // Budget spent, but he is still calling. Say nothing rather than
                // call CQ over him: disarm anything queued (an ARMED burst fires
                // on its own otherwise - the lesson from the v1.3.3 busy-station
                // hold) and give the slot up. rearm_current() will not arm a new
                // CQ while s_final_hold is set.
                if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ARMED) ft8_tx_disarm();
                ft8_status_set("%s still asking - holding TX", s_final_call);
                return;
            }
        } else {
            s_final_hold = false;      // mid-exchange with someone else
        }
    }

    // Comeback auto-resume: a partner we abandoned recently decoded THIS slot
    // with a message addressed to us, and we're not mid-exchange with anyone
    // else - pick the QSO back up where it stopped. Restoring BEFORE the
    // state snapshot below means the restored WAIT_* handler processes their
    // message from this very slot (no extra cycle lost). An armed CQ is
    // cancelled exactly like cqrun_answer() does for a fresh answer.
    {
        lock();
        ft8_qso_state_t st0 = s_state;
        unlock();
        bool interruptible = (st0 == FT8_QSO_IDLE || st0 == FT8_QSO_DONE ||
                              st0 == FT8_QSO_TIMEOUT || st0 == FT8_QSO_CQ);
        if (interruptible && resume_comeback_heard(slot_sec)) {
            char who[FT8_CALL_MAX_LEN];
            strncpy(who, s_resume_call, sizeof(who) - 1);
            who[sizeof(who) - 1] = '\0';
            if (st0 == FT8_QSO_CQ) ft8_tx_disarm();
            resume_restore();
            ft8_status_set("QSO %s: partner came back - resuming", who);
            ESP_LOGI(TAG, "resume (auto): %s heard again - continuing exchange", who);
        }
    }

    lock();
    ft8_qso_state_t st = s_state;
    char    target[FT8_CALL_MAX_LEN];
    int     freq     = s_freq_hz;
    int64_t min_scan = s_min_scan_utc;
    bool    from_cq  = s_from_cq;
    strncpy(target, s_target, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    unlock();

    if (st == FT8_QSO_TIMEOUT) {
        // A pileup-initiated pounce that got no answer (the caller wandered off
        // in the minutes since they called) would normally go sticky. Instead
        // clear it and move to the next waiting station so one dead caller
        // doesn't stall the whole drain. A human/robot pounce timeout is left
        // sticky (s_pileup_active is false for those).
        if (s_pileup_active) {
            // ft8_qso_abort() also clears the CQ-run context, so without this a
            // single caller who wandered off would quietly end the whole CQ run
            // once the rest of the queue drained. Same rule as the completion
            // path: nothing automatic stops the operator calling CQ.
            lock();
            bool             was_from_cq  = s_from_cq;
            bool             had_cq_saved = s_have_cq_saved;
            ft8_tx_request_t cq_ctx       = s_cq_saved;
            unlock();

            ft8_qso_abort();                       // TIMEOUT -> IDLE, clears s_pileup_active

            if (was_from_cq && had_cq_saved) {
                lock();
                s_from_cq       = true;
                s_cq_saved      = cq_ctx;
                s_have_cq_saved = true;
                unlock();
            }
            if (try_start_pileup_pounce()) return; // next waiting station

            // Queue empty after a dead caller: fall back to the CQ we were
            // running rather than going idle.
            if (was_from_cq && had_cq_saved) {
                lock();
                s_cur_req       = cq_ctx;
                s_have_cur      = true;
                s_state         = FT8_QSO_CQ;
                s_target[0]     = '\0';
                s_missed_slots  = 0;
                s_cq_calls_sent = 0; s_cq_listen_done_at = -1;
                s_cq_exhausted  = false;
                unlock();
                arm_current_if_idle();
                ft8_status_set("CQ: calling - listening for answers");
                ESP_LOGI(TAG, "pileup drained (last caller timed out) - resuming CQ @ %d Hz",
                         cq_ctx.audio_freq_hz);
            }
        }
        return;
    }
    if (st == FT8_QSO_IDLE) {
        // Auto-work pileup from IDLE too: the drain used to trigger only at
        // QSO completion/timeout, so checking the box mid-session with a
        // pileup already waiting did nothing until the next QSO ended. Guards:
        // TX must be fully idle (never steal a manually-armed transmission)
        // and no fresh manual exchange in progress; try_start_pileup_pounce()
        // itself gates on the setting, identity, and waiting callers.
        char manual[FT8_CALL_MAX_LEN];
        if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_IDLE &&
            !ft8_qso_get_working_target(manual, sizeof(manual))) {
            try_start_pileup_pounce();
        }
        return;
    }

    if (st == FT8_QSO_DONE) {
        // Auto-work pileup: before falling back to CQ/idle, work anyone still
        // waiting in the pileup (they called us during a busy exchange). Drains
        // strongest-first across successive completions; only when the list is
        // empty do we resume CQ / go idle as before.
        if (try_start_pileup_pounce()) return;
        s_pileup_active = false;
        // A CQ-run QSO resumes calling CQ on the same tone instead of going
        // idle - same as the QSO_TIMEOUT_SLOTS give-up path in register_miss(),
        // just on the success side. Saves the operator from re-tapping Call CQ
        // after every single contact during an activation.
        lock();
        bool resume = s_from_cq && s_have_cq_saved;
        ft8_tx_request_t cq_req = s_cq_saved;
        if (resume) {
            s_cur_req      = cq_req;
            s_have_cur     = true;
            s_state        = FT8_QSO_CQ;
            s_target[0]    = '\0';
            s_missed_slots = 0;
            s_cq_calls_sent = 0; s_cq_listen_done_at = -1;   // resumed CQ = fresh sequence for the auto-stop count
            s_cq_exhausted  = false;
        } else {
            s_state    = FT8_QSO_IDLE;
            s_have_cur = false;
        }
        unlock();
        if (resume) {
            arm_current_if_idle();
            ft8_status_set("CQ: calling - listening for answers");
            ESP_LOGI(TAG, "QSO done - resuming CQ @ %d Hz", cq_req.audio_freq_hz);
        } else {
            ft8_status_set("Idle");
        }
        return;
    }

    if (st == FT8_QSO_WAIT_DONE) {
        // Final (73/RR73) armed or fired; once it leaves the air we're done.
        if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_IDLE) {
            lock(); s_state = FT8_QSO_DONE; s_have_cur = false; unlock();
            ft8_status_set("QSO %s: complete!", target);
            ESP_LOGI(TAG, "QSO with %s complete", target);
            clear_dt_follow();   // QSO done - back to the UTC/GPS beat
            // Drop the just-worked station from the pileup. It was removed at
            // qso_start, but the partner keeps addressing us through the
            // exchange (report/RR73/73, and Dirk DK7CVD's late-reply case), and
            // capture_pileup_callers no longer excludes them once s_target
            // clears - so without this (plus the worked-before skip in capture)
            // they'd reappear in the pileup right after a successful QSO.
            ft8_pileup_remove(target);
            // A manually-run exchange with this station is over too - stop
            // exempting/highlighting them as "being worked". Start the
            // just-completed grace instead (keeps their trailing 73 out of
            // the pileup - see DONE_GRACE_TTL_S).
            lock();
            if (s_manual_target[0] && strcmp(s_manual_target, target) == 0)
                s_manual_target[0] = '\0';
            strncpy(s_last_done_call, target, sizeof(s_last_done_call) - 1);
            s_last_done_call[sizeof(s_last_done_call) - 1] = '\0';
            s_last_done_ts = time(NULL);
            // Remember the final we just sent, so it can be re-sent if they turn
            // out not to have decoded it (see final_resend_if_still_asked).
            //
            // HOUND (rule 4): except after a hound contact, where there is no
            // final to re-send - the Fox's RR73 closed it and we deliberately
            // stayed quiet. Leaving s_final_call set would have us transmitting
            // "73" into a Fox's frequency for up to three slots simply because it
            // is still calling other hounds, which it always is.
            if (s_hound_active) {
                s_final_call[0] = '\0';
                s_final_resends = 0;
                s_hound_active  = false;   // session over; the next start decides afresh
            } else {
                strncpy(s_final_call, target, sizeof(s_final_call) - 1);
                s_final_call[sizeof(s_final_call) - 1] = '\0';
                s_final_freq_hz = s_freq_hz;
                s_final_kind    = s_have_cur ? s_cur_req.kind : FT8_TX_KIND_73;
                snprintf(s_final_extra, sizeof(s_final_extra), "%s",
                         s_have_cur && s_cur_req.extra_field[0] ? s_cur_req.extra_field : "73");
                s_final_resends = 0;
            }
            unlock();

            // A completed QSO with this call supersedes any older resumable
            // half-QSO - never auto-resume into a duplicate afterwards.
            if (s_resume_call[0] && strcmp(s_resume_call, target) == 0) {
                s_resume_call[0]  = '\0';
                s_resume_have_req = false;
            }

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

            // ARRL Field Day exchange (if any): their_exch is "<class> <section>";
            // split off the section (last token) for the dedicated ADIF fields.
            char their_fd_class[FT8_FD_EXCH_LEN] = {0}, their_fd_section[FT8_FD_EXCH_LEN] = {0};
            lock();
            bool have_fd_exch = s_fd_their_exch[0] != '\0';
            char fd_exch_copy[FT8_FD_EXCH_LEN];
            strncpy(fd_exch_copy, s_fd_their_exch, sizeof(fd_exch_copy) - 1);
            fd_exch_copy[sizeof(fd_exch_copy) - 1] = '\0';
            unlock();
            if (have_fd_exch) {
                char *sp = strrchr(fd_exch_copy, ' ');
                if (sp) {
                    *sp = '\0';
                    snprintf(their_fd_class, sizeof(their_fd_class), "%s", fd_exch_copy);
                    snprintf(their_fd_section, sizeof(their_fd_section), "%s", sp + 1);
                }
            }

            // Were they activating a park/summit? Resolved now, while the
            // contact is fresh - the spot can age out of the store long before
            // anyone exports the log, and a chase with no SIG_INFO earns no
            // credit for either side. Empty for an ordinary QSO.
            char their_sig[8] = "", their_ref[16] = "";
            spots_activation_for_call(target, cat_get_frequency(),
                                      their_sig, sizeof(their_sig),
                                      their_ref, sizeof(their_ref));

            adif_qso_t qso = {
                .their_call = target,
                .my_call    = s_my_call,
                .my_grid    = qs.my_grid,
                .their_grid = their_grid,
                .freq_hz    = cat_get_frequency(),
                .mode       = (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? "FT4" : "FT8",
                .rst_sent   = s_rst_sent,
                .rst_rcvd   = s_rst_rcvd,
                .qso_time   = time(NULL),
                .my_arrl_class      = have_fd_exch ? qs.fd_class   : NULL,
                .my_arrl_section    = have_fd_exch ? qs.fd_section : NULL,
                .their_arrl_class   = have_fd_exch ? their_fd_class   : NULL,
                .their_arrl_section = have_fd_exch ? their_fd_section : NULL,
                .their_sig      = their_sig[0] ? their_sig : NULL,
                .their_sig_info = their_ref[0] ? their_ref : NULL,
            };
            // Duplicate guard (see QSO_DUP_LOG_WINDOW_SEC): the same call on the
            // same band inside the window is this same contact reaching DONE a
            // second time - most often because the operator finished it by hand
            // after the machine already had - not a second QSO.
            bool dup = (s_logged_call[0] &&
                        strcmp(s_logged_call, target) == 0 &&
                        strcmp(adif_log_band_for_freq(qso.freq_hz),
                               adif_log_band_for_freq(s_logged_freq_hz)) == 0 &&
                        ((int64_t)time(NULL) - s_logged_ts) < QSO_DUP_LOG_WINDOW_SEC);
            if (dup) {
                ESP_LOGW(TAG, "not logging %s again - same call/band logged %llds ago",
                         target, (long long)((int64_t)time(NULL) - s_logged_ts));
            } else {
                adif_log_record(&qso);
                strncpy(s_logged_call, target, sizeof(s_logged_call) - 1);
                s_logged_call[sizeof(s_logged_call) - 1] = '\0';
                s_logged_freq_hz = qso.freq_hz;
                s_logged_ts      = (int64_t)time(NULL);
            }
            // Grace ring: note it on BOTH branches. A contact reaching DONE a
            // second time is still one we have just worked, and it is precisely
            // the case the unattended pickers must not pick up again.
            note_worked_now(target, qso.freq_hz);
            s_fd_their_exch[0] = '\0';
        }
        return;
    }

    // ---- CQ: listen for anyone answering us --------------------------------
    if (st == FT8_QSO_CQ) {
        char caller[FT8_CALL_MAX_LEN] = {0};
        int  caller_freq = freq;
        int  caller_snr  = 0;
        char report[FT8_FD_EXCH_LEN] = {0};
        bool got_rr73 = false, got_73 = false;
        if (scan_for_reply_to_me(slot_sec, caller, sizeof(caller),
                                 &caller_freq, &caller_snr,
                                 report, sizeof(report), &got_rr73, &got_73)) {
            ESP_LOGI(TAG, "CQ: %s answered @ %d Hz snr=%d (rr73=%d 73=%d)",
                     caller, caller_freq, caller_snr, got_rr73, got_73);

            // MANUAL PICK (Eric K3FNB): the operator chooses who to work rather
            // than the first answer winning. Only the CHOICE is manual - a tapped
            // caller runs the normal automated exchange from there.
            //
            // Deliberately keeps calling CQ instead of standing down. An activator
            // wants the pile-up to keep building while they pick, and going silent
            // would look like the radio had stopped. capture_pileup_callers() has
            // already recorded this caller at the top of advance(), so they are
            // tappable in the pile-up list without anything extra here.
            qmx_settings_t mp;
            settings_load_all(&mp);
            if (mp.ft8_filters.cq_manual_pick) {
                ESP_LOGI(TAG, "CQ: manual pick is on - %s is waiting in the pile-up, "
                              "not answering automatically", caller);
                return;
            }

            ft8_tx_disarm();   // cancel the re-armed CQ (no-op if already ACTIVE)
            cqrun_answer(caller, caller_freq, caller_snr, report, slot_sec, got_rr73, got_73);
        } else {
            // Auto-stop limit reached and the extra listening slot after the
            // final call brought no answer - end the CQ session. Deliberately
            // IDLE, not TIMEOUT: nothing went wrong, the operator asked for
            // exactly this pause.
            lock();
            bool exhausted = s_cq_exhausted;
            int  sent      = s_cq_calls_sent;
            unlock();
            if (exhausted) {
                lock();
                s_state         = FT8_QSO_IDLE;
                s_have_cur      = false;
                s_have_cq_saved = false;
                s_cq_exhausted  = false;
                s_cq_calls_sent = 0; s_cq_listen_done_at = -1;
                unlock();
                ft8_tx_disarm();
                ft8_status_set("CQ stopped after %d calls - no answer", sent);
                ESP_LOGI(TAG, "CQ auto-stop: %d calls unanswered - session ended", sent);
                return;
            }
            // No answer - check whether someone has drifted onto our tone
            // since we started calling, and move off it if so. Otherwise
            // on_tx_complete keeps the CQ armed at the same tone; idle
            // fallback only.
            relocate_tone_if_clashing(true);
            arm_current_if_idle();
        }
        return;
    }

    // ---- Exchange states: hear the partner on the opposite-parity slot -----
    // (our first message hasn't fired yet -> nothing to hear; don't scan
    // early). Normally only WAIT_RPT needs this (pounce hasn't sent TX1 yet),
    // but a skip-TX1 pounce starts straight in WAIT_ROGER, so it needs the
    // same guard - gated to !from_cq so CQ-run's own WAIT_ROGER (whose reply
    // has always already fired by the time advance() can next run - see
    // cqrun_answer()) is never affected.
    if ((st == FT8_QSO_WAIT_RPT || (st == FT8_QSO_WAIT_ROGER && !from_cq)) &&
        slot_sec < min_scan) return;

    char report[FT8_FD_EXCH_LEN] = {0};
    bool got_rr73 = false, got_73 = false;
    int  snr_db   = 0;
    bool found = scan_for_response(slot_sec, report, sizeof(report), &got_rr73, &got_73, &snr_db);

    // Heard the partner this slot -> update the DT-follow-partner TX offset to
    // their current beat (engages only if they're >threshold off the band).
    if (found) update_dt_follow(target);

    // They didn't answer US - but are they visibly mid-exchange with a third
    // party? If so this is not a missed reply, it's a busy frequency: hold
    // WITHOUT counting a miss, so we neither time out nor grey-list a station
    // whose only fault is being popular (Roy KI0ER). rearm_current() reads
    // s_busy_holds and skips the transmission itself. Pounce only - CQ-run's
    // partner answered us, so one wandering off is a lost QSO that should time
    // out normally. Bounded: past the cap we fall through to the usual
    // miss/timeout path so a vanished station still gives up.
    // HOUND (rule 3): never hold for a busy Fox. A Fox is working somebody in
    // every single slot - that is what a Fox IS - so the hold below would engage
    // immediately and keep us silent for its whole budget, i.e. we would join the
    // pileup by not calling. Being ignored while it works its queue is the normal
    // hound experience, and the ordinary miss/timeout path handles it.
    if (!found && !from_cq && target[0] && !s_hound_active &&
        (st == FT8_QSO_WAIT_RPT || st == FT8_QSO_WAIT_ROGER || st == FT8_QSO_WAIT_RR73)) {
        char with[FT8_CALL_MAX_LEN] = {0};
        if (s_robot_started && partner_busy_with(target, with, sizeof with)) {
            // Robot pick turned out busy: abandon, do not wait (Roy KI0ER). The
            // robot's next tick chooses another CQ caller - and it cannot
            // re-pick this one immediately, because their last message is a
            // reply, not a CQ, so the is_cq gate excludes them until they call
            // again. Deliberately NOT a grey-list strike: busy is not
            // unresponsive, and the hold never counted a miss either.
            ESP_LOGI(TAG, "robot pick %s is working %s - moving on instead of waiting",
                     target, with[0] ? with : "someone");
            ft8_status_set("%s busy with %s - robot moving on",
                           target, with[0] ? with : "someone");
            lock();
            s_state         = FT8_QSO_IDLE;
            s_target[0]     = '\0';
            s_have_cur      = false;
            s_from_cq       = false;
            s_partner_freq_hz = 0;
            s_robot_started = false;
            unlock();
            ft8_tx_disarm();          // the re-armed burst must not fire either
            clear_dt_follow();
            return;
        }
        if (partner_busy_with(target, with, sizeof with) &&
            s_busy_holds < QSO_BUSY_HOLD_MAX_SLOTS) {
            lock();
            s_busy_holds++;
            snprintf(s_busy_with, sizeof s_busy_with, "%s", with);
            int holds = s_busy_holds;
            unlock();
            // Also cancel the burst that on_tx_complete() already armed before
            // we knew they were busy - otherwise the first held slot still
            // transmits (hardware-observed), and saving transmissions is the
            // entire point. No-op if it's already ACTIVE, so a burst on air is
            // never cut mid-message; the hold then starts from the next cycle.
            // Releasing the hold calls arm_current_if_idle() to put it back.
            ft8_tx_disarm();
            ft8_status_set("QSO %s: working %s - waiting (%d/%d)",
                           target, with[0] ? with : "someone", holds,
                           QSO_BUSY_HOLD_MAX_SLOTS);
            ESP_LOGI(TAG, "%s is working %s - holding, not counting a miss (%d/%d)",
                     target, with[0] ? with : "someone", holds, QSO_BUSY_HOLD_MAX_SLOTS);
            return;
        }
        // Not busy any more (or held long enough): stop holding so the next
        // rearm_current() transmits again and misses resume counting.
        if (s_busy_holds) {
            ESP_LOGI(TAG, "%s free again (or hold expired) after %d slots - resuming",
                     target, s_busy_holds);
            lock();
            s_busy_holds   = 0;
            s_busy_with[0] = '\0';
            unlock();
            arm_current_if_idle();   // we skipped re-arms while holding
        }
    }

    qmx_settings_t qs_exch;
    settings_load_all(&qs_exch);
    bool fd_mode = qs_exch.field_day_en && qs_exch.fd_class[0] && qs_exch.fd_section[0];

    if (st == FT8_QSO_WAIT_RPT) {
        // POUNCE: we sent our grid; expect their signal report (normal mode)
        // or their class+section (Field Day - sent without "R", since this is
        // their first FD-specific message to us).
        if (!found) {
            // #201 (Roy KI0ER): TX1 is armed but has not gone out yet, so our
            // tone is still nobody's reference - if someone has landed on it in
            // the meantime, move before we key rather than transmitting on top
            // of them and merely showing "FREQ BUSY".
            //
            // #219: no longer gated on !s_tx_sent. Moving mid-QSO is legitimate
            // - the partner matches on callsign, not on tone (Roy KI0ER) - and
            // this is the no-progress path, so it only fires when we are being
            // stepped on and getting nowhere, which is exactly when a WSJT-X
            // operator would move by hand. The bounds live in
            // relocate_tone_if_clashing(): at most QSO_MIDQSO_MOVE_MAX_HZ, at
            // most QSO_MIDQSO_MOVE_LIMIT times, never during a burst.
            relocate_tone_if_clashing(false);
            // Skip-TX1 opens with a report, so this message can carry one
            // even here; refresh it if we re-heard them this slot.
            if (!fd_mode) refresh_report_from_heard(false, target, freq, slot_sec);
            register_miss("waiting for report");
            return;
        }

        // Our own locally-measured SNR of their signal. This is what TX2's
        // "R<report>" must carry - our measurement of THEM, NOT an echo of the
        // report they sent us (fixed 2026-07-17: two field reports, Steve N0SZ
        // + Jonathan KN6LFB, both saw us reply R<their-report> regardless of
        // actual strength; skip-TX1 was already correct because it builds the
        // report from this same SNR). Also the RST_SENT logged for ADIF, same
        // convention cqrun_answer() uses for the CQ-run direction.
        char our_rpt[8];
        fmt_report(snr_db, our_rpt, sizeof(our_rpt));
        strncpy(s_rst_sent, our_rpt, sizeof(s_rst_sent) - 1);
        s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';

        bool ok;
        if (got_rr73 || got_73) {
            ESP_LOGI(TAG, "WAIT_RPT: %s sent RR73/73 directly", target);
            ok = send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73",
                           FT8_QSO_WAIT_DONE);
            if (ok) ft8_status_set("QSO %s: sending 73", target);
        } else if (fd_mode) {
            // Their class+section (no R - this is their first FD message).
            // Capture it for ADIF, then acknowledge with our own, R-prefixed.
            strncpy(s_fd_their_exch, report, sizeof(s_fd_their_exch) - 1);
            s_fd_their_exch[sizeof(s_fd_their_exch) - 1] = '\0';
            char roger[FT8_FD_EXCH_LEN];
            snprintf(roger, sizeof(roger), "R %s %s", qs_exch.fd_class, qs_exch.fd_section);
            ESP_LOGI(TAG, "WAIT_RPT (FD): %s sent '%s' -> TX2 %s", target, report, roger);
            ok = send_next_fd(FT8_TX_KIND_ROGER_RPT, target, freq, slot_sec, roger,
                              FT8_QSO_WAIT_RR73);
            if (ok) ft8_status_set("QSO %s: heard %s - sending %s", target, report, roger);
        } else {
            // TX2 = "R" + OUR measured report of them (our_rpt), not their
            // report echoed back. `report` (their report of us) is captured
            // as RST_RCVD only. roger[] sized generously so the inlined
            // make_roger's "R%s" can't trip -Wformat-truncation on our_rpt's
            // 8-byte bound (real content is tiny, e.g. "R-04").
            char roger[16];
            make_roger(our_rpt, roger, sizeof(roger));

            // HOUND (rule 1): QSY DOWN onto the Fox before answering. This is the
            // whole point of Fox/Hound - the Fox listens only to its own narrow
            // slice, so an R-report sent from our up-band calling tone is never
            // heard and the contact dies one message short.
            //
            // Applied to s_freq_hz as well as to this message, so every re-send
            // (rearm_current) stays down there until the Fox rogers us. The
            // partner's tone comes from their decodes and is real Hz, not a bin
            // index - the v0.18.4 fix, without which this would QSY to nonsense.
            if (s_hound_active && s_partner_freq_hz > 0) {
                int fox_hz = s_partner_freq_hz;
                if (fox_hz < FT8_TX_TONE_MIN_HZ) fox_hz = FT8_TX_TONE_MIN_HZ;
                if (freq != fox_hz) {
                    ESP_LOGI(TAG, "HOUND: QSY %d -> %d Hz (onto %s) for the R-report",
                             freq, fox_hz, target);
                    ft8_status_set("Hound: QSY to %d Hz - answering %s", fox_hz, target);
                }
                freq = fox_hz;
                lock(); s_freq_hz = fox_hz; unlock();
            }

            ESP_LOGI(TAG, "WAIT_RPT: %s reported us %s, we heard them %s -> TX2 %s",
                     target, report, our_rpt, roger);
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
        arm_current_replacing_armed();
        return;
    }

    if (st == FT8_QSO_WAIT_ROGER) {
        // CQ-RUN: we sent a report; expect their R<report> (then we send RR73).
        if (!found) {
            // #219: same as WAIT_RPT - stepped on and getting nowhere is when a
            // WSJT-X operator moves, so do it here too, under the same bounds.
            relocate_tone_if_clashing(false);
            // THE reported case: we answered their CQ with a report and
            // they have not come back to us. Re-send with this slot's
            // measurement rather than the one from when we first called.
            if (!fd_mode) refresh_report_from_heard(false, target, freq, slot_sec);
            register_miss("waiting for roger");
            return;
        }

        if (got_rr73 || got_73) {
            if (send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73", FT8_QSO_WAIT_DONE))
                ft8_status_set("QSO %s: sending 73", target);
            arm_current_replacing_armed();
            return;
        }
        if (is_roger_token(report)) {
            if (report[1] == ' ') {
                // Field Day ack "R <theirclass> <theirsection>" - capture their
                // exchange for ADIF (strip the "R " prefix).
                strncpy(s_fd_their_exch, report + 2, sizeof(s_fd_their_exch) - 1);
                s_fd_their_exch[sizeof(s_fd_their_exch) - 1] = '\0';
            } else if (report[1] == '+' || report[1] == '-' ||
                       (report[1] >= '0' && report[1] <= '9')) {
                // Numeric roger "R<rpt>": the value is their own measurement of
                // OUR signal, not an echo of the report we sent (we sent -08,
                // OS4K rogered R-06 - live capture 2026-07-15). This is the
                // RST_RCVD for a CQ-run/skip-TX1 QSO; without this it logged
                // the "599" placeholder. Excludes "RRR" (no numeric value).
                strncpy(s_rst_rcvd, report + 1, sizeof(s_rst_rcvd) - 1);
                s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
            }
            if (send_next(FT8_TX_KIND_ROGER_RPT, target, freq, slot_sec, "RR73",
                          FT8_QSO_WAIT_DONE))
                ft8_status_set("QSO %s: rogered %s - sending RR73", target, report);
            arm_current_replacing_armed();
            return;
        }
        // They repeated their grid/report (didn't get ours). Before counting
        // the miss, refresh our report to the SNR we measured THIS slot so the
        // re-send - and the RST_SENT we log - carries our freshest reading, the
        // way WSJT-X does. FD's fixed class+section is never refreshed.
        if (!fd_mode) refresh_our_report(snr_db, false, target, freq, slot_sec);
        register_miss("re-sending report");
        return;
    }

    if (st == FT8_QSO_WAIT_RR73) {
        // POUNCE: we sent R<report>; expect RR73/73 (then we send 73).
        //
        // "RRR" is the older roger form and closes the exchange exactly like
        // RR73 - we've sent our R<report>, they've acknowledged it, all that's
        // left is our 73. Without this the exact-string RR73/73 test below
        // dropped "RRR" into the "they repeated their report" branch, so we
        // re-sent R<report> every slot forever while they re-sent RRR (Roy
        // KI0ER, 2026-07-27, working NH6L - the QSO only completed once he
        // took over by hand). Handled here rather than folded into
        // scan_for_response()'s got_rr73 on purpose: WAIT_ROGER above already
        // accepts "RRR" via is_roger_token() and must keep answering it with
        // RR73, not the 73 that got_rr73 would produce there.
        bool got_rrr = (strcmp(report, "RRR") == 0);
        if (!found || (!got_rr73 && !got_73 && !got_rrr)) {
            // Re-heard them repeating their report (found, but no RR73 yet):
            // refresh our R<report> to this slot's fresh SNR before re-sending,
            // same WSJT-X behaviour as the CQ-run WAIT_ROGER path above.
            if (found && !fd_mode) refresh_our_report(snr_db, true, target, freq, slot_sec);
            // Not addressed to us this slot, but still heard: our R<report>
            // gets the same treatment.
            else if (!fd_mode) refresh_report_from_heard(true, target, freq, slot_sec);
            relocate_tone_if_clashing(false);   // #219, same bounds
            register_miss("waiting for RR73");
            return;
        }
        // HOUND (rule 2): the Fox's RR73 ENDS it. A hound does not send 73 - the
        // Fox's frequency is the scarcest thing on the band and a courtesy 73
        // there is pure clutter, on top of the pileup F/H exists to thin out.
        //
        // Completing without a final message: go to WAIT_DONE with nothing armed,
        // and its handler (which waits for TX_IDLE) logs the QSO, clears the
        // pileup and posts "complete" exactly as it does for any other contact -
        // no second completion path to keep in step. Also hop back to the hound
        // tone now, so the next Fox call goes out up-band where it belongs.
        if (s_hound_active) {
            ESP_LOGI(TAG, "HOUND: %s sent %s - complete, no 73 (Fox frequency stays clear)",
                     target, got_rrr ? "RRR" : "RR73/73");
            ft8_tx_disarm();          // the re-armed R-report must not fire again
            lock();
            s_state    = FT8_QSO_WAIT_DONE;
            s_have_cur = false;
            if (s_hound_tone_hz > 0) s_freq_hz = s_hound_tone_hz;
            unlock();
            ft8_status_set("Hound: %s worked - back on %d Hz", target,
                           s_hound_tone_hz > 0 ? s_hound_tone_hz : freq);
            return;
        }

        ESP_LOGI(TAG, "WAIT_RR73: %s sent %s - arming TX3", target,
                 got_rrr ? "RRR" : "RR73/73");
        if (send_next(FT8_TX_KIND_73, target, freq, slot_sec, "73", FT8_QSO_WAIT_DONE))
            ft8_status_set("QSO %s: sending 73", target);
        else {
            lock(); s_state = FT8_QSO_TIMEOUT; unlock();
            ft8_status_set("QSO %s: TX error", target);
        }
        arm_current_replacing_armed();
        return;
    }
}

void ft8_qso_on_tx_complete(void)
{
    // #201: from here on the partner may be tracking our tone - stop moving it.
    s_tx_sent = true;
    // Clamp the scan gate to when our message ACTUALLY fired. s_min_scan_utc
    // is predicted at start (next_slot_sec) assuming the burst waits for the
    // next matching boundary - but the reply-window path (FT8_REPLY_TX_WINDOW_MS)
    // can legally fire it mid-slot in the CURRENT slot, up to a full parity
    // cycle earlier than predicted. The stale prediction then discards the
    // partner's prompt reply as "too early" and we re-send TX1/the report once
    // for nothing (hardware-observed: min_scan 30 s in the future, phantom's
    // R-report in the very next slot ignored). We're called right after the
    // burst, still inside its slot, so "now's slot + 1" is the true earliest
    // slot a reply can arrive in. Only ever lowers the gate, never raises it.
    lock();
    // Count the CQ burst that just finished for the auto-stop limit. State is
    // still CQ only while nobody has answered (an answer moves the state
    // machine on mid-burst, at which point the count no longer matters).
    if (s_state == FT8_QSO_CQ) s_cq_calls_sent++;
    if (s_state != FT8_QSO_IDLE && s_min_scan_utc > 0) {
        int period_ms = (s_have_cur && s_cur_req.protocol == FTX_PROTOCOL_FT4) ? 7500 : 15000;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        int64_t earliest = ((now_ms / period_ms) * period_ms + period_ms) / 1000;
        if (s_min_scan_utc > earliest) {
            ESP_LOGI(TAG, "TX fired earlier than predicted - min_scan %lld -> %lld",
                     (long long)s_min_scan_utc, (long long)earliest);
            s_min_scan_utc = earliest;
        }
    }
    unlock();

    // ⭐ RST_SENT is what we actually TRANSMITTED, not what we last armed.
    //
    // Gyula HA3HZ reported logged report values being wrong. s_rst_sent was
    // written at ARM time in three places, and an armed request can be replaced
    // before it ever reaches the air - refresh_our_report() does exactly that,
    // deliberately, whenever we re-hear the partner with a fresher SNR. So the
    // log could carry a report that was never sent, which is the same class of
    // dishonesty as the fabricated "599" removed in v1.3.4: a value in the log
    // that nothing on the air supports.
    //
    // We are called immediately after the burst and ft8_tx_arm() refuses while a
    // burst is ACTIVE, so s_cur_req here IS the message that just went out.
    // Latching from its extra_field makes RST_SENT track reality; if a fresher
    // report is armed afterwards it will overwrite this on ITS own completion.
    //
    // Guarded to reports only: a Field Day exchange carries class+section in the
    // same field and a TX1 carries a grid, and neither is a signal report.
    lock();
    if (s_have_cur && (s_cur_req.kind == FT8_TX_KIND_REPLY ||
                       s_cur_req.kind == FT8_TX_KIND_ROGER_RPT)) {
        const char *e = s_cur_req.extra_field;
        if (e[0] == 'R' && (e[1] == '+' || e[1] == '-')) e++;   /* "R-10" -> "-10" */
        if ((e[0] == '+' || e[0] == '-') && e[1] >= '0' && e[1] <= '9') {
            if (strcmp(e, s_rst_sent) != 0) {
                ESP_LOGI(TAG, "RST_SENT <- %s (as transmitted; was '%s')", e, s_rst_sent);
            }
            strncpy(s_rst_sent, e, sizeof(s_rst_sent) - 1);
            s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
        }
    }
    unlock();

    // The operator picked a new TX offset while this burst was on the air.
    // Apply it now, BEFORE the re-arm below, so the very next transmission uses
    // it - that is the whole point of queueing it rather than refusing.
    lock();
    int pending = s_pending_tone_hz;
    if (pending > 0) s_pending_tone_hz = 0;
    bool running = (s_state != FT8_QSO_IDLE);
    if (pending > 0 && running) {
        s_freq_hz = pending;
        // Parity untouched, exactly as in the immediate path: the partner
        // tracks our slot, not our tone.
        if (s_have_cur)      s_cur_req.audio_freq_hz  = pending;
        if (s_have_cq_saved) s_cq_saved.audio_freq_hz = pending;
    }
    unlock();
    if (pending > 0 && running) {
        ft8_status_set("TX tone -> %d Hz", pending);
        ESP_LOGI(TAG, "TX tone applied after the burst: %d Hz", pending);
    }

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

void ft8_qso_note_manual_target(const char *target_call)
{
    if (!target_call || !target_call[0]) return;
    lock();
    strncpy(s_manual_target, target_call, sizeof(s_manual_target) - 1);
    s_manual_target[sizeof(s_manual_target) - 1] = '\0';
    s_manual_target_ts = time(NULL);
    unlock();
}

// "An exchange is under way with s_target." The one predicate both accessors
// below ask, because they used to ask different questions and disagreed:
// get_working_target() tested the STATE (so it released at completion) while
// get_pinned_call() tested s_target[0] - which is deliberately NOT cleared when
// a QSO completes, because final_resend_if_still_asked() needs it afterwards.
// So the browser's decode list kept the finished station highlighted as "being
// worked" until the next QSO overwrote the name (Randy N4OPI: "that call also
// does not change from green text to grey in the decode list until a new QSO
// starts"). Route any future "are we working someone" test through here.
static inline bool qso_state_is_live(ft8_qso_state_t st)
{
    return st == FT8_QSO_WAIT_RPT || st == FT8_QSO_WAIT_ROGER ||
           st == FT8_QSO_WAIT_RR73 || st == FT8_QSO_WAIT_DONE;
}

bool ft8_qso_get_working_target(char *buf, size_t len)
{
    if (!buf || !len) return false;
    buf[0] = '\0';
    lock();
    ft8_qso_state_t st = s_state;
    if (qso_state_is_live(st)) {
        strncpy(buf, s_target, len - 1);
        buf[len - 1] = '\0';
    } else if (s_manual_target[0] &&
               (time(NULL) - s_manual_target_ts) < MANUAL_TARGET_TTL_S) {
        strncpy(buf, s_manual_target, len - 1);
        buf[len - 1] = '\0';
    }
    unlock();
    return buf[0] != '\0';
}

void ft8_qso_notify_manual_final(const char *target_call)
{
    if (!target_call || !target_call[0]) return;

    lock();
    ft8_qso_state_t st = s_state;
    unlock();
    if (st != FT8_QSO_IDLE && st != FT8_QSO_DONE && st != FT8_QSO_TIMEOUT) {
        return;   // a machine QSO is running - its own completion path logs
    }

    // Reports for the ADIF record, best-effort from the decode table:
    // RST_SENT = our measured SNR of them (same value the manual builder puts
    // in a report message); RST_RCVD = their numeric report of us if their
    // last message carried one ("R-09"/"-09"), else left EMPTY. In a manual
    // grid-flow their report arrived a step earlier and isn't stored, so an
    // RR73-as-last-message leaves RST_RCVD unset - the honest answer for a
    // hand-run QSO, and adif_log.c omits the field rather than faking it.
    char rst_sent[8] = "", rst_rcvd[8] = "";
    ft8_call_t *snap = heap_caps_malloc(
        sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap) {   // PSRAM, never the LVGL task's small stack (~11 KB snapshot)
        int n = 0;
        ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
        for (int i = 0; i < n; i++) {
            if (strcmp(snap[i].call, target_call) != 0) continue;
            fmt_report(snap[i].last_snr_db, rst_sent, sizeof(rst_sent));
            char t1[16], t2[16], rest[FT8_FD_EXCH_LEN];
            if (split_msg3(snap[i].last_text, t1, sizeof(t1), t2, sizeof(t2),
                           rest, sizeof(rest))) {
                const char *rpt = (rest[0] == 'R' && (rest[1] == '+' || rest[1] == '-'))
                                    ? rest + 1
                                    : ((rest[0] == '+' || rest[0] == '-') ? rest : NULL);
                if (rpt) snprintf(rst_rcvd, sizeof(rst_rcvd), "%.7s", rpt);
            }
            break;
        }
        free(snap);
    }

    lock();
    s_state = FT8_QSO_WAIT_DONE;
    strncpy(s_target, target_call, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_from_cq       = false;
    s_have_cur      = false;   // the final is already armed by the modal
    s_have_cq_saved = false;
    s_min_scan_utc  = 0;
    s_missed_slots  = 0;
    strncpy(s_rst_sent, rst_sent, sizeof(s_rst_sent) - 1);
    s_rst_sent[sizeof(s_rst_sent) - 1] = '\0';
    strncpy(s_rst_rcvd, rst_rcvd, sizeof(s_rst_rcvd) - 1);
    s_rst_rcvd[sizeof(s_rst_rcvd) - 1] = '\0';
    s_fd_their_exch[0] = '\0';
    unlock();

    ft8_status_set("QSO %s: sending final", target_call);
    ESP_LOGI(TAG, "manual final to %s armed - WAIT_DONE (sent=%s rcvd=%s)",
             target_call, rst_sent, rst_rcvd);
}

void ft8_qso_abort(void)
{
    lock();
    char target[FT8_CALL_MAX_LEN];
    strncpy(target, s_target, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    // A mid-exchange abort is resumable too (no-op for CQ/idle aborts - the
    // save helper only records WAIT_* exchange states).
    resume_record_save_locked();
    s_state         = FT8_QSO_IDLE;
    s_target[0]     = '\0';
    s_have_cur      = false;
    s_have_cq_saved = false;
    s_from_cq       = false;
    s_partner_freq_hz = 0;
    s_busy_holds    = 0;
    s_busy_with[0]  = '\0';
    s_pending_tone_hz = 0;   // never carry a queued tone into the next contact
    s_cq_calls_sent = 0; s_cq_listen_done_at = -1;
    s_cq_exhausted  = false;
    s_pileup_active = false;   // an abort ends any pileup drain
    // An aborted hound contact must not leave us parked on the Fox's frequency
    // (we may have QSY'd down for the R-report), and the next contact decides
    // hound-ness afresh from its own partner.
    if (s_hound_active && s_hound_tone_hz > 0) s_freq_hz = s_hound_tone_hz;
    s_hound_active  = false;
    clear_dt_follow();         // ...and returns TX to the UTC/GPS beat
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

bool ft8_qso_is_hound_active(void)
{
    lock(); bool h = s_hound_active; unlock();
    return h;
}

int ft8_qso_get_cq_calls_sent(void)
{
    lock();
    int n = (s_state == FT8_QSO_CQ) ? s_cq_calls_sent : -1;
    unlock();
    return n;
}

void ft8_qso_get_target(char *buf, size_t len)
{
    if (!buf || !len) return;
    lock();
    strncpy(buf, s_target, len - 1);
    buf[len - 1] = '\0';
    unlock();
}

void ft8_qso_get_pinned_call(char *buf, size_t len)
{
    if (!buf || !len) return;
    buf[0] = '\0';
    lock();
    if (s_target[0] && qso_state_is_live(s_state)) {
        // A live exchange, engine-driven (pounce or CQ-run answer). The state
        // test is load-bearing: s_target survives completion on purpose (the
        // final re-send reads it), so testing the name alone kept a logged
        // station pinned and green - see qso_state_is_live().
        strncpy(buf, s_target, len - 1);
    } else if (s_manual_target[0] &&
               ((int64_t)time(NULL) - s_manual_target_ts) < MANUAL_TARGET_TTL_S) {
        // Tapped a row and replied by hand without the state machine taking over.
        // Same TTL the pileup exemption uses, so the two agree on "still working
        // this station".
        strncpy(buf, s_manual_target, len - 1);
    }
    buf[len - 1] = '\0';
    unlock();
}

int ft8_qso_get_tx_tone_hz(void)
{
    lock();
    int hz = (s_state == FT8_QSO_IDLE) ? 0 : s_freq_hz;
    unlock();
    return hz;
}

bool ft8_qso_set_tx_tone_hz(int hz, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';

    if (hz < FT8_TX_TONE_MIN_HZ || hz > FT8_TX_TONE_MAX_HZ) {
        if (err) snprintf(err, err_len, "Tone must be %d-%d Hz",
                          FT8_TX_TONE_MIN_HZ, FT8_TX_TONE_MAX_HZ);
        return false;
    }

    // Never mid-burst: ft8_tx_disarm() is a no-op while ACTIVE and ft8_tx_arm()
    // refuses outright, so moving now would update our bookkeeping while the
    // engine kept transmitting on the old tone - half-applied, which is worse
    // than not applying.
    //
    // ⭐ BUT DO NOT REFUSE - REMEMBER IT. An FT8 burst is ~12.6 s of a 15 s slot
    // and a QSO transmits every other slot, so roughly 40% of attempts landed
    // mid-burst and were rejected. The exchange then carried on at the tone it
    // started on, which is exactly what Roy KI0ER reported: "it does not honor
    // my new choice, but instead remembers the offset my station transmitted on
    // at the start of the qso". Measured on the bench: the FIRST attempt at a
    // mid-QSO move came back "Transmitting - try again after this burst".
    //
    // Making the operator's choice depend on their timing is the bug. The burst
    // still finishes on the old tone (it must), and on_tx_complete() applies
    // this at the first legal moment.
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE) {
        lock();
        bool running = (s_state != FT8_QSO_IDLE);
        if (running) s_pending_tone_hz = hz;
        unlock();
        if (!running) {
            if (err) snprintf(err, err_len, "No CQ or QSO running");
            return false;
        }
        ft8_status_set("TX tone -> %d Hz after this burst", hz);
        ESP_LOGI(TAG, "TX tone %d Hz queued - applying when the burst ends", hz);
        return true;
    }

    lock();
    ft8_qso_state_t st = s_state;
    int old_hz = s_freq_hz;
    if (st != FT8_QSO_IDLE) {
        s_freq_hz = hz;
        // Parity is deliberately untouched: only audio_freq_hz changes, so the
        // request keeps its use_parity/want_even_slot and still fires in the
        // same slot. That's what makes a mid-exchange move safe - the partner
        // tracks our slot, not our tone (same reasoning WSJT-X relies on).
        if (s_have_cur)      s_cur_req.audio_freq_hz  = hz;
        if (s_have_cq_saved) s_cq_saved.audio_freq_hz = hz;  // survives a resume-CQ
    }
    unlock();

    if (st == FT8_QSO_IDLE) {
        if (err) snprintf(err, err_len, "No CQ or QSO running");
        return false;
    }

    ft8_tx_disarm();          // drop the request still armed at the old tone
    arm_current_if_idle();    // ...and re-arm the same message at the new one
    ft8_status_set("TX tone -> %d Hz", hz);
    ESP_LOGI(TAG, "TX tone moved %d Hz -> %d Hz (state=%d)", old_hz, hz, (int)st);
    return true;
}

bool ft8_qso_is_busy(char *target_buf, size_t len)
{
    lock();
    ft8_qso_state_t st = s_state;
    // snprintf, not strncpy: it always terminates, and the strncpy form made GCC
    // warn about a truncated copy that the following assignment already handled.
    if (target_buf && len) snprintf(target_buf, len, "%s", s_target);
    unlock();
    return st != FT8_QSO_IDLE && st != FT8_QSO_DONE && st != FT8_QSO_TIMEOUT;
}

void ft8_qso_get_cur_extra(char *buf, size_t len)
{
    if (!buf || !len) return;
    lock();
    if (s_have_cur && s_cur_req.extra_field[0])
        strncpy(buf, s_cur_req.extra_field, len - 1);
    else
        buf[0] = '\0';
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

bool ft8_qso_get_priority_freq(int *freq_hz_out)
{
    lock();
    ft8_qso_state_t st   = s_state;
    bool             from_cq = s_from_cq;
    int              freq    = s_partner_freq_hz;
    unlock();

    // Pounce only (WAIT_RPT / WAIT_ROGER / WAIT_RR73 — WAIT_ROGER normally
    // belongs to CQ-run, see the state table in ft8_qso.h, but a skip-TX1
    // pounce starts straight in WAIT_ROGER too; the `from_cq` check above
    // already excludes the real CQ-run case, so including it here only ever
    // matches the skip-TX1 pounce).
    //
    // Returns the PARTNER's tone (s_partner_freq_hz), which is what the caller
    // wants: the frequency their next message will arrive on. This used to
    // return s_freq_hz — OUR TX tone — which for a pounce is a deliberately
    // CLEAR slot away from theirs (ft8_find_clear_tone_hz), so the decode
    // reorder in ft8_test.c was aimed ~250 Hz off the partner and the
    // optimisation never fired. Measured 2026-07-28: partner at 1200 Hz, our
    // burst at 1450 Hz, hint 1450, ±25 Hz window 1425–1475.
    //
    // CQ-run is still skipped: there the answering station's tone isn't
    // tracked after the initial answer, so we'd have no reliable hint anyway.
    if (from_cq) return false;
    if (st != FT8_QSO_WAIT_RPT && st != FT8_QSO_WAIT_ROGER && st != FT8_QSO_WAIT_RR73)
        return false;
    // Unknown tone must mean "no hint" — a wrong hint is worse than none,
    // since it front-loads whatever happens to sit at that frequency.
    if (freq <= 0) return false;

    if (freq_hz_out) *freq_hz_out = freq;
    return true;
}
