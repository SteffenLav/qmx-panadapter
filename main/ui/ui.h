#pragma once

#include "lvgl.h"
#include <stdbool.h>

void ui_init(lv_display_t *disp);

// Restore the UI mode (Panadapter/FT8) persisted at the last toggle.
// Call after ft8_screen_init()/ft8_status_init()/ft8_tx_init()/ft8_qso_init()
// and audio/cat init have completed.
void ui_apply_saved_mode(void);

// Phase 4/5 hooks (stubs for now)
void ui_update_frequency(uint32_t freq_hz);
void ui_set_memory_label(const char *text);  /* active memory channel; pass NULL to clear */
void ui_update_smeter(int s_units);
void ui_update_mode(const char *mode);   // Phase 5.10: e.g. "USB", "CW"
void ui_update_band(const char *band);   // Phase 5.10: e.g. "20m", "40m"
void ui_refresh_band_label(uint32_t freq_hz);  // cheap, call every FA poll
void ui_push_spectrum(const float *bins, int n_bins);   // Phase 4
void ui_push_waterfall_row(const uint8_t *rgb565_row);  // Phase 5

// Phase 5.4: runtime-set spectrum display range (autoscale)
void ui_set_db_range(float db_min, float db_max);
void ui_set_cw_pitch_hz(uint16_t hz);  // CW sidetone offset, persisted to NVS
void ui_set_cw_cal_hz(int16_t hz);     // CW LO trim (Hz), +/-100, persisted to NVS
float    ui_get_zoom_factor(void);      // current zoom (1.0=full, max 24.0)
uint16_t ui_get_cw_pitch_hz(void);
uint32_t ui_band_last_hz(uint32_t center_hz); // 0 if never visited      // CW sidetone offset in Hz
int16_t  ui_get_if_cal_hz(void);         // per-unit IF calibration trim in Hz
int   ui_get_pan_offset_bins(void);     // current pan offset in FFT bins
void  ui_set_zoom(float zoom, int pan_bins); // set zoom+pan, persists zoom to NVS
int  ui_get_if_bin_shift(int n_bins);  // Total bin shift = (IF_OFFSET_HZ + if_cal_hz) -> bins

// Bottom status bar: 3-zone layout (left/center/right). Pass NULL or "" to clear.
void ui_set_bottom_left(const char *text);

// Bottom-bar firmware version, centered between the battery text and the UTC clock.
void ui_set_bottom_version(const char *text);

// Bottom-bar right zone: WiFi icon+SSID (or "off" text), optional RSSI
// (rendered in jitter-free fixed-width digit cells), and a trailing
// suffix (e.g. " -67dBm  192.168.1.5" minus the rssi number -> "dBm  192.168.1.5").
// Pass show_rssi=false to hide the RSSI cells (disconnected).
void ui_set_bottom_wifi(const char *icon_ssid, bool show_rssi, int rssi_dbm, const char *suffix);

// Bottom-bar UTC clock (center). valid=false shows "--:--:-- UTC"
// (pre-SNTP-sync placeholder); h/m/s ignored in that case.
void ui_set_bottom_clock(int h, int m, int s, bool valid);
void ui_set_flat_mode(bool on);

// Phase 5.4: update dB label text (called by autoscale)
void ui_set_db_labels(float db_min, float db_max);


// Phase 5.10G: passband indicator (CAT FW or mode default)
void ui_update_passband_width(uint32_t hz);

// Phase 9 (v0.9.5): read-only getters for the web server status JSON.
// Updated by the CAT task; readers may observe a torn ASCII string briefly
// during a mode change. Acceptable for a 1 Hz status poll.
const char *ui_get_mode_str(void);
const char *ui_get_band_str(void);
uint32_t ui_get_passband_width_hz(void);
