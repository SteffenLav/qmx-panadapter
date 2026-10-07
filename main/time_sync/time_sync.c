#include "time_sync.h"
#include "rtc.h"
#include "settings.h"
#include "cat.h"
#include "wifi/wifi.h"

#include <string.h>
#include <stdlib.h>   // labs()
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"
#include "unit_gps/unit_gps.h"   // liveness wrapper + the module that owns freshness

static const char *TAG = "time_sync";

// UTC epoch bounds for sanity checks
// Bench switch for the OFFLINE (POTA) time path - see time_sync_notify_qmx().
// Ships as 0. Set to 1 to exercise the no-WiFi branch on a bench that has WiFi;
// actually disabling WiFi persists to NVS and strands the device offline with no
// way back except the Tab5's own drawer. Used to verify the fix for Don WB0LQW's
// lost-UTC report 2026-08-13.
#ifndef TIMESYNC_FORCE_OFFLINE_TEST
#define TIMESYNC_FORCE_OFFLINE_TEST 0
#endif

#define EPOCH_SANE_MIN  1700000000LL  // 2023-11-14
#define EPOCH_SANE_MAX  2208988800LL  // 2040-01-01 — anything beyond is garbage

// Fallback freshness window, used only when WiFi is down (see the bug note
// on time_sync_notify_qmx() below for why this can no longer be the primary
// "is SNTP still good" signal).
#define SNTP_FRESH_MS  (10LL * 60 * 1000)

// AUTO-DETECT tolerance (ms). Online, we mark a QMX as GPS-disciplined only when
// its tick agrees with SNTP this tightly - a real GPS second boundary lands
// within ~tens of ms - AND only when that agreement is not something we caused
// ourselves (see s_qmx_time_pushed below).
//
// ⚠ The original reasoning here was WRONG and produced a false "UTC(GPS)" on a
// radio with no GPS at all (operator's own bench unit, 2026-08-17). It claimed a
// push-set RTC "is only whole-second accurate" and so would miss this window.
// It is not: cat_set_qmx_time() sends TM<hhmmss>; at whatever moment the call
// happens, and the radio starts its second when it parses that - so the tick
// phase we induce is uniform in 0..1000 ms, and lands inside 300 ms a good third
// of the time on its own. The measured case agreed to 12 ms.
#define QMX_GPS_CONFIRM_MS 300

// How far off makes the radio's clock plainly ITS OWN again rather than the one
// we set. A QMX's software RTC is not persisted through a power cycle (it starts
// at 00:00), so a disagreement this large means our push is gone.
#define QMX_CLOCK_LOST_SEC 60

// FT8 auto-sync leash (OFFLINE only). When there is no SNTP/GPS reference, the
// FT8 consensus tracker (ft8_test.c) is the only time source, and it nudges the
// clock toward the on-air population timing each slot. This is the POSITION
// bound on that: the cumulative pull from the boot-RTC/QMX anchor may not exceed
// +/-this, so noise (or the ~560 ms RX-audio-latency chase, see
// apply_ft8_correction) can't drag the clock arbitrarily far. When SNTP/GPS IS
// up the FT8 auto-sync is disabled entirely (the clock stays on the accurate
// reference), so this leash only applies offline.
#define FT8_LEASH_MS      500

/* ⭐ IS THE DATE ITSELF TRUSTWORTHY? (Don WB0LQW, 2026-09-13)
 *
 * Every offline time source here gives only a TIME OF DAY - the QMX's TM;, the
 * QMX-GPS tick, and the manual HH:MM:SS set - and each pastes it onto
 * get_date_anchor(), which with no RTC and no internet is last_unix_time: the
 * last moment this unit had a good clock. The supercap RTC holds 30-40 h, so a
 * Tab5 left off "a couple of days" boots with the date it was last used and
 * logs every QSO under it. Don found his POTA log two days behind; his time of
 * day was perfect.
 *
 * So the date is verified only by something that actually knows it: SNTP, an
 * RTC that survived, or the operator (time_sync_set_date / confirm_date).
 * ui/date_confirm_modal.c asks while this is false. Logging is never blocked. */
static bool              s_date_verified     = false;

// Timestamps of the last accepted sync from each source; 0 = never.
static int64_t           s_last_qmx_sync_ms  = 0;
static int64_t           s_last_sntp_sync_ms = 0;
static time_sync_source_t s_source           = TIME_SOURCE_NONE;

// AUTO-DETECTED: is the connected QMX GPS-disciplined? Derived at CAT connect
// from whether its tick agrees tightly with SNTP (replaces the old manual
// "QMX has GPS" checkbox). The NVS qmx_gps field now just PERSISTS this so an
// offline/POTA session (no SNTP to re-verify) remembers the last verdict.
static bool              s_qmx_gps_confirmed = false;

// Have we push-set the connected radio's clock? Persisted, because the state it
// describes lives in the RADIO and outlives a Tab5 reboot - which is precisely
// how the false-GPS bug happened. qmx_sync_once() is careful to detect BEFORE
// pushing within one boot, but reflash the Tab5 with the radio left powered and
// the NEXT boot's detection measures a clock we set in the PREVIOUS one. Its
// agreement with us then says nothing about GPS, so it must not be counted as
// evidence. A clock we set cannot be a witness for itself.
static bool              s_qmx_time_pushed = false;

static void set_qmx_time_pushed(bool v)
{
    if (v == s_qmx_time_pushed) return;
    s_qmx_time_pushed = v;
    settings_set_qmx_time_pushed(v);
}

static void set_qmx_gps_confirmed(bool v)
{
    if (v == s_qmx_gps_confirmed) return;
    s_qmx_gps_confirmed = v;
    settings_set_qmx_gps(v);   // persist for offline continuity
    ESP_LOGI(TAG, "QMX GPS auto-detect: %s", v ? "CONFIRMED (GPS-disciplined)" : "not present");
}

bool time_sync_qmx_gps_confirmed(void) { return s_qmx_gps_confirmed; }

// The source actually MAINTAINING the clock right now, for the UI label - not
// the last one-off writer. A manual/FT8 nudge stamps s_source, but if SNTP or a
// confirmed GPS is up they are the ongoing authority, so report that instead.
// GPS is only a live reference while the RADIO CARRYING IT IS ATTACHED.
//
// s_qmx_gps_confirmed is restored from NVS at boot so an offline POTA start
// keeps GPS discipline without re-detecting - but on its own it says only
// "this QMX had GPS the last time we looked", not "there is a GPS clock here
// now". Reported by Don N2VGU (2026-08-09): his Tab5 showed UTC(GPS) with the
// QMX+ unplugged and WiFi connected, so the label claimed GPS accuracy while
// SNTP was actually keeping the clock. The Tab5 has no GPS chip of its own -
// if the radio is not there, neither is the GPS.
static bool gps_is_live(void)
{
    return s_qmx_gps_confirmed && cat_is_ready();
}

// The Unit GPS is its own liveness test - a satellite fix with a lock byte,
// independent of whether any radio is attached. Ranked FIRST: while it is
// live it outranks the QMX+ internal GPS because it is
// the only offline source here that also carries the DATE.
bool time_sync_unit_gps_is_live(void)
{
    return unit_gps_is_live();
}

time_sync_source_t time_sync_get_effective_source(void)
{
    if (time_sync_unit_gps_is_live())                   return TIME_SOURCE_UNIT_GPS;  // Unit GPS on PORT.A
    if (gps_is_live())                                  return TIME_SOURCE_QMX;       // QMX+ GPS
    if (wifi_is_connected() && wifi_time_is_valid())    return TIME_SOURCE_SNTP;
    return s_source;   // offline: FT8 / manual / RTC / naive-QMX
}

// Sum of every FT8-derived nudge (time_sync_apply_correction_ms*) applied
// since the last hard sync (SNTP/QMX/manual/RTC), in ms, sign-matched to
// apply_ft8_correction's delta_ms convention (positive = clock was fast,
// time subtracted). Lets a caller reconstruct "what would the clock read
// right now if FT8 had never nudged it" as current_time + this offset -
// used only for the panadapter waterfall's FT8-vs-SNTP slot-line overlay
// (debug/visualization, not used for any sync decision).
static int64_t           s_ft8_cum_offset_ms = 0;

time_sync_source_t time_sync_get_source(void) { return s_source; }
int64_t time_sync_get_ft8_offset_ms(void) { return s_ft8_cum_offset_ms; }

static bool epoch_is_sane(int64_t t)
{
    return t > EPOCH_SANE_MIN && t < EPOCH_SANE_MAX;
}

// Do we already hold a clock worth defending against a GPS-less QMX? Anything
// but "nothing" and "the QMX told us" counts: the Tab5 RTC, SNTP, an FT8-derived
// correction and a manual set are all better references than a radio RTC that
// restarts at 00:00. Deliberately requires the system clock to be sane too, so a
// stale s_source cannot veto a genuinely useful QMX reading.
static bool clock_is_trusted(void)
{
    if (!epoch_is_sane((int64_t)time(NULL))) return false;
    switch (s_source) {
    case TIME_SOURCE_UNIT_GPS:
    case TIME_SOURCE_RTC:
    case TIME_SOURCE_SNTP:
    case TIME_SOURCE_MANUAL:
    case TIME_SOURCE_FT8:
        return true;
    default:
        return false;   // NONE, or the QMX itself
    }
}

static const char *trusted_source_name(void)
{
    switch (s_source) {
    case TIME_SOURCE_UNIT_GPS: return "Unit-GPS";
    case TIME_SOURCE_RTC:    return "Tab5 RTC";
    case TIME_SOURCE_SNTP:   return "SNTP";
    case TIME_SOURCE_MANUAL: return "manual";
    case TIME_SOURCE_FT8:    return "FT8";
    case TIME_SOURCE_QMX:    return "QMX";
    default:                 return "none";
    }
}

// Only correct the radio when it is meaningfully wrong. Its RTC has 1 s
// resolution over CAT, so a couple of seconds of disagreement is just rounding.
#define QMX_PUSH_THRESHOLD_SEC 3

// Push UTC time-of-day to the QMX's onboard RTC so it stays in sync for
// no-WiFi (POTA) sessions. Skipped when the QMX has GPS discipline (it has
// better time than anything the Tab5 carries). Never called when the QMX is
// the *source* of the sync — only when Tab5 has a better clock.
/* ⛔ NEVER OVERWRITE A GPS RADIO'S CLOCK. Steffen's QMX+ sat at "not GPS" for
 * hours, 2026-10-07, and this function is why.
 *
 * The only gate used to be s_qmx_gps_confirmed - OUR OWN inference, from the
 * tick agreeing with SNTP. That makes a trap with no exit: detection fails for
 * any reason, so we push; cat_set_qmx_time() carries whole seconds only, so a
 * clock reading 19:16:01.9 pushes :01 and leaves the radio nearly a second
 * slow; the next tick is a second out and fails the test; so we push again.
 * The log caught it in one line - the radio's own GPS said 19:16:02 and we
 * wrote 19:16:01 into it. Nothing recovers from that except a power cycle,
 * because only then does the QMX drop its software clock and let its GPS
 * re-discipline it. That is exactly what the operator saw: power-cycle the
 * radio and it reads GPS immediately.
 *
 * So the gate is now the RADIO'S OWN STATEMENT that a GPS is permanently
 * fitted, which it answers from its own menu and which no inference of ours can
 * poison. And "it has not answered yet" is held separately from "it said no",
 * because they used to be the same false - and the first push goes out ~19 s
 * after boot on the SNTP notify, before the MM query has landed at ~21-51 s.
 *
 * ⚠ The whole-second truncation is still there for radios we DO push to. It can
 * leave any QMX up to a second slow. Not fixed here: doing it properly means
 * measuring the CAT write latency and aligning to a boundary, and this change
 * is about the radios we should never have been writing to at all. */
static void push_to_qmx(time_t utc)
{
    if (!cat_is_ready()) return;
    if (s_qmx_gps_confirmed) {
        ESP_LOGD(TAG, "QMX is GPS-disciplined — skipping Tab5→QMX time push");
        return;
    }
    if (!cat_qmx_gps_source_known()) {
        ESP_LOGI(TAG, "Tab5→QMX time push deferred - radio has not said yet "
                      "whether it has a GPS fitted");
        return;
    }
    if (cat_qmx_gps_source_internal()) {
        ESP_LOGI(TAG, "QMX reports a permanent GPS - NOT pushing our time to it, "
                      "whatever our own detection currently thinks");
        return;
    }
    struct tm tm_utc;
    gmtime_r(&utc, &tm_utc);
    esp_err_t err = cat_set_qmx_time(tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Tab5→QMX time push failed: 0x%x", err);
    } else {
        // Remember it: from here on, this radio's clock agreeing with ours is
        // our own doing and can never confirm GPS.
        set_qmx_time_pushed(true);
        ESP_LOGI(TAG, "Tab5→QMX time push: %02d:%02d:%02d UTC",
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    }
}

// Return a date anchor (UTC epoch of some recent day) from the best available
// source: current system clock (if sane) or NVS last-known timestamp.
static time_t get_date_anchor(void)
{
    time_t now = time(NULL);
    if (epoch_is_sane((int64_t)now)) return now;

    /* ONE FIELD, not the whole ~1 KB struct: this runs on time_sync_task,
     * whose stack is 3072 bytes, and an ESP_LOGW's vfprintf frame is most of
     * a kilobyte on its own. See the rule in settings.h. */
    uint32_t anchor = settings_get_last_unix_time();
    if (epoch_is_sane((int64_t)anchor)) return (time_t)anchor;

    ESP_LOGW(TAG, "No valid date anchor (NVS=0x%08lx) — using fallback 2023-11-14; time-of-day will be correct",
             (unsigned long)anchor);
    return (time_t)EPOCH_SANE_MIN;
}

// Sync priorities (highest first). The QMX-GPS case (operator sets the qmx_gps
// flag for a GPS-disciplined QMX+) REORDERS these - see time_sync_notify_qmx().
//
//   Plain QMX (no GPS):
//     1. SNTP        - wins when WiFi is up; authoritative internet time
//     2. Tab5 RTC    - boot seed (rtc_apply_to_system) before SNTP/QMX
//     3. QMX TM;     - offline fallback only (SNTP not fresh)
//     4. Manual      - always applied (POTA, no QMX GPS or SNTP)
//   The QMX internal RTC drifts freely with no GPS discipline, so trusting it
//   above SNTP would break FT8 timing when it's off - hence fallback-only.
//
//   QMX+ with GPS (qmx_gps flag set):
//     1. QMX-GPS (tick) - a primary standard that OUTRANKS SNTP. We don't take
//                      the whole-second TM; value (that's +/-1 s); instead
//                      cat_gps_tick_sync() catches the SECOND BOUNDARY (the flip
//                      N->N+1) and apply_gps_tick() phase-locks the clock to it,
//                      giving ~+/-25 ms, drift-free, WiFi-independent - better
//                      than our SNTP. Re-locked every 5 min by time_sync_task.
//     2. SNTP        - sanity reference: a genuine fix agrees with SNTP within
//                      QMX_GPS_SANITY_S; a gross disagreement = "no fix", keep
//                      SNTP (the only lock guard, since CAT has no lock readout).
//
// FT8 auto-sync (OFFLINE fallback only): when NO SNTP/GPS reference exists, a
// continuous damped nudge toward the band consensus keeps FT8 timing usable.
// When SNTP/GPS IS up it is DISABLED - the FT8 timing offset is dominated by
// ~560 ms of one-way RX audio latency (not a clock error), and our CAT-based TX
// has no matching latency, so letting it pull the clock only drags TX late.
// See apply_ft8_correction().

static void write_to_rtc_and_nvs(time_t utc, const char *source)
{
    struct tm tm_utc;
    gmtime_r(&utc, &tm_utc);
    if (!rtc_set_time(&tm_utc)) {
        ESP_LOGW(TAG, "%s: RTC write failed", source);
    }
    if (epoch_is_sane((int64_t)utc)) {
        settings_set_last_unix_time((uint32_t)utc);
    }
}

static void apply_and_persist(time_t utc, const char *source)
{
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(utc, source);
    s_ft8_cum_offset_ms = 0;  // hard sync: any prior FT8 nudge is now baked in

    struct tm tm_utc;
    gmtime_r(&utc, &tm_utc);
    ESP_LOGI(TAG, "Time set from %s: %04d-%02d-%02d %02d:%02d:%02d UTC",
             source,
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

// Priority 1: SNTP — always authoritative. Sets system clock, RTC, and NVS.
void time_sync_notify_sntp(time_t utc)
{
    apply_and_persist(utc, "SNTP");
    s_last_sntp_sync_ms = esp_timer_get_time() / 1000;
    s_source = TIME_SOURCE_SNTP;
    s_date_verified = true;
    settings_set_date_src(DATE_SRC_SNTP);
    push_to_qmx(utc);
}

/* A disagreement big enough to be worth saying out loud - the same 300 ms that
 * separates "agrees" from "does not agree" for a QMX tick. SNTP disagreement
 * is a LOG, never a veto here: the RMC status byte is the receiver telling us
 * it has a lock, where SNTP agreement was only ever an inference about someone
 * else's lock (#173). */
#define UNIT_GPS_SNTP_WARN_MS QMX_GPS_CONFIRM_MS

/* push_to_qmx() is a blocking CAT write of up to 200 ms from a non-poll
 * context, so a source that speaks every second must not call it every second.
 * Once when it takes over, then every 5 minutes - the same cadence the QMX
 * re-locked at. The skip rule inside push_to_qmx() is untouched: only a
 * GPS-disciplined radio is never pushed to. */
static time_t s_last_unit_push_utc = 0;
#define UNIT_GPS_PUSH_INTERVAL_S 300

// Priority 0: Unit GPS on PORT.A - a satellite clock that carries its own DATE,
// so nothing here asks get_date_anchor() what day it might be (Don WB0LQW's
// two-days-behind POTA log is the failure this path exists to end).
bool time_sync_notify_unit_gps(int year, int mon, int mday,
                               int h, int m, int s,
                               uint32_t frac_us, int64_t flip_us)
{
    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof(tm_utc));
    tm_utc.tm_year  = year - 1900;
    tm_utc.tm_mon   = mon - 1;
    tm_utc.tm_mday  = mday;
    tm_utc.tm_hour  = h;
    tm_utc.tm_min   = m;
    tm_utc.tm_sec   = s;
    tm_utc.tm_isdst = 0;
    // ESP-IDF runs with UTC as the default timezone, so mktime == timegm here
    // (the same assumption main/rtc/rtc.c documents).
    time_t t = mktime(&tm_utc);
    if (t < 0) {
        ESP_LOGW(TAG, "Unit-GPS date/time did not convert - ignoring");
        return false;
    }

    /* Where the clock actually is, in microseconds.
     *
     * t*1e6 + frac is UTC AT THE MOMENT THE SENTENCE DESCRIBES. Carrying it
     * forward by the time since the flip-stamp puts us at "now": when the
     * sentence has a fractional second that is exact; when it does not, the
     * flip stamp IS the boundary (arrival of the N->N+1 sentence), which is
     * precisely what apply_gps_tick() does for a QMX+ tick. With neither - the
     * first lock - this is a whole-second apply and claims no phase at all. */
    int64_t carry_us = (flip_us > 0) ? (esp_timer_get_time() - flip_us) : 0;
    if (carry_us < 0) carry_us = 0;
    int64_t utc_now_us = (int64_t)t * 1000000LL + (int64_t)frac_us + carry_us;
    time_t  utc_now    = (time_t)(utc_now_us / 1000000LL);

    if (!epoch_is_sane((int64_t)utc_now)) {
        ESP_LOGW(TAG, "Unit-GPS time out of range (%lld) - ignoring", (long long)utc_now);
        return false;
    }

    // Log a disagreement with the internet, but apply the satellite anyway.
    if (wifi_is_connected() && wifi_time_is_valid()) {
        struct timeval sys;
        gettimeofday(&sys, NULL);
        int64_t sys_us = (int64_t)sys.tv_sec * 1000000LL + sys.tv_usec;
        int64_t d_ms   = llabs(utc_now_us - sys_us) / 1000;
        if (d_ms > UNIT_GPS_SNTP_WARN_MS) {
            ESP_LOGW(TAG, "Unit-GPS off SNTP by %lld ms - applying Unit GPS anyway "
                          "(RMC status is the lock indication)", (long long)d_ms);
        }
    }

    struct timeval tv = { .tv_sec = utc_now,
                          .tv_usec = (suseconds_t)(utc_now_us % 1000000LL) };
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(utc_now, "Unit-GPS");
    s_ft8_cum_offset_ms = 0;   // hard sync: any prior FT8 nudge is now baked in

    s_date_verified = true;    // the DATE came from the satellite - idempotent
    /* ⛔ ADDED IN THE MERGE, not on either branch. date_src landed on main
     * (royord #18) on the same day this branch was written, so this path
     * set the date without recording WHERE it came from - and the next
     * boot would have distrusted a perfectly good satellite date and asked
     * the operator anyway. Two changes to the same semantics, merged
     * cleanly by git because they touch different lines. */
    settings_set_date_src(DATE_SRC_UNIT_GPS);
    s_source = TIME_SOURCE_UNIT_GPS;

    struct tm shown;
    gmtime_r(&utc_now, &shown);
    ESP_LOGI(TAG, "Time set from Unit-GPS: %04d-%02d-%02d %02d:%02d:%02d.%03d UTC%s",
             shown.tm_year + 1900, shown.tm_mon + 1, shown.tm_mday,
             shown.tm_hour, shown.tm_min, shown.tm_sec, (int)(tv.tv_usec / 1000),
             flip_us > 0 ? " phase-locked" : " (whole second)");

    if (s_last_unit_push_utc == 0 ||
        (utc_now - s_last_unit_push_utc) >= UNIT_GPS_PUSH_INTERVAL_S) {
        push_to_qmx(utc_now);
        s_last_unit_push_utc = utc_now;
    }
    return true;
}

/* Dev only: a bench with WiFi always has a verified date, so without this the
 * question could never be seen before it reaches a park. Cleared by the
 * operator answering, exactly like the real case. */
static bool s_dev_force_unverified = false;

void time_sync_dev_force_date_unverified(void)
{
    s_dev_force_unverified = true;
    s_date_verified = false;
    ESP_LOGW(TAG, "DEV: date forced to unverified (ignores SNTP until answered)");
}

/* ⭐ THE SUPERCAP RTC SAYING "valid" IS NOT THE SAME AS THE DATE BEING RIGHT.
 *
 * royord, #18, 2026-10-07: he flashed the M5 UserDemo (whose default RTC date
 * is 1901, and he nudged it to 1904), then flashed this firmware back. The RTC
 * reported valid, so boot marked the date verified and the question was never
 * asked - the firmware was certain about a date that was over a century out.
 *
 * rtc_is_valid() only means the supercap never browned out. It says nothing
 * about WHO wrote the value, and another firmware writing the RTC leaves our
 * NVS record untouched. So the RTC is believed only when our own record backs
 * it up, on all three counts:
 *
 *   1. We recorded a date source we trust (SNTP, a GPS fix, or the operator
 *      reading it off the screen). A fresh flash has none - it asks, which is
 *      the right default.
 *   2. The RTC has not gone BACKWARDS past that record. This is what catches
 *      royord: our record held a 2026 time, the RTC came up in 1904.
 *   3. It has not run so far past the record that nobody has watched it. A
 *      Tab5 that sat in a drawer for months gets asked once - which is Don
 *      WB0LQW's original fault (two days off, two days wrong in the log), just
 *      at a longer scale.
 *
 * The cost of a false NO is one question. The cost of a false YES is a day of
 * QSOs logged under the wrong date, which has now happened twice to real
 * operators. The asymmetry decides every judgement call here.
 */
#define DATE_TRUST_MAX_UNATTENDED_S  (30LL * 24 * 3600)

static bool rtc_date_is_trustworthy(time_t rtc_now)
{
    const uint8_t src = settings_get_date_src();
    if (src != DATE_SRC_OPERATOR && src != DATE_SRC_SNTP &&
        src != DATE_SRC_QMX_GPS  && src != DATE_SRC_UNIT_GPS) {
        ESP_LOGW(TAG, "RTC date NOT trusted: no recorded source (src=%u)", (unsigned)src);
        return false;
    }

    const int64_t last = (int64_t)settings_get_last_unix_time();
    if (last == 0) {
        ESP_LOGW(TAG, "RTC date NOT trusted: source %u but no recorded time", (unsigned)src);
        return false;
    }

    const int64_t now = (int64_t)rtc_now;
    if (now < last) {
        ESP_LOGW(TAG, "RTC date NOT trusted: clock went BACKWARDS %lld s past our "
                      "own record - something else wrote this RTC",
                 (long long)(last - now));
        return false;
    }
    if (now - last > DATE_TRUST_MAX_UNATTENDED_S) {
        ESP_LOGW(TAG, "RTC date NOT trusted: %lld days since our last record",
                 (long long)((now - last) / 86400));
        return false;
    }

    ESP_LOGI(TAG, "RTC date trusted: src=%u, %lld s since our record",
             (unsigned)src, (long long)(now - last));
    return true;
}

bool time_sync_date_verified(void)
{
    if (s_dev_force_unverified) return s_date_verified;
    return s_date_verified || (wifi_is_connected() && wifi_time_is_valid());
}

void time_sync_confirm_date(void)
{
    if (!s_date_verified) ESP_LOGI(TAG, "date confirmed by the operator");
    s_date_verified = true;
    settings_set_date_src(DATE_SRC_OPERATOR);
    s_dev_force_unverified = false;
}

// Replace the DATE, keeping the current time of day to the microsecond - the
// operator is correcting the day, and the seconds they may have just set by
// hold-and-release must not move.
bool time_sync_set_date(int year, int mon, int mday)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t tod = (int64_t)tv.tv_sec % 86400;
    if (tod < 0) tod += 86400;
    struct tm tm_d = { .tm_year = year - 1900, .tm_mon = mon - 1, .tm_mday = mday, .tm_isdst = 0 };
    time_t day = mktime(&tm_d);   // ESP-IDF runs in UTC, so mktime == timegm
    time_t utc = day + (time_t)tod;
    if (day == (time_t)-1 || !epoch_is_sane((int64_t)utc)) {
        ESP_LOGW(TAG, "date %04d-%02d-%02d rejected", year, mon, mday);
        return false;
    }
    tv.tv_sec = utc;
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(utc, "date");
    s_date_verified = true;
    s_dev_force_unverified = false;
    settings_set_date_src(DATE_SRC_OPERATOR);
    ESP_LOGI(TAG, "date set by the operator: %04d-%02d-%02d (time of day kept)", year, mon, mday);
    return true;
}

// Priority 3: QMX TM; time-of-day — offline fallback only.
//
// BUG (found 2026-06-26, field report): this used to gate purely on "SNTP
// synced within the last SNTP_FRESH_MS (10 min)" — but ESP-IDF's SNTP client
// (ESP_NETIF_SNTP_DEFAULT_CONFIG) only re-fires its callback roughly once an
// hour once synced, so s_last_sntp_sync_ms stops updating ~1 minute after
// boot and "looks stale" to this 10-minute check for the rest of every hour,
// even though the network clock is perfectly healthy and WiFi never
// dropped. Every 5-minute periodic QMX poll after that point would then
// silently overwrite the system clock with the QMX's free-running,
// non-GPS-disciplined RTC — caught live mid-QSO as the FT8 slot clock
// jumping ~2 s off after a "Time set from QMX" log line, despite WiFi
// staying connected throughout.
//
// Fix: trust WiFi connectivity + "has SNTP ever synced" (wifi_is_connected()
// + wifi_time_is_valid(), which never resets while the STA interface stays
// up) as the primary "is SNTP still authoritative" signal, matching the
// documented priority ("SNTP always wins when WiFi is up"). The time-window
// check is now only a fallback for the case WiFi itself is reported up but
// the bits haven't been observed yet (startup race).
bool time_sync_notify_qmx(int h, int m, int s)
{
    // Naive whole-second QMX fallback for a NON-GPS QMX (a GPS one is handled by
    // the precise tick path - apply_gps_tick - and never reaches here, since
    // qmx_sync_once only falls through to the naive query when the tick didn't
    // apply). Offline fallback only: SNTP wins whenever it's fresh.
    time_t  anchor    = get_date_anchor();
    int64_t day_start = ((int64_t)anchor / 86400) * 86400;
    time_t  utc       = (time_t)(day_start + h * 3600 + m * 60 + s);

    int64_t now_ms    = esp_timer_get_time() / 1000;
    bool wifi_sntp_ok = wifi_is_connected() && wifi_time_is_valid();
#if TIMESYNC_FORCE_OFFLINE_TEST
    // TEMP: pretend we are offline so the POTA path can be exercised on a bench
    // that has WiFi. Disabling WiFi for real would persist to NVS and strand the
    // device offline with no way back except the Tab5's own drawer.
    wifi_sntp_ok = false;
    s_last_sntp_sync_ms = 0;
#endif
    bool sntp_fresh   = wifi_sntp_ok ||
                        (s_last_sntp_sync_ms > 0 &&
                         (now_ms - s_last_sntp_sync_ms) < SNTP_FRESH_MS);
    s_last_qmx_sync_ms = now_ms;

    if (sntp_fresh) {
        ESP_LOGD(TAG, "QMX TM; %02d:%02d:%02d - SNTP fresh (WiFi up=%d), skipping",
                 h, m, s, (int)wifi_sntp_ok);
        return false;
    }

    // A QMX WITHOUT GPS is not a time reference. Its RTC free-runs and comes up
    // at 00:00 after any power-off, so offline it must never overwrite a clock we
    // already trust. The Tab5's supercap RTC, set from SNTP before leaving home,
    // holds seconds-accurate UTC for 30-40 h - that is the entire basis of the
    // offline POTA workflow in the manual.
    //
    // Don WB0LQW lost his accurate UTC to exactly this: RTC good, turn the radio
    // on in the field, and the first poll pulled the clock back to the QMX's
    // 00:00. Offline SNTP is NEVER fresh, so the guard above cannot help him -
    // it only ever protected the WiFi case.
    //
    // The right direction is the opposite one, which is what he asked for: push
    // OUR time to the radio. Only done when the radio is actually wrong, so a
    // healthy pair does not trade CAT writes every five minutes.
    if (!s_qmx_gps_confirmed && clock_is_trusted()) {
        time_t  now_utc = time(NULL);
        struct tm tm_now;
        gmtime_r(&now_utc, &tm_now);
        int ours   = tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec;
        int theirs = h * 3600 + m * 60 + s;
        int off    = ours - theirs;
        if (off < 0) off = -off;
        if (off > 43200) off = 86400 - off;   // wrap at midnight

        ESP_LOGI(TAG, "QMX TM; %02d:%02d:%02d ignored - radio has no GPS and our "
                      "clock is trusted (%s, %d s apart)",
                 h, m, s, trusted_source_name(), off);
        if (off > QMX_PUSH_THRESHOLD_SEC) push_to_qmx(now_utc);
        return false;
    }

    apply_and_persist(utc, "QMX");
    s_source = TIME_SOURCE_QMX;
    return true;
}

// FT8-signal-derived correction. delta_ms > 0 means clock is fast.
// Apply an FT8-derived clock nudge. `leash` = enforce the FT8_LEASH_MS position
// bound (auto-sync path); the manual-Apply path passes false so an operator
// override always lands in full. *out_utc (if non-NULL) returns the resulting
// clock. Returns the delta ACTUALLY applied (after leashing) - 0 if the leash
// blocked it - which is what the modal/log show as the real "nudge".
static int apply_ft8_correction(int delta_ms, bool leash, time_t *out_utc)
{
    int applied = delta_ms;

    if (leash) {   // leash == the auto-sync path (manual Apply passes false)
        // Same correction: a remembered GPS verdict is not an absolute
        // reference when the radio is unplugged, and suppressing the FT8
        // nudge on the strength of it would leave such a unit with NO
        // discipline at all.
        bool ref_ok = (wifi_is_connected() && wifi_time_is_valid())
                      || gps_is_live()
                      || time_sync_unit_gps_is_live();
        if (ref_ok) {
            // A real absolute reference (SNTP or GPS) exists -> do NOT let FT8
            // touch the clock. Root-caused 2026-07-18: the FT8 timing offset is
            // dominated by ~560 ms of ONE-WAY RX audio latency (QMX SDR + USB
            // buffering), NOT a clock error. Our FT8 TX is CAT tone-stepping
            // (no audio pipeline, ~ms latency), so pulling the clock to zero
            // that RX latency would drag our TX ~500 ms LATE while GPS/SNTP
            // would keep it correct - exactly why cum pinned at the leash. So
            // the FT8 auto-sync is now the OFFLINE fallback only; when a real
            // reference is up, the clock stays on it.
            if (out_utc) *out_utc = time(NULL);
            return 0;
        }
        // Offline: FT8 is the only reference. Bound the cumulative pull to
        // +/-FT8_LEASH_MS against the boot-RTC/QMX anchor so noise (or the same
        // RX-latency chase) can't drag the clock arbitrarily far. Moving BACK
        // toward the anchor is always allowed in full.
        int64_t cum     = s_ft8_cum_offset_ms;
        int64_t new_cum = cum + delta_ms;
        if      (new_cum >  FT8_LEASH_MS) applied = (int)((int64_t)FT8_LEASH_MS  - cum);
        else if (new_cum < -FT8_LEASH_MS) applied = (int)((int64_t)-FT8_LEASH_MS - cum);
    }

    if (applied == 0) {              // leash blocked it entirely - clock untouched
        if (out_utc) *out_utc = time(NULL);
        return 0;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec - (int64_t)applied * 1000LL;
    tv.tv_sec  = (time_t)(us / 1000000LL);
    tv.tv_usec = (suseconds_t)(us % 1000000LL);
    if (tv.tv_usec < 0) { tv.tv_sec--; tv.tv_usec += 1000000; }
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(tv.tv_sec, "FT8");
    s_source = TIME_SOURCE_FT8;
    s_ft8_cum_offset_ms += applied;
    ESP_LOGI(TAG, "FT8 timing correction: %+d ms (cum %+lld ms)", applied, (long long)s_ft8_cum_offset_ms);
    if (out_utc) *out_utc = tv.tv_sec;
    return applied;
}

void time_sync_apply_correction_ms(int delta_ms)   // manual Apply - never leashed
{
    time_t utc;
    apply_ft8_correction(delta_ms, false, &utc);
    push_to_qmx(utc);
}

int time_sync_apply_correction_ms_quiet(int delta_ms)   // auto-sync - leashed
{
    return apply_ft8_correction(delta_ms, true, NULL);
}

// Priority 5 (last resort): manual entry from user (rare POTA offline use).
void time_sync_set_manual(int year, int mon, int mday, int h, int m, int s)
{
    struct tm tm_utc = {
        .tm_year  = year - 1900,
        .tm_mon   = mon - 1,
        .tm_mday  = mday,
        .tm_hour  = h,
        .tm_min   = m,
        .tm_sec   = s,
        .tm_isdst = 0,
    };
    // mktime() is safe: ESP-IDF runs with UTC as the default timezone
    time_t utc = mktime(&tm_utc);
    if (!epoch_is_sane((int64_t)utc)) {
        ESP_LOGW(TAG, "manual time rejected (year=%d looks wrong)", year);
        return;
    }
    apply_and_persist(utc, "manual");
    s_source = TIME_SOURCE_MANUAL;
    push_to_qmx(utc);
}

void time_sync_mark_ft8(void)
{
    s_source = TIME_SOURCE_FT8;
}

void time_sync_mark_qmx(void)
{
    s_source = TIME_SOURCE_QMX;
}

void time_sync_push_to_qmx(void)
{
    push_to_qmx(time(NULL));
}

// Phase-lock the system clock to a GPS second boundary caught by
// cat_gps_tick_sync(): at flip_us (esp_timer), true UTC was exactly h:m:s.000.
// Carry it forward by the elapsed micros so the SUB-SECOND phase is right - this
// is what turns GPS-over-CAT from a +/-1 s whole-second guess into a genuine
// +/-25 ms, drift-free reference (better than our SNTP), justifying GPS-primary.
// Returns true if the tick was accepted and applied (a GPS-quality fix); false
// if rejected (not GPS-disciplined). Online, "accepted" means a TIGHT agreement
// with SNTP (QMX_GPS_CONFIRM_MS) - that tightness is exactly what distinguishes
// a real GPS second boundary (~tens of ms) from a non-GPS RTC. Offline (no SNTP
// to check), accept only for a QMX we already confirmed as GPS (persisted).
static bool apply_gps_tick(int h, int m, int s, int64_t flip_us)
{
    time_t  anchor      = get_date_anchor();
    int64_t day_start   = ((int64_t)anchor / 86400) * 86400;
    int64_t utc_flip_us = ((int64_t)day_start + h * 3600 + m * 60 + s) * 1000000LL;  // .000 at flip
    int64_t elapsed_us  = esp_timer_get_time() - flip_us;
    int64_t utc_now_us  = utc_flip_us + elapsed_us;
    time_t  utc_now     = (time_t)(utc_now_us / 1000000LL);

    if (!epoch_is_sane((int64_t)utc_now)) {
        ESP_LOGW(TAG, "GPS-tick time out of range - ignoring");
        return false;
    }

    if (wifi_is_connected() && wifi_time_is_valid()) {
        struct timeval sys;
        gettimeofday(&sys, NULL);
        int64_t sys_us = (int64_t)sys.tv_sec * 1000000LL + sys.tv_usec;
        int64_t d_ms   = llabs(utc_now_us - sys_us) / 1000;
        if (d_ms > 43200000) d_ms = 86400000 - d_ms;   // midnight-wrap safe
        if (d_ms > QMX_GPS_CONFIRM_MS) {
            // Far enough off that our own push cannot be what we are looking at.
            // A QMX loses its software RTC on power-down, so this is the moment
            // the radio's clock becomes its own again - and the only moment a
            // later-fitted GPS could be detected. Forget the push.
            if (d_ms > (int64_t)QMX_CLOCK_LOST_SEC * 1000) set_qmx_time_pushed(false);
            ESP_LOGW(TAG, "QMX tick %02d:%02d:%02d off SNTP by %lldms - not GPS-disciplined",
                     h, m, s, (long long)d_ms);
            return false;
        }
        // Tight agreement - but if we are the reason for it, it proves nothing.
        // UNLESS the radio itself says it has a GPS permanently fitted, which is
        // its own answer rather than our inference and so settles it outright
        // (#174). This is what stops a genuine QMX+ we have pushed to from being
        // stuck as "not GPS" until its clock is next seen unset.
        if (s_qmx_time_pushed && !cat_qmx_gps_source_internal()) {
            ESP_LOGW(TAG, "QMX tick %02d:%02d:%02d agrees to %lldms, but WE set this "
                          "radio's clock - not treating that as GPS",
                     h, m, s, (long long)d_ms);
            return false;
        }
    } else if (!s_qmx_gps_confirmed) {
        return false;   // offline + never confirmed GPS -> don't trust a stray RTC
    }

    struct timeval tv = { .tv_sec = utc_now, .tv_usec = (suseconds_t)(utc_now_us % 1000000LL) };
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(utc_now, "QMX-GPS");
    s_ft8_cum_offset_ms = 0;
    s_source = TIME_SOURCE_QMX;
    ESP_LOGI(TAG, "Time set from QMX-GPS(tick): %02d:%02d:%02d.%03d UTC phase-locked (%lldms since flip)",
             h, m, s, (int)(tv.tv_usec / 1000), (long long)(elapsed_us / 1000));
    return true;
}

/* ⭐ GP GIVES THE DATE AND THE GPS ANSWER. IT DOES NOT GIVE THE PHASE.
 *
 * ⛔ THIS IS THE SECOND VERSION. The first one applied GP as a phase-locked
 * time reference, on the reasoning that GP reads the RECEIVER while TM; reads
 * the radio's free-running clock. The reasoning was right about provenance and
 * wrong about timing, and the bench said so within the hour.
 *
 * Measured 2026-10-07 against the PC clock (itself 60 ms off pool.ntp.org, so
 * not the term that matters), three runs, QMX+ on 1_04_010:
 *
 *     GP second boundary:  +998, +863, +991 ms LATE vs UTC
 *     TM second boundary:  +102,   -4,  -52 ms vs UTC
 *
 * GP reports the PREVIOUS second. That is ordinary NMEA behaviour - a receiver
 * emits the sentence for second N during second N+1 - and it means the radio's
 * own clock, disciplined at power-on and then free-running, is the better
 * PHASE reference by an order of magnitude. Applying GP as a boundary left the
 * bench Tab5 1.25 s slow, which FT8 slot alignment would have paid for.
 *
 * Not poll load: the lag is identical at 50 ms and 200 ms between polls.
 * Not a constant to subtract either: 863-998 ms over three runs is not clean
 * enough to call it exactly one second, and it crept ~10 ms per second within
 * single bursts.
 *
 * So GP is used for the two things it IS authoritative about:
 *
 *   1. THE DATE. TM; never carried one, which is the whole offline half of
 *      royord's #18. A date is a whole-day quantity; a 1 s lag cannot touch it
 *      except within 1 s of midnight, where the clock is re-read every 5
 *      minutes anyway.
 *   2. THE GPS ANSWER. A GP reply that parses comes from the receiver, which
 *      nothing we do can write. That settles provenance outright - no SNTP
 *      agreement test, no s_qmx_time_pushed guard, works offline.
 *
 * The clock itself is left to the TM path below, EXCEPT when there is no
 * usable clock at all (insane epoch, or a date that disagrees with the
 * satellite). Then a whole-second GP apply is plainly better than nothing, and
 * it says so in the log rather than claiming a phase it does not have.
 */
static bool apply_qmx_gp(int y, int mo, int d, int h, int mi, int s, int64_t flip_us)
{
    struct tm tm_utc = {0};
    tm_utc.tm_year = y - 1900;
    tm_utc.tm_mon  = mo - 1;
    tm_utc.tm_mday = d;
    tm_utc.tm_hour = h;
    tm_utc.tm_min  = mi;
    tm_utc.tm_sec  = s;
    tm_utc.tm_isdst = 0;
    time_t gp_utc = mktime(&tm_utc);   // IDF runs UTC, so mktime == timegm
    if (gp_utc < 0 || !epoch_is_sane((int64_t)gp_utc)) {
        ESP_LOGW(TAG, "QMX-GP date/time out of range - ignoring");
        return false;
    }

    /* The receiver answered, so this radio has a GPS. Its own answer, not our
     * inference from a clock we might have set ourselves. */
    set_qmx_gps_confirmed(true);
    s_date_verified = true;
    settings_set_date_src(DATE_SRC_QMX_GPS);

    /* Is the clock we are already running good enough to keep its phase? */
    time_t    now = time(NULL);
    struct tm now_tm;
    gmtime_r(&now, &now_tm);
    bool have_clock = epoch_is_sane((int64_t)now);
    bool same_day   = have_clock &&
                      now_tm.tm_year + 1900 == y &&
                      now_tm.tm_mon  + 1    == mo &&
                      now_tm.tm_mday        == d;

    if (same_day) {
        /* Date confirmed; phase stays with TM;. Nothing to write. */
        ESP_LOGI(TAG, "QMX-GP: date %04d-%02d-%02d confirmed from the receiver "
                      "(GPS confirmed; phase left to TM;)", y, mo, d);
        return true;
    }

    /* No usable clock, or the satellite disagrees about the DAY. Take GP's
     * whole second - ~1 s late, and labelled as such. The next TM tick fixes
     * the phase; nothing else would fix the date. */
    int64_t carry_us = esp_timer_get_time() - flip_us;
    if (carry_us < 0) carry_us = 0;
    int64_t utc_us = (int64_t)gp_utc * 1000000LL + carry_us;
    struct timeval tv = { .tv_sec  = (time_t)(utc_us / 1000000LL),
                          .tv_usec = (suseconds_t)(utc_us % 1000000LL) };
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(tv.tv_sec, "QMX-GP");
    s_ft8_cum_offset_ms = 0;
    s_source = TIME_SOURCE_QMX;
    ESP_LOGW(TAG, "Time set from QMX-GP: %04d-%02d-%02d %02d:%02d:%02d UTC - "
                  "WHOLE SECOND ONLY, GP runs ~1 s late; TM; will set the phase",
             y, mo, d, h, mi, s);
    return true;
}

/* One attempt at the GP path. Costs up to ~1.3 s while it brackets a second
 * boundary, and holds the CAT poll for that long. Returns false - cheaply - on
 * a radio older than 1.04_004, and after the full 1.3 s on a radio with no fix.
 *
 * True means "the receiver answered": the date and the GPS verdict are settled.
 * It does NOT mean the clock was set, and the caller must still run the TM
 * tick for the phase. See apply_qmx_gp() for the measurement behind that. */
bool time_sync_try_qmx_gp(void)
{
    int y, mo, d, h, mi, s;
    int64_t flip_us;
    if (cat_gps_gp_sync(&y, &mo, &d, &h, &mi, &s, &flip_us) != ESP_OK) return false;
    return apply_qmx_gp(y, mo, d, h, mi, s, flip_us);
}

// True once qmx_sync_once() has made its FIRST determination for the current
// QMX - no longer gates whether detection retries (see the 2026-09-23 fix
// note below), only whether push_to_qmx() has already seeded a non-GPS
// radio's RTC once. Reset by a reboot (static init) or time_redetect().
static bool s_qmx_detect_done = false;

void time_sync_force_redetect(void)
{
    s_qmx_detect_done = false;
    ESP_LOGW(TAG, "GPS auto-detect re-armed - will run on the next periodic pass");
}

// QMX time sync + GPS auto-detection (replaces the manual flag). Detection
// runs on the QMX's OWN clock and requires SNTP as ground truth. A GPS QMX's
// tick agrees tightly (within QMX_GPS_CONFIRM_MS) -> confirmed; a small/unset
// QMX is far off -> rejected, and we push our time to set its RTC.
//
// ⚠ Ordering within one boot is NOT sufficient protection, though this comment
// used to say it was ("happens BEFORE any Tab5->QMX push, so a push cannot
// masquerade as GPS"). s_qmx_detect_done is reset by a TAB5 reboot; the clock we
// pushed lives in the RADIO, which is not rebooted with us. Reflash the Tab5 with
// the QMX left powered and this "first" detection reads a clock we set in an
// earlier session. That is a real false positive, seen on a GPS-less bench unit.
// The durable guard is s_qmx_time_pushed, which crosses boots the same way the
// radio's clock does.
//
// ⛔ WAS ONE-SHOT, NOT ANYMORE - 2026-09-23. The tight-agreement test used to
// run exactly once (~15-45 s after boot) and latch its verdict for the rest of
// the session: a fail was permanent until a reboot or the time_redetect escape
// hatch. Measured on the bench: a QMX+ with a genuine GPS fix, moved/settling
// right after boot, missed the 300 ms window by ~430 ms at the one-shot's
// 45.8 s mark and then sat at "not GPS-disciplined" for 17+ minutes with the
// fix long since solid - nothing ever checked again. Steffen OZ1LAV and John
// Schindler (W5JSS) both hit this on real GPS-equipped units. Retrying is
// safe: apply_gps_tick()'s own s_qmx_time_pushed guard already stops a Tab5-
// pushed clock from being mistaken for GPS on any later attempt, same as the
// first one, so nothing here can produce a false positive by trying again.
static void qmx_sync_once(void)
{
    int h, m, s;
    int64_t flip_us;
    bool sntp_up = wifi_is_connected() && wifi_time_is_valid();

    /* ⭐ GP FIRST, FOR THE DATE AND THE GPS VERDICT - NOT FOR THE CLOCK.
     * It settles whether this radio has a GPS from the radio's own answer
     * instead of inferring it from a clock we may have set, and it carries the
     * date that TM; never did. The PHASE still comes from the TM tick below:
     * GP's second boundary measures ~1 s late (see apply_qmx_gp()). */
    bool gp_answered = time_sync_try_qmx_gp();
    if (gp_answered) {
        s_qmx_detect_done = true;   // settled, and not by inference
        sntp_up = wifi_is_connected() && wifi_time_is_valid();   // GP may have stepped the clock
    }

    // --- Auto-detect (needs SNTP to compare against), retried every periodic
    // pass until confirmed. push_to_qmx() still fires only on the very first
    // attempt - later attempts leave seeding the radio's RTC to the steady-
    // state fallback below, which already runs every pass regardless. ---
    if (!s_qmx_gps_confirmed && sntp_up && !gp_answered) {
        bool gps = (cat_gps_tick_sync(&h, &m, &s, &flip_us) == ESP_OK) &&
                   apply_gps_tick(h, m, s, flip_us);   // tight-agreement test inside
        set_qmx_gps_confirmed(gps);
        if (!s_qmx_detect_done) {
            s_qmx_detect_done = true;
            if (!gps) push_to_qmx(time(NULL));   // non-GPS: set its own RTC (once)
        }
        if (gps) return;                         // GPS confirmed + applied
    }

    // --- Steady state ---
    if (s_qmx_gps_confirmed) {
        if (cat_gps_tick_sync(&h, &m, &s, &flip_us) == ESP_OK &&
            apply_gps_tick(h, m, s, flip_us))
            return;   // re-locked to the GPS beat
    }
    // Not GPS (or tick missed, or offline-undetected): naive whole-second
    // fallback - applies only when SNTP isn't fresh (see time_sync_notify_qmx).
    if (cat_query_qmx_time(&h, &m, &s) == ESP_OK) {
        time_sync_notify_qmx(h, m, s);
    }
}

// Background task: initial QMX sync at CAT connect, then every 5 minutes.
// Covers Panadapter mode; ft8_task handles its own initial sync in FT8 mode.
static void time_sync_task(void *arg)
{
    // 15 s head start for ft8_task and CAT handshake before we query TM;
    vTaskDelay(pdMS_TO_TICKS(15000));

    const int MAX_WAIT_S = 300;
    int waited = 15;
    while (!cat_is_ready() && waited < MAX_WAIT_S) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        waited += 5;
    }

    if (cat_is_ready()) {
        // Auto-detect GPS + sync. qmx_sync_once() does the Tab5→QMX push itself,
        // AFTER detecting on the QMX's own clock (so a push can't fake GPS).
        qmx_sync_once();
    } else {
        ESP_LOGW(TAG, "CAT not ready after %ds — QMX time sync deferred to periodic", MAX_WAIT_S);
    }

    // Re-sync every 5 minutes (re-locks to the GPS beat / catches GPS lock events)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(300000));
        if (!cat_is_ready()) continue;
        qmx_sync_once();
    }
}

// Priority 2: Tab5 RTC — applied immediately at boot before QMX/SNTP are available.
void time_sync_init(i2c_master_bus_handle_t bus)
{
    if (rtc_init(bus) != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed — supercap RTC not available");
    } else if (rtc_is_valid()) {
        if (rtc_apply_to_system()) {
            s_source = TIME_SOURCE_RTC;
            /* ⛔ WAS: s_date_verified = true, "the supercap held, so the date is
             * the one it kept". The supercap holding only means nobody cut the
             * power - it does not mean the value is ours. See
             * rtc_date_is_trustworthy() above and royord, #18. */
            s_date_verified = rtc_date_is_trustworthy(time(NULL));
        } else {
            ESP_LOGW(TAG, "RTC read failed despite valid flag");
        }
    } else {
        ESP_LOGI(TAG, "RTC not valid (supercap dead or first boot) — waiting for QMX/SNTP sync");
    }

    // Seed the GPS verdict from the persisted auto-detection (for an offline
    // boot with no SNTP to re-verify); a fresh online detection overrides it.
    qmx_settings_t icfg;
    settings_load_all(&icfg);
    s_qmx_gps_confirmed = icfg.qmx_gps;
    s_qmx_time_pushed   = icfg.qmx_time_pushed;

    // Discard any verdict reached by the old, broken test, and assume we had set
    // this radio's clock. Both halves are deliberate:
    //
    //  - The stored verdict cannot be trusted: the test that produced it accepted
    //    our own push as proof of GPS. Keeping it would suppress the time pushes
    //    the radio actually needs, and be believed offline where there is no SNTP
    //    to re-check it.
    //  - We have no record of whether we pushed (the flag is new), and a one-shot
    //    phase comparison cannot tell a GPS tick from a clock we set. Assuming we
    //    pushed is the safe direction: being wrong costs a genuine GPS owner the
    //    "GPS" label while SNTP still keeps their clock correct, whereas the other
    //    way round we would keep asserting GPS accuracy we do not have.
    //
    // ⚠ Cost of that choice, and it is a real limitation: a QMX+ whose GPS we had
    // already pushed to will not re-confirm until its clock is next seen unset.
    // The clean removal is to ask the radio instead of inferring - "GPS source" in
    // its GPS & Ser. Ports menu reads QMX+ Internal for a permanently fitted GPS,
    // and MM can Get it over CAT. Not done here; see TODO.
    if (s_qmx_gps_confirmed) {
        ESP_LOGW(TAG, "stored QMX-GPS verdict discarded: it could have come from "
                      "measuring our own time push - re-detecting");
        set_qmx_gps_confirmed(false);
        set_qmx_time_pushed(true);
    }

    // 3072 -> 6144: a qmx_settings_t local (icfg) on the tightest stack in
    // this file. qmx_settings_t grew ~1350 B total this session (#pwrcal) -
    // generous this time, not incremental, after a +1024 bump undershot on
    // the same bug class elsewhere (sd_archive).
    psram_task_create(time_sync_task, "time_sync", 8192, NULL, 4, tskNO_AFFINITY);
}
