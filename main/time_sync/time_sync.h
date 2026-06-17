#pragma once
#include <time.h>
#include "driver/i2c_master.h"

// Global time-sync orchestrator.
//
// Sync priorities (highest first):
//   1. QMX/QMX+ GPS-disciplined time (TM; when GPS locked — re-syncs on GPS lock events)
//   2. Tab5 RX8130CE RTC (supercap-backed, applied immediately at boot)
//   3. SNTP/WiFi (accurate, but defers to QMX when QMX has recently synced)
//   4. QMX/QMX+ any clock (crystal oscillator, used when offline / no GPS)
//   5. Manual input (rare POTA use via time_sync_set_manual)
//
// GPS lock detection: the QMX CAT protocol does not expose a GPS status command.
// Until detected, all QMX TM; time is treated as potentially GPS-disciplined —
// SNTP does not override the system clock when QMX has synced in the last 5 min.
// Any accepted sync writes through to the RX8130CE so the clock persists across
// power-off (30-40 h supercap retention).
//
// Call time_sync_init() once after display_init() (I2C bus must be up).

// Init: bring up RTC (priority 2), apply to system clock if valid, spawn sync task.
void time_sync_init(i2c_master_bus_handle_t bus);

// Priority 3: called from the SNTP callback with the validated UTC epoch.
// Always writes to RTC + NVS; only updates system clock when QMX has not synced
// in the last 5 minutes (so QMX GPS time, if active, is not overridden by SNTP).
void time_sync_notify_sntp(time_t utc);

// Priority 4 (or 1 when GPS-disciplined): called when a QMX TM; response is available.
// Reconstructs full UTC from the best date anchor. Always updates system clock + RTC.
// Records a timestamp so SNTP defers to QMX for the next 5 minutes.
void time_sync_notify_qmx(int h, int m, int s);

// Priority 5: manual override — full UTC date+time. For rare POTA sessions where
// QMX has no GPS and WiFi is unavailable.
void time_sync_set_manual(int year, int mon, int mday, int h, int m, int s);
