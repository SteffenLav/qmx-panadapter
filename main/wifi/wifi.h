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

// Persist WiFi credentials (NVS + live driver config) WITHOUT changing the
// connection state — no connect, no disconnect, and the on/off preference is
// left untouched. Use this when saving credentials while WiFi is meant to
// stay off, so a subsequent enable picks them up. (panadapter_wifi_reconnect
// is the "save AND connect now" variant.)
void panadapter_wifi_update_credentials(const char *ssid, const char *pass);

// Live WiFi on/off toggle. Persists the wifi_enabled preference to NVS AND
// takes effect immediately (offloaded to a task — safe from the LVGL thread):
//   enable  → brings the radio up and connects using the stored credentials
//             (starts it if this boot left it idle, else re-issues connect).
//   disable → disconnects, stops the radio, and suppresses the auto-reconnect
//             retry loop until re-enabled.
// A no-op beyond the NVS write if the WiFi subsystem hasn't been started yet
// (the persisted flag is then honoured at boot).
void panadapter_wifi_set_enabled(bool enabled);

// Returns true once the station has an IP address.
bool wifi_is_connected(void);

// Is WiFi even meant to be running? False after the operator turned it off (the
// drawer switch / POTA use). Lets callers tell "WiFi is broken" apart from "WiFi
// is off on purpose" - the context-help triage must not flag the second as a fault.
bool panadapter_wifi_is_enabled(void);

// Currently-connected SSID; empty string if not connected.
const char *wifi_get_ssid(void);

// Current AP signal strength in dBm; 0 if not connected.
int wifi_get_rssi_dbm(void);

// Currently-assigned IP address as a dotted-quad string.
// Returns empty string if not connected or no IP yet.
const char *wifi_get_ip(void);

// The live address/mask/gateway/DNS, as four dotted-quad strings. Returns false
// (and blanks all four) when there is no address yet. Every pointer may be NULL.
//
// This is the reference a proposed STATIC configuration is judged against - see
// util/ip_guard.h. Read live, never cached.
bool panadapter_wifi_get_lease(char ip[16], char mask[16],
                               char gw[16], char dns[16]);

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