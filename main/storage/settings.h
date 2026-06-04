#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// All persisted settings. Floats are stored as raw 32-bit bit-patterns
// in NVS (NVS doesn't have a native float type).
typedef struct {
    float db_min;       // spectrum/waterfall floor (dBm)
    float db_max;       // spectrum/waterfall ceiling (dBm)
    float ema_alpha;    // spectrum EMA smoothing (0..1)
    bool  iq_enabled;   // I/Q balance correction on/off
    bool  flat_mode;    // Phase 5.12: flat-spectrum view (per-bin floor)
    char  wifi_ssid[33];   // WiFi SSID (32 chars + NUL, IEEE max)
    char  wifi_pass[65];   // WiFi password (64 chars + NUL, WPA2 max)
    uint32_t last_vfo_hz; // last QMX VFO frequency in Hz (0 = unknown)
    uint16_t cw_pitch_hz;  // CW sidetone offset in Hz (default 700)
    uint8_t  colormap_idx; // waterfall colour map: 0=Thermal 1=Viridis 2=Turbo 3=Grayscale
    char     my_callsign[16];  // operator callsign for FT8 (15 chars + NUL)
    char     my_grid[8];       // Maidenhead grid (6 chars + NUL, e.g. "JO45ab")
} qmx_settings_t;

// Initialise the settings module. Opens an NVS handle. Safe to call
// even if NVS init failed in main; in that case all setters are no-ops
// and load_all returns defaults.
void settings_init(void);

// Populate *out with stored values, or defaults for fields not yet
// written. Always succeeds (falls back to defaults silently).
void settings_load_all(qmx_settings_t *out);

// Per-field setters. Each schedules a debounced flush to NVS — fast
// repeated calls (e.g. a slider drag) only result in one flash write
// after the user pauses.
void settings_set_db_min(float v);
void settings_set_db_max(float v);
void settings_set_ema_alpha(float v);
void settings_set_iq_enabled(bool v);
void settings_set_flat_mode(bool v);
// WiFi credential setters. Pass NULL or empty string to clear.
void settings_set_wifi_ssid(const char *ssid);
void settings_set_wifi_pass(const char *pass);

// Save last-known VFO frequency (debounced flush; same wear profile as the sliders).
void settings_set_last_vfo(uint32_t hz);
// Save CW sidetone pitch in Hz (debounced flush).
void settings_set_cw_pitch_hz(uint16_t hz);
// Save waterfall colour-map index (debounced flush).
void settings_set_colormap_idx(uint8_t idx);

// Operator identity. Pass NULL or empty to clear. Used by the FT8
// transmitter (v0.11+) and shown in the FT8 view info pane.
void settings_set_my_callsign(const char *call);
void settings_set_my_grid(const char *grid);

// Force any pending writes to flash immediately. Call before reboot
// if you want absolute certainty. Normally not needed.
void settings_flush(void);

#ifdef __cplusplus
}
#endif