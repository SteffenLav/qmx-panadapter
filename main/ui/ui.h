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
void ui_update_smeter(int s_units);
void ui_update_mode(const char *mode);   // Phase 5.10: e.g. "USB", "CW"
void ui_update_band(const char *band);   // Phase 5.10: e.g. "20m", "40m"
void ui_refresh_band_label(uint32_t freq_hz);  // cheap, call every FA poll
void ui_push_spectrum(const float *bins, int n_bins);   // Phase 4
void ui_push_waterfall_row(const uint8_t *rgb565_row);  // Phase 5

// Brief centered auto-hiding toast (1.5 s). Runs on the LVGL task — call only
// from the LVGL/UI thread. Used for "Work in progress…." on shelved controls.
void ui_toast(const char *msg);

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
int  ui_get_if_offset_hz(void);        // Baseband Hz the dial maps to (12 kHz, +CW LO offset+trim in CW)

// Passband edges in Hz, relative to VFO/dial (mode + CAT-width dependent).
void ui_get_passband_edges_hz(int32_t *out_low, int32_t *out_high);

// Bottom status bar: 3-zone layout (left/center/right). Pass NULL or "" to clear.
void ui_set_bottom_left(const char *text);
void ui_set_bottom_battery(const char *icon, uint32_t icon_color_hex, const char *text);
// Show a static struck-through red battery (no pack attached); no text, no flicker.
void ui_set_bottom_battery_absent(void);

// Bottom-bar firmware version, centered between the battery text and the UTC clock.
void ui_set_bottom_version(const char *text);

// Bottom-bar SD-backup indicator: a small red dot (+ "SD" label) between the
// battery voltage and the firmware version that breathes while a microSD card
// is mounted and being mirrored (active=true), hidden when false. Driven by
// the sd_archive task on mount/unmount, and once at boot to sync initial state.
void ui_set_sd_active(bool active);

// Full-screen breathing red bezel shown while FT8 simulation mode is on
// (see ft8_sim.h) - an unmissable reminder that nothing transmitted right
// now is real. Called from the FT8-drawer-only sim mode toggle and once at
// drawer-build time to restore the saved NVS state.
void ui_set_sim_mode_indicator(bool active);

// Re-evaluate the breathing red border from both its sources (the drawer's
// general sim-mode toggle AND the FT8/FT4 sub-mode, since FT4 TX is always
// forced through the same interlock regardless of the toggle - see ft8_tx.c's
// FT4 SAFETY note). Call this after either source changes, instead of
// ui_set_sim_mode_indicator() directly.
void ui_refresh_sim_mode_indicator(void);

// Bottom-bar right zone: WiFi icon+SSID (or "off" text), optional RSSI
// (rendered in jitter-free fixed-width digit cells), and a trailing
// suffix (e.g. " -67dBm  192.168.1.5" minus the rssi number -> "dBm  192.168.1.5").
// Pass show_rssi=false to hide the RSSI cells (disconnected).
void ui_set_bottom_wifi(const char *icon_ssid, bool show_rssi, int rssi_dbm, const char *suffix);

// Bottom-bar UTC clock (center). valid=false shows "--:--:--" with suffix.
// suffix is the time-source indicator, e.g. " UTC(NTP)" or " UTC".
void ui_set_bottom_clock(int h, int m, int s, bool valid, const char *suffix);
bool ui_get_flat_mode(void);
void ui_set_flat_mode(bool on);
void ui_flat_mode_reset(void);  // re-seed flat-spectrum floor on first audio after QMX (re)connect

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

// Open the frequency entry keypad pre-filled with initial_hz and
// initial_mode (one of "DiGi"/"USB"/"LSB"/"CW"), in "picker" mode: Enter
// calls cb(typed_hz, selected_mode, true) without touching the QMX; Cancel
// (or tap-outside) calls cb(0, selected_mode, false). Used by the memory
// modal to confirm/edit a frequency + mode before naming a memory slot.
typedef void (*ui_freq_picker_cb_t)(uint32_t freq_hz, const char *mode, bool accepted);
void ui_freq_picker_open(uint32_t initial_hz, const char *initial_mode, ui_freq_picker_cb_t cb);

// Restart the infinite-repeat "breathing" opacity animations on the
// edge-swipe grip handles. lv_anim_delete_all() (screenshot capture) kills
// these along with one-shot anims; call this after taking a snapshot.
void ui_restart_edge_grip_anims(void);

// ---- Physical (Tab5 snap-on) keyboard bridge ----
// Track which textarea the physical keyboard should type into. Called from
// the textarea FOCUSED/DEFOCUSED/DELETE events wired up in
// ui_theme_style_textarea(); not normally called directly.
void ui_kbd_note_focus(lv_obj_t *ta);
void ui_kbd_note_unfocus(lv_obj_t *ta);

// Register the open modal's Save and Cancel buttons so the physical keyboard's
// Enter key clicks Save (commit + close) and Esc clicks Cancel. Either may be
// NULL. Auto-cleared when the buttons are deleted (modal closed). Call once
// after the modal's buttons are created.
void ui_kbd_set_buttons(lv_obj_t *save_btn, lv_obj_t *cancel_btn);

// Register the physical keyboard's text callback (installs an internal handler
// that applies typed characters to the focused textarea on the LVGL thread).
// Call once after the UI is built.
void ui_kbd_bridge_init(void);
