#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// FT8 CQ-run reply filters: up to two "include" and two "exclude" terms,
// each independently enabled, matched against the *whole* decoded message
// text (so POTA/SOTA tags, country prefixes, grids etc. are all fair game,
// not just the callsign). Plus two standalone toggles.
#define FT8_FILTER_TEXT_LEN 16

// How the robot ranks eligible CQ callers when several pass the filters in the
// same slot. (Stored as uint8_t robot_priority.)
typedef enum {
    FT8_ROBOT_PRI_STRONGEST = 0,  // highest SNR first (best chance of completing)
    FT8_ROBOT_PRI_WEAKEST   = 1,  // lowest SNR first (help the weak ones / ragchew DX hunt)
    FT8_ROBOT_PRI_DISTANT   = 2,  // greatest great-circle distance from our grid (DX)
} ft8_robot_priority_t;

typedef struct {
    bool incl_en[2];
    char incl_text[2][FT8_FILTER_TEXT_LEN];
    bool excl_en[2];
    char excl_text[2][FT8_FILTER_TEXT_LEN];
    bool excl_worked_before; // skip callers already in the ADIF log (enforced since the robot landed)
    bool excl_plain_cq;      // hide bare "CQ ..." rows, show only replies to us
    bool incl_cq_only;       // show ONLY "CQ ..." rows (display filter; does not affect auto-reply)
    // --- Robot (auto-answer) — appended; old NVS blobs read back 0 (=off, STRONGEST) ---
    bool    robot_en;        // auto-answer CQ callers with no tap (default off)
    uint8_t robot_priority;  // ft8_robot_priority_t: which caller to pick first
} ft8_filters_t;

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
    int16_t  cw_cal_hz;        // CW LO trim (Hz), default -60, range +/-100
    float    zoom_factor;      // spectrum/waterfall zoom, 1.0=full, max 24.0
    uint8_t  brightness_pct;   // LCD backlight brightness, 0..100, default 100
    uint8_t  last_ui_mode;     // last UI mode: 0=Panadapter, 1=FT8 (default 0)
    uint32_t last_unix_time;   // last UTC unix time seen from SNTP (0 = never synced)
    char     cq_msg[3][28];    // 3 user-editable CQ message presets (FT8 TX)
    uint8_t  cq_sel;           // which CQ preset is active, 0..2 (default 0)
    bool     onboarded;        // first-boot WiFi/identity prompts shown (default false)
    bool     wifi_enabled;     // initiate WiFi at boot (default true)
    bool     qmx_gps;         // QMX/QMX+ has GPS discipline — skip Tab5→QMX time push
    bool     freq_kp_calc;    // freq keypad digit layout: false=phone, true=10-key/calc
    int16_t  freq_kp_dx;      // freq keypad popup position: offset from screen center, px (default 0,0)
    int16_t  freq_kp_dy;
    char     qrz_api_key[40]; // QRZ Logbook API key (GUID-format, set via web UI)
    uint32_t qrz_uploaded_n;  // count of ADIF records already uploaded to QRZ
    char     eqsl_user[16];   // eQSL.cc username (callsign), set via web UI
    char     eqsl_pswd[32];   // eQSL.cc password
    uint32_t eqsl_uploaded_n; // count of ADIF records already uploaded to eQSL
    bool     cw_audio_en;     // CW sidetone audio on Tab5 speaker/headphone (default false)
    uint8_t  cw_audio_vol;    // CW audio output volume 0..100 (default 60)
    float    wf_black_db;     // waterfall black level: dB above floor -> black (default 9)
    float    wf_contrast_db;  // waterfall contrast: dB span filling the colour ramp (default 45)
    uint8_t  wf_floor_blend;  // waterfall per-bin floor blend 0..100% (0=global, default 100)
    uint8_t  wf_window;       // FFT window: 0=Blackman-Harris 1=Hann 2=Nuttall (default 0)
    bool     display_flip;    // landscape flipped 180 deg for upside-down mounting (default false)
    bool     snap_to_peak;    // tap-to-tune snaps to the strongest nearby signal (default true)
    uint8_t  bandplan_region; // band-plan strip region: 0=auto(from grid) 1=R1 2=R2 3=R3
    bool     distance_in_miles; // FT8 decode list: show distance in miles instead of km (default false)
    bool     ft8_sync_lines;  // Panadapter: FT8-sync-vs-SNTP waterfall slot-boundary lines + 2x waterfall speed (default false)
    ft8_filters_t ft8_filters;        // CQ-run reply include/exclude filters
    bool     field_day_en;    // ARRL Field Day exchange mode: TX/RX class+section instead of grid/report (default false)
    char     fd_class[4];     // Field Day class, e.g. "16A" (1-2 digit transmitter count + category letter)
    char     fd_section[4];   // Field Day ARRL/RAC section abbreviation, e.g. "EMA"
    bool     sim_mode_en;     // FT8 simulation mode: phantom stations, real radio never keyed (default false)
    uint8_t  ft8_op_mode;     // FT8/FT4 sub-mode (ft8_op_mode_t: 0=FT8 1=FT4), default 0 - see ft8_test.h
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

// FT8 CQ message presets. idx 0..2. Pass NULL/empty to clear a slot.
// settings_set_cq_sel selects the active preset (0..2). Debounced flush.
void settings_set_cq_msg(uint8_t idx, const char *text);
void settings_set_cq_sel(uint8_t idx);


// First-boot onboarding done: once true, the WiFi/identity prompts are never
// shown again (debounced flush).
void settings_set_onboarded(bool v);

// FT8 CQ-run reply include/exclude filters (debounced flush).
void settings_set_ft8_filters(const ft8_filters_t *f);

// CW audio output: enable the on-device CW sidetone (speaker/headphone), and
// its volume 0..100 (debounced flush).
void settings_set_cw_audio_en(bool v);
void settings_set_cw_audio_vol(uint8_t v);

// Waterfall colorisation (debounced flush). Black level and contrast are in
// dB; floor blend is 0..100 (% per-bin vs global floor); window is the FFT
// analysis window index (0=Blackman-Harris 1=Hann 2=Nuttall).
void settings_set_wf_black_db(float db);
void settings_set_wf_contrast_db(float db);
void settings_set_wf_floor_blend(uint8_t pct);
void settings_set_wf_window(uint8_t idx);

// Display 180-degree flip for upside-down mounting (debounced flush).
void settings_set_display_flip(bool v);

// Tap-to-tune snap-to-strongest-signal on/off (debounced flush). When false a
// tap tunes exactly where you touched (after the mode grid-snap), with no pull
// toward the nearest peak.
void settings_set_snap_to_peak(bool v);

// FT8 distance display unit (debounced flush). When false show distance in km,
// when true show distance in miles.
void settings_set_distance_in_miles(bool v);

// Panadapter-only diagnostic overlay (debounced flush): when true, draws the
// FT8-sync-vs-SNTP-only waterfall slot-boundary marker lines and doubles the
// waterfall scroll speed; when false, waterfall behaves exactly as before.
void settings_set_ft8_sync_lines(bool v);

// Band-plan strip region (debounced flush): 0=auto (derive from grid), 1=R1,
// 2=R2, 3=R3.
void settings_set_bandplan_region(uint8_t v);

// ARRL Field Day exchange mode (debounced flush). When enabled, FT8 QSOs
// exchange class+section (fd_class/fd_section) instead of grid/signal report.
void settings_set_field_day_en(bool v);
void settings_set_fd_class(const char *cls);
void settings_set_fd_section(const char *section);

// FT8 simulation mode (debounced flush): phantom-station practice mode -
// see ft8_sim.h. The real QMX is never keyed while this is on.
void settings_set_sim_mode_en(bool v);
void settings_set_ft8_op_mode(uint8_t v);

// WiFi boot-initiation toggle (debounced flush). When false the radio stays
// idle at boot even if credentials are stored; the user can re-enable from
// the settings drawer.
void settings_set_wifi_enabled(bool v);

// QMX/QMX+ GPS discipline flag (debounced flush). When true the Tab5 will
// NOT push its clock to the QMX after a sync, because the GPS is more
// accurate than anything the Tab5 has (SNTP, FT8, manual).
void settings_set_qmx_gps(bool v);

// Freq keypad digit layout: false=phone (1 2 3 top), true=10-key/calculator
// (7 8 9 top). Persisted (debounced flush) so it survives a reboot.
void settings_set_freq_kp_calc(bool v);

// Freq keypad popup position: offset (dx, dy) in pixels from screen center,
// where it last sat after being dragged. Debounced flush (a drag-release
// writes once, not per-pixel during the drag). Deliberately NOT part of
// DIRTY_CONFIG_EXPORT_MASK — purely cosmetic placement, not worth a SD-card
// mirror write.
void settings_set_freq_kp_pos(int16_t dx, int16_t dy);

// QRZ Logbook API key, set via the web UI (debounced flush). Pass NULL or
// empty to clear.
void settings_set_qrz_api_key(const char *key);

// Count of ADIF records already uploaded to QRZ (offset into the log file
// for the next upload batch). Debounced flush.
void settings_set_qrz_uploaded_n(uint32_t n);

// eQSL.cc credentials, set via the web UI (debounced flush). Pass NULL or
// empty to clear. eQSL has no API-key scheme - username+password is the only
// auth eQSL's real-time interface supports.
void settings_set_eqsl_user(const char *user);
void settings_set_eqsl_pswd(const char *pswd);

// Count of ADIF records already uploaded to eQSL (offset into the log file
// for the next upload batch). Debounced flush.
void settings_set_eqsl_uploaded_n(uint32_t n);

// QMX IF offset calibration trim (Hz). Per-unit oscillator variance shifts
// the +12 kHz IF injection; this trim corrects what users see on the spectrum/
// waterfall. Clamped to +/-200 Hz, persisted to NVS (debounced flush).
void settings_set_cw_cal_hz(int16_t hz);
void settings_set_zoom_factor(float v);

// LCD backlight brightness, 0..100 (debounced flush).
void settings_set_brightness_pct(uint8_t pct);

// Last UI mode: 0=Panadapter, 1=FT8 (debounced flush).
void settings_set_last_ui_mode(uint8_t mode);

// Save last-known UTC unix time from SNTP (debounced flush). Used as a
// "last known date" anchor so the QMX RTC time-of-day (no date) can be
// turned into a full timestamp when SNTP is unavailable (e.g. POTA).
void settings_set_last_unix_time(uint32_t unix_sec);

// Force any pending writes to flash immediately. Call before reboot
// if you want absolute certainty. Normally not needed.
void settings_flush(void);

#ifdef __cplusplus
}
#endif