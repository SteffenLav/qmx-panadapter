#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the C6 co-processor, initialise the WiFi station, connect to
// the SSID/password stored in NVS (skipped if empty), and start SNTP.
// Returns immediately; the actual work runs in a background task.
// Safe to call once at startup.
void panadapter_wifi_start(void);

// Update WiFi credentials at runtime. SSID and password are persisted
// to NVS, and a disconnect/reconnect cycle is triggered. Safe to call
// before panadapter_wifi_start (credentials will be picked up at boot).
void panadapter_wifi_reconnect(const char *ssid, const char *pass);

// Returns true once the station has an IP address.
bool wifi_is_connected(void);

// Currently-connected SSID; empty string if not connected.
const char *wifi_get_ssid(void);

// Current AP signal strength in dBm; 0 if not connected.
int wifi_get_rssi_dbm(void);

// Currently-assigned IP address as a dotted-quad string.
// Returns empty string if not connected or no IP yet.
const char *wifi_get_ip(void);

// Returns true once SNTP has set the system time at least once.
bool wifi_time_is_valid(void);

// ---- WiFi scan (for the SSID picker) ----------------------------------
// One scanned access point.
typedef struct {
    char    ssid[33];
    int8_t  rssi;     // dBm
    bool    locked;   // true if not an open network
} wifi_scan_ap_t;

typedef enum {
    WIFI_SCAN_IDLE = 0,
    WIFI_SCAN_RUNNING,
    WIFI_SCAN_DONE,
    WIFI_SCAN_FAILED,
} wifi_scan_state_t;

// Kick off an asynchronous scan of nearby APs. Brings the radio up first if
// needed (offloaded to a task — safe to call from the LVGL thread; returns
// immediately). Poll panadapter_wifi_scan_state() for completion.
void panadapter_wifi_scan_start(void);

// Current scan state.
wifi_scan_state_t panadapter_wifi_scan_state(void);

// Copy up to `max` scan results (deduplicated, strongest first) into `out`.
// Returns the number copied. Valid once the state is WIFI_SCAN_DONE.
int panadapter_wifi_scan_get(wifi_scan_ap_t *out, int max);

#ifdef __cplusplus
}
#endif