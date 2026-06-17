#pragma once
#include <time.h>
#include "driver/i2c_master.h"

// Global time-sync orchestrator.
//
// Priority (highest first):
//   1. QMX/QMX+ GPS-derived time (via TM; CAT query) — re-syncs on GPS lock events
//   2. SNTP/WiFi
//   3. Manual input (rare POTA use via time_sync_set_manual)
//
// Any accepted sync writes through to the Tab5's RX8130CE supercap RTC so the
// clock survives a power-off (30-40 h retention). The system clock (settimeofday)
// is always updated too.
//
// Call time_sync_init() once after display_init() (I2C bus must be up).

// Init: bring up RTC, apply to system clock if valid, spawn periodic QMX sync task.
void time_sync_init(i2c_master_bus_handle_t bus);

// Called from the SNTP callback with the validated UTC epoch.
// Writes to RTC and updates the NVS date anchor.
void time_sync_notify_sntp(time_t utc);

// Called whenever a QMX time-of-day is available (H:M:S, no date).
// Reconstructs full UTC from the current date (RTC or NVS anchor), writes to
// RTC, and sets the system clock.
void time_sync_notify_qmx(int h, int m, int s);

// Manual override: full date + time in UTC. For rare POTA sessions where
// QMX has no GPS and WiFi is unavailable.
void time_sync_set_manual(int year, int mon, int mday, int h, int m, int s);
