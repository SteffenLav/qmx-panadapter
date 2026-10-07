#pragma once
#include <time.h>
#include <stdint.h>
#include "driver/i2c_master.h"

// Which source last disciplined the system clock.
typedef enum {
    TIME_SOURCE_NONE,     // no sync yet (boot state)
    TIME_SOURCE_RTC,      // Tab5 RX8130CE supercap RTC
    TIME_SOURCE_QMX,      // QMX TM; (offline / POTA fallback)
    TIME_SOURCE_SNTP,     // internet NTP via WiFi
    TIME_SOURCE_MANUAL,   // manual entry
    TIME_SOURCE_FT8,      // derived from FT8 signal timing (sub-second precision)
    TIME_SOURCE_UNIT_GPS, // Unit GPS v1.1 on PORT.A (NMEA RMC, satellite UTC+date)
} time_sync_source_t;

// Returns the source that last applied a time update (the last WRITER).
time_sync_source_t time_sync_get_source(void);

// The source actually MAINTAINING the clock now (the current authority), for the
// UI label - GPS/SNTP when one is up, regardless of a stray one-off FT8/manual
// nudge. Use this for display; time_sync_get_source() is the raw last-writer.
time_sync_source_t time_sync_get_effective_source(void);

// Auto-detected: is the connected QMX GPS-disciplined? Derived at CAT connect
// from whether its tick agrees tightly with SNTP (no manual config). Drives the
// GPS-primary behaviour + the UTC(GPS) vs UTC(QMX)/UTC(NTP) label.
bool time_sync_qmx_gps_confirmed(void);

// Sum of every FT8-derived nudge applied since the last hard sync
// (SNTP/QMX/manual/RTC), in ms (positive = clock was running fast, time was
// subtracted). current_time_ms + this value reconstructs "what time would
// it be if FT8 auto-sync had never nudged the clock" - i.e. the SNTP/QMX-only
// timeline. Used by the panadapter waterfall's FT8-vs-SNTP slot-boundary
// overlay; not used for any sync decision itself.
int64_t time_sync_get_ft8_offset_ms(void);

// Global time-sync orchestrator.
//
// Sync priorities (highest first):
//   0. Unit GPS v1.1 on PORT.A (live = LOCKED and inside its freshness
//      window) - a satellite clock with the FULL DATE, offline-capable, and
//      ranked above everything else while it is live
//   1. QMX/QMX+ GPS-disciplined time (TM; when GPS locked — re-syncs on GPS lock events)
//   2. Tab5 RX8130CE RTC (supercap-backed, applied immediately at boot)
//   3. SNTP/WiFi (accurate, but defers to QMX when QMX has recently synced)
//   4. QMX/QMX+ any clock (crystal oscillator, used when offline / no GPS)
//   5. Manual input (rare POTA use via time_sync_set_manual)
//
// The effective (displayed) authority is computed in time_sync.c's
// time_sync_get_effective_source(): live Unit GPS > live QMX-GPS > SNTP > the
// last writer. That is the single place the ranking lives - do not restate it
// here, this header's numbering predates several of the rules and has been
// stale before (see time_sync_notify_qmx's own comment).
//
// GPS lock detection (QMX path): the QMX CAT protocol does not expose a GPS
// status command. Until detected, all QMX TM; time is treated as potentially
// GPS-disciplined — SNTP does not override the system clock when QMX has
// synced in the last 5 min. The Unit GPS path needs no detection: its NMEA
// status byte IS the lock bit.
//
// Any accepted sync writes through to the RX8130CE so the clock persists
// across power-off (30-40 h supercap retention).
//
// Call time_sync_init() once after display_init() (I2C bus must be up).

// Init: bring up RTC (priority 2), apply to system clock if valid, spawn sync task.
void time_sync_init(i2c_master_bus_handle_t bus);

// Priority 0: Unit GPS v1.1 (NMEA RMC on PORT.A). Builds UTC from the FULL
// civil date and time the sentence carries - never from get_date_anchor() -
// so the DATE comes from the satellite and s_date_verified follows it.
//
// flip_us is the esp_timer stamp of the sentence that carried the N->N+1
// second edge (0 when this sentence carries no edge, e.g. the first lock), and
// frac_us is the sentence's own fractional second. Together they phase-align
// the clock to the second boundary the same way apply_gps_tick() does for a
// QMX+ tick; with neither, the apply is whole-second and says so.
//
// An online SNTP disagreement is LOGGED with its delta, never used as a veto:
// RMC's status byte is a real lock indication, where SNTP agreement was only
// ever an inference. Returns true when the time was applied.
bool time_sync_notify_unit_gps(int year, int mon, int mday,
                               int h, int m, int s,
                               uint32_t frac_us, int64_t flip_us);

// Live Unit GPS as a time reference: pipeline LOCKED and inside
// UNIT_GPS_FRESH_MS (a single freshness window, owned by the unit_gps module -
// this is a wrapper, not a second opinion). False whenever Port mode is not
// unit_gps, because the module is then OFF.
bool time_sync_unit_gps_is_live(void);

// Priority 3: called from the SNTP callback with the validated UTC epoch.
// Always writes to RTC + NVS; only updates system clock when QMX has not synced
// in the last 5 minutes (so QMX GPS time, if active, is not overridden by SNTP).
void time_sync_notify_sntp(time_t utc);

// Priority 3 (offline/POTA fallback only - see time_sync.c for the real,
// current priority logic; this header's numbering predates it and is stale).
// Called when a QMX TM; response is available. Reconstructs full UTC from
// the best date anchor. Returns true and updates system clock + RTC only
// when SNTP is not currently authoritative (WiFi down, or never synced);
// returns false (no-op) when SNTP/WiFi is healthy, since SNTP wins per the
// documented priority. Callers that show a status message should check the
// return value rather than assuming the query succeeding means the clock
// changed.
bool time_sync_notify_qmx(int h, int m, int s);

// One attempt at reading the QMX+'s GPS RECEIVER over CAT (GP;). It settles
// two things the radio's own clock cannot: the DATE (TM; never carried one)
// and whether this radio HAS a GPS (the receiver's answer, not our inference
// from a clock we may have set ourselves).
//
// It does NOT set the clock phase, except when there is no usable clock at all
// or the satellite disagrees about the day - GP's second boundary measures
// ~1 s late, and the TM tick is an order of magnitude better. Callers must
// still run the TM path for the time. Blocks up to ~1.3 s and holds the CAT
// poll for that time. False on firmware older than 1.04_004 (immediately) or
// a radio with no fix (after the timeout).
bool time_sync_try_qmx_gp(void);

// Is the DATE trustworthy? True after SNTP, a surviving Tab5 RTC, or the
// operator confirming/setting it. False when the date was pasted from the
// last-known timestamp because the RTC ran down offline (Don WB0LQW's POTA log
// two days behind). ui/date_confirm_modal.c asks while this is false.
/* WHO set the date, persisted in NVS beside the time itself.
 *
 * This is NOT time_source_t. That one says where the current TIME came from and
 * changes minute to minute; this says whether the DATE on the clock descends
 * from something that actually knows the date, and it has to survive a power
 * cycle - which is the whole point.
 *
 * Numbers are on-flash values. Never renumber, only append.
 * 4 is reserved for the Unit GPS on PORT.A (ericmoritz, #15) so that branch
 * does not have to renumber when it lands. */
typedef enum {
    DATE_SRC_NONE     = 0,   /* nobody we trust - ask the operator */
    DATE_SRC_OPERATOR = 1,   /* they read it off the screen and confirmed it */
    DATE_SRC_SNTP     = 2,   /* internet */
    DATE_SRC_QMX_GPS  = 3,   /* a QMX+ with a GPS fix */
    DATE_SRC_UNIT_GPS = 4,   /* reserved: M5Stack Unit GPS on PORT.A (#15) */
} date_src_t;

bool time_sync_date_verified(void);
// The operator says the date shown is right.
void time_sync_confirm_date(void);
// Set the DATE only, keeping the time of day. Returns false if out of range.
bool time_sync_set_date(int year, int mon, int mday);
// Dev only: treat the date as unverified even with SNTP, until answered.
void time_sync_dev_force_date_unverified(void);

// Priority 5: manual override — full UTC date+time. For rare POTA sessions where
// QMX has no GPS and WiFi is unavailable.
void time_sync_set_manual(int year, int mon, int mday, int h, int m, int s);

// Apply a sub-second correction derived from FT8 signal timing.
// delta_ms > 0: system clock is fast by that many ms (time is subtracted).
// delta_ms < 0: system clock is slow (time is added). Writes through to RTC+NVS,
// and pushes the corrected time to the QMX over CAT (cat_set_qmx_time - a
// direct, blocking, non-poll-task-routed CDC write, up to 200 ms).
// Call only from a context that can tolerate that stall (e.g. UI button
// handlers) - NEVER from the FT8 decode task or any other hot path, where it
// would both block the caller and race the CAT poll task's own CDC writes.
// Use time_sync_apply_correction_ms_quiet() from hot paths instead.
void time_sync_apply_correction_ms(int delta_ms);

// Same as time_sync_apply_correction_ms(), but skips the QMX CAT push - safe
// to call from the FT8 decode task for continuous per-slot auto-sync. Still
// updates the system clock + Tab5 RTC + NVS (RTC is a separate I2C bus from
// CAT/CDC, so that write doesn't carry the same hazard). Enforces the
// FT8_LEASH_MS position bound and RETURNS the delta actually applied (after
// leashing; 0 if the leash blocked it) - the caller shows this as the "nudge".
int time_sync_apply_correction_ms_quiet(int delta_ms);

// Mark the time source as FT8 without changing the clock value.
void time_sync_mark_ft8(void);

// Mark the time source as QMX without changing the clock value.
void time_sync_mark_qmx(void);

// Push the current system clock to the QMX's onboard RTC right now.
// Respects the qmx_gps NVS flag — no-op when GPS discipline is active.
// Call whenever the Tab5 has a good time and the QMX should be updated
// (e.g. after the "Set and Sync" modal saves in NTP mode, where the
// system clock itself did not change so push_to_qmx wasn't triggered).
void time_sync_push_to_qmx(void);

// DEV ONLY: re-arm the one-shot QMX GPS auto-detection so it runs again on the
// next periodic pass (within 5 min), without rebooting.
//
// Exists because the detection is deliberately once-per-boot, and the only other
// way to re-trigger it is a Tab5 reset - which, with the radio attached, is the
// documented #74 USB-wedge trigger, and recovering from THAT needs a QMX power
// cycle, which resets the radio's clock and so destroys the very precondition
// the test needs. Same reasoning as the cat_raw escape hatch: make the question
// answerable instead of guessed.
void time_sync_force_redetect(void);
