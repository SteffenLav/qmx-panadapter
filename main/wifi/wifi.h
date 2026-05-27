#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the C6 co-processor, initialise the WiFi station, connect to
// the SSID/password defined in wifi_credentials.h, and start SNTP.
// Returns immediately; the actual work runs in a background task.
// Safe to call once at startup.
void wifi_start(void);

// Returns true once the station has an IP address.
bool wifi_is_connected(void);

// Returns true once SNTP has set the system time at least once.
bool wifi_time_is_valid(void);

#ifdef __cplusplus
}
#endif