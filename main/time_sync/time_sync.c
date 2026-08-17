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

time_sync_source_t time_sync_get_effective_source(void)
{
    if (gps_is_live())                                    return TIME_SOURCE_QMX;   // GPS
    if (wifi_is_connected() && wifi_time_is_valid())      return TIME_SOURCE_SNTP;
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
static void push_to_qmx(time_t utc)
{
    if (!cat_is_ready()) return;
    if (s_qmx_gps_confirmed) {
        ESP_LOGD(TAG, "QMX is GPS-disciplined — skipping Tab5→QMX time push");
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

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (epoch_is_sane((int64_t)cfg.last_unix_time)) return (time_t)cfg.last_unix_time;

    ESP_LOGW(TAG, "No valid date anchor (NVS=0x%08lx) — using fallback 2023-11-14; time-of-day will be correct",
             (unsigned long)cfg.last_unix_time);
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
    push_to_qmx(utc);
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
        bool ref_ok = (wifi_is_connected() && wifi_time_is_valid()) || gps_is_live();
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
        if (s_qmx_time_pushed) {
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

// True once we've made a real (online) GPS/not-GPS determination for the current
// QMX. Reset by a reboot (static init) - so a QMX swap re-detects on next boot;
// a hot-swap keeps the prior verdict until reboot (acceptable - swaps are rare).
static bool s_qmx_detect_done = false;

// One QMX time sync + one-time GPS auto-detection (replaces the manual flag).
// Detection runs on the QMX's OWN clock and requires SNTP as ground truth. A GPS
// QMX's tick agrees tightly -> confirmed; a small/unset QMX is far off ->
// rejected, and we push our time to set its RTC.
//
// ⚠ Ordering within one boot is NOT sufficient protection, though this comment
// used to say it was ("happens BEFORE any Tab5->QMX push, so a push cannot
// masquerade as GPS"). s_qmx_detect_done is reset by a TAB5 reboot; the clock we
// pushed lives in the RADIO, which is not rebooted with us. Reflash the Tab5 with
// the QMX left powered and this "first" detection reads a clock we set in an
// earlier session. That is a real false positive, seen on a GPS-less bench unit.
// The durable guard is s_qmx_time_pushed, which crosses boots the same way the
// radio's clock does.
static void qmx_sync_once(void)
{
    int h, m, s;
    int64_t flip_us;
    bool sntp_up = wifi_is_connected() && wifi_time_is_valid();

    // --- One-time auto-detect (needs SNTP to compare against) ---
    if (!s_qmx_detect_done && sntp_up) {
        bool gps = (cat_gps_tick_sync(&h, &m, &s, &flip_us) == ESP_OK) &&
                   apply_gps_tick(h, m, s, flip_us);   // tight-agreement test inside
        set_qmx_gps_confirmed(gps);
        s_qmx_detect_done = true;
        if (!gps) push_to_qmx(time(NULL));   // non-GPS: set its own RTC (once)
        if (gps)  return;                    // GPS confirmed + applied
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

    psram_task_create(time_sync_task, "time_sync", 3072, NULL, 4, tskNO_AFFINITY);
}
