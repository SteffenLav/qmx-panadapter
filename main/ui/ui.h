#pragma once
#include "ui_mode.h"

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

void ui_init(lv_display_t *disp);

// Create the LVGL pointer (mouse) input device + on-screen cursor. Call once,
// after ui_init(). Reads USB HID mouse state (usb_hid_mouse.c); the cursor is
// hidden until a mouse is actually present. Safe to call even with no mouse.
void ui_mouse_init(void);

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
void ui_refresh_freq_label(uint32_t freq_hz);  // cheap, call every FA poll - re-asserts a dropped label write
void ui_refresh_bandplan_strip(uint32_t freq_hz);  // cheap, call every FA poll - same rationale
void ui_push_spectrum(const float *bins, int n_bins);   // Phase 4
void ui_push_waterfall_row(const uint8_t *rgb565_row);  // Phase 5

// Brief centered auto-hiding toast (1.5 s). Runs on the LVGL task — call only
// from the LVGL/UI thread. Used for "Work in progress…." on shelved controls.
void ui_toast(const char *msg);
/* Same toast, with a readable dwell for anything the operator must act on. */
void ui_toast_ms(const char *msg, uint32_t ms);
/* #298: arm the one-time "you can switch back" notice. Called from main.c with
 * the stored flag; a unit that has already been told never arms it. */
void ui_still_notice_arm(bool armed);

// The QMX was found receiving on VFO B or Split and has been switched to VFO A
// (the panadapter reads and writes VFO A only). Tells the operator, because we
// changed their radio. Safe off the LVGL thread.
void ui_set_vfo_switched_notice(const char *was);

// Phase 5.4: runtime-set spectrum display range (autoscale)
void ui_set_db_range(float db_min, float db_max);
// The QMX's achievable CW filter centres: 500-950 Hz in 25 Hz steps, the union of
// every centre across all its passbands (operation manual, "The complete list of 54
// filters now available in QMX"). Shared because cat.c range-checks the value it
// reads back from the radio and used to reject anything outside 600-800 - the same
// wrong assumption the slider had, which would have discarded a radio genuinely set
// to 550.
#define CW_CENTER_MIN_HZ  500
#define CW_CENTER_MAX_HZ  950
#define CW_CENTER_STEP_HZ 25

void ui_set_cw_pitch_hz(uint16_t hz);  // CW sidetone offset, persisted to NVS
// Adopt the radio's OWN CW centre without writing back to it. The boot-time push of
// our stored value is dead on arrival - measured: sent at 4.5 s, the radio is not
// reachable until ~17 s - so the radio's value is the one that must win, and this is
// how it gets in. Roy KI0ER asked for exactly this.
void ui_seed_cw_pitch_hz(uint16_t hz);
void ui_set_cw_cal_hz(int16_t hz);     // CW LO trim (Hz), +/-100, persisted to NVS
float    ui_get_zoom_factor(void);      // current zoom (1.0=full, max 24.0)
uint16_t ui_get_cw_pitch_hz(void);
void ui_set_rit_pill_show(bool show);  // panadapter RIT pill visibility (NVS-backed)
uint32_t ui_band_last_hz(uint32_t center_hz); // 0 if never visited      // CW sidetone offset in Hz
// True if hz falls inside a recognized HF amateur band's edges (lo/hi
// receive that band's edges if non-NULL). False for anything out of band
// or in a gap between bands - used to refuse saving an out-of-band memory
// channel frequency.
bool ui_validate_band_freq_hz(uint32_t hz, uint32_t *lo_out, uint32_t *hi_out);
int16_t  ui_get_if_cal_hz(void);         // per-unit IF calibration trim in Hz
int   ui_get_pan_offset_bins(void);     // current pan offset in FFT bins
void  ui_set_zoom(float zoom, int pan_bins); // set zoom+pan, persists zoom to NVS
/* Still display (#298): the spectrum and waterfall hold still and the VFO
 * cursor moves across them, instead of the display following the dial.
 * Toggle from /api/cmd {"action":"still_view","on":false} while it is
 * being evaluated - it changes how the whole panadapter feels. */
void ui_set_still_view(bool on);
bool ui_get_still_view(void);

/* The pan the display is ACTUALLY drawn at, in Hz. ui_get_pan_offset_bins() is
 * the same value rounded to a whole FFT bin (46.875 Hz); anything that has to
 * agree with the spectrum to better than that needs this one. */
int64_t ui_get_pan_offset_hz(void);

int  ui_get_if_bin_shift(int n_bins);  // Total bin shift = (IF_OFFSET_HZ + if_cal_hz) -> bins
int  ui_get_if_offset_hz(void);        // Baseband Hz the dial maps to (12 kHz, +CW LO offset+trim in CW)
int  ui_get_if_residual_hz(void);      // Hz between the DRAWN centre and the dial:
                                       // the spectrum is rotated a WHOLE number of
                                       // 46.88 Hz bins, so the true offset lands
                                       // between them. Drawn->freq subtracts it;
                                       // dial->pixel adds it. Was worth up to 23 Hz
                                       // of CW tuning error (Roy KI0ER).

// Passband edges in Hz, relative to VFO/dial (mode + CAT-width dependent).
void ui_get_passband_edges_hz(int32_t *out_low, int32_t *out_high);

// Bottom status bar: 3-zone layout (left/center/right). Pass NULL or "" to clear.
void ui_set_bottom_left(const char *text);
void ui_set_bottom_battery(const char *icon, uint32_t icon_color_hex, const char *text);
// Show a static struck-through red battery (no pack attached); no text, no flicker.
void ui_set_bottom_battery_absent(void);

// Bottom-bar firmware version, centered between the battery text and the UTC clock.
void ui_set_bottom_version(const char *text);

// #218: the bottom-bar version label doubles as the update indicator - it is
// the one surface always on screen. Text and colour are composed by status.c
// so the Tab5 and the browser say the same thing.
void ui_set_update_line(const char *text, uint32_t colour);

// Red, fixed-colour, PULSING variant for "a correctly-confirmed action just
// failed" (currently: an OTA download that could not reach the server).
// Fast opacity animation, not a colour swap - a 1 Hz colour alternation
// proved too subtle to read as "blinking" over the few seconds it needs to
// stay unmissable. status.c calls this once per second while the failure
// is being shown; a plain ui_set_update_line() call (any other state)
// stops the pulse.
void ui_set_update_line_failed(const char *text);

// The same breathing line in any colour. Used green for "downloaded, waiting
// for you", which persists until the operator acts and so has to be noticeable
// without taking the whole bar over.
void ui_set_update_line_pulsing(const char *text, uint32_t colour);
// Called on the LVGL thread when the operator taps the update line. status.c
// registers this and, since #239, it does exactly one thing: open ota_modal.
// That is what makes a single plain tap safe - a stray touch costs a
// dismissible window, never a download or a restart.
void ui_set_update_tap_cb(void (*cb)(void));

// Tell the UI whether the update line currently offers something to tap.
// While true the WHOLE bottom bar accepts the tap (no precision needed), and
// the band-plan strip refuses presses across the label's x-range - the strip is
// only 22 px, sits directly on the bar, and a tap on it retunes, so reaching
// for the update line used to move the dial.
void ui_set_update_line_tappable(bool on);
bool ui_update_line_tappable(void);

// Resource-monitor floating overlay text (see build_resource_monitor in
// ui.c). No-op if the panel object doesn't exist yet or was never toggled on.
void ui_set_resource_monitor_text(const char *text);

// Dev-only: toggle the resource-monitor overlay's visibility. Not a user
// feature — no drawer control. Triggered only by the hidden `resmon` web
// command (POST /api/cmd {"action":"resmon"}).
void ui_resource_monitor_toggle(void);

// Bottom-bar SD-backup indicator: a small static dot (+ "SD" label) between the
// battery voltage and the firmware version. Static, never breathing - a card in
// the slot is ambient state, not something to draw the eye to.
typedef enum {
    UI_SD_NONE = 0,        // no card found            -> dot hidden
    UI_SD_MIRRORING,       // mounted, mirroring live   -> GREEN
    UI_SD_SNAPSHOT_ONLY,   // boot backup written, live mirroring unavailable
                           // (WiFi is on - see sd_archive.c) -> YELLOW
} ui_sd_state_t;

void ui_set_sd_state(ui_sd_state_t st);

// Back-compat shim: true -> UI_SD_MIRRORING, false -> UI_SD_NONE.
void ui_set_sd_active(bool active);

// Persistent top-of-screen red banner shown whenever the QMX never confirmed
// IQ mode (see cat_get_iq_mode_confirmed()) - without it the spectrum will
// appear mirrored/shifted. Called from cat.c's link_task after the Q9 1;/Q9;
// retry loop at connect time; cleared automatically once a later attempt
// confirms IQ mode (e.g. on reconnect/power-cycle).
void ui_set_iq_mode_warning(bool active);

// Is the IQ-mode warning currently up? Read by the context-help triage so
// "the spectrum looks mirrored" is offered first when it is actually happening.
bool ui_iq_mode_warning_active(void);

// Release the radio to its own front panel (Stan's pause button, via Samuel
// W7STF), or take it back. Drives cat_user_pause_set() plus the on-screen
// banner and the drawer button's label, so every entry point - drawer, banner
// tap, web - leaves the same visible state. Safe from any task (the display
// lock is recursive).
void ui_set_cat_paused(bool paused);

/* Open/close the settings drawer from outside the UI. Used by the hidden
 * /api/cmd {"action":"drawer"} dev action so a layout change can be checked on
 * a screenshot. Call with the display lock held. */
void ui_set_drawer_open(bool open);

// TEMP (#285): hide the spectrum/waterfall canvases so LVGL's drawing cost can
// be decomposed against cpu_owners. bit0=spectrum, bit1=waterfall. Delete with
// the #284/#285 instruments.
void ui_dev_canvas_hide(unsigned mask);
void ui_set_drawer_expert(bool expert);
/* Scroll the open drawer so a section below the fold can be screenshotted. */
void ui_set_drawer_scroll_y(int y);

/* Power the Tab5 off after putting the radio back into receive.
 *
 * The power BUTTON cuts power in hardware with no warning to firmware, so a
 * shutdown mid-burst leaves the QMX keyed until it is power-cycled (Roy KI0ER).
 * This is the route that can be made safe: stop transmitting, flush settings,
 * then signal power off. Blocking, takes roughly a third of a second, and does
 * not return. */
void ui_power_off_safely(void);

// Call whenever a help overlay (the docs Reader, the "What's wrong?" panel) opens
// or closes. While one owns the screen the top-bar hit zones and the drawer/memory
// edge swipes are dropped out of hit-testing, and the QMX-wait prompt stands down -
// otherwise Panadapter navigation steals touches meant for the overlay, which is
// what made the Reader's own Back/Exit buttons untappable.
void ui_help_overlay_changed(void);

// Called from cat.c's VN; response handler once the QMX firmware version is
// known, so the drawer can reveal 1_04+-gated sections (AM mode, Tune button)
// even if it was already built (lazy, first-open) before VN; answered. No-op
// if the drawer hasn't been built yet - drawer_build() will see the current
// firmware string directly.
void ui_notify_qmx_fw_known(void);

// Called from cat_set_frequency() when a retune clears RIT, so tap-to-RIT mode
// stands down with the offset it was setting. Without it the operator's next tap
// on the spectrum would silently set an offset instead of tuning — a mode they
// did not ask to still be in. Flag write only, safe from any task; the pill's own
// timer repaints. See the RIT block in ui.c.
void ui_rit_notify_retune(void);

// Ask for the Panadapter/FT8 view to change, from any task. Thread-safe: sets a
// flag that the LVGL thread drains within ~1 s (the switch spawns/stops ft8_task
// and moves widgets, so it cannot run on the caller's task). Used by the web
// UI's view buttons.
void ui_request_base_mode(bool ft8);
void ui_request_base_mode_m(ui_mode_t m);   // names any of the three pages

/* Re-raise the edge-swipe strips. ⛔ ANY PAGE THAT FOREGROUNDS ITS OWN
 * near-full-screen container must call this afterwards, or it buries the
 * strips and the operator cannot swipe off that page. */
void ui_raise_edge_strips(void);

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

// Bottom-bar right zone: strength fan, SSID (or "off"), then the IP pinned to
// the right edge. rssi_dbm drives the fan's lit-element count and is ignored
// when connected=false (fan shown fully dim). Pass ip="" when disconnected.
// Bottom-bar Bluetooth glyph: dim when disabled, pale when scanning, blue when
// a device is connected.
void ui_set_bottom_bt(bool enabled, bool connected);

void ui_set_bottom_wifi(const char *ssid, bool connected, int rssi_dbm, const char *ip);

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

// The frequency the display is working from (freshest UI-commanded dial reading,
// falling back to the last CAT poll). Use this rather than cat_get_frequency()
// for anything display-shaped: cat reads 0 while the radio is off, but the Tab5
// keeps showing - and drawing spots on - the band it was last tuned to.
uint32_t ui_get_dial_freq_hz(void);

// Open the frequency entry keypad pre-filled with initial_hz and
// initial_mode (one of "DiGi"/"USB"/"LSB"/"CW"), in "picker" mode: Enter
// calls cb(typed_hz, selected_mode, true) without touching the QMX; Cancel
// (or tap-outside) calls cb(0, selected_mode, false). Used by the memory
// modal to confirm/edit a frequency + mode before naming a memory slot.
// Returns true to accept (closes the freq pad) or false to reject (the freq
// pad stays open, exactly as the user left it, so they can correct the value
// without losing their place - e.g. an out-of-band frequency). The return
// value is only consulted when accepted==true (the user pressed Enter);
// the accepted==false call (Cancel) always closes regardless of what's
// returned.
typedef bool (*ui_freq_picker_cb_t)(uint32_t freq_hz, const char *mode, bool accepted);
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
/* Feed one keystroke from a non-snap-on source (Bluetooth HID keyboard, #273).
 * text: one char = literal, longer = a named key token ("enter", "esc", "tab",
 * "backspace", "del", "left", "right", "up", "down", "pgup", "pgdn").
 * mods: HID_KEY_MOD_CTRL / HID_KEY_MOD_ALT, as ui.c's own shortcut table uses. */
void ui_kbd_feed(const char *text, uint8_t mods);

void ui_kbd_set_buttons(lv_obj_t *save_btn, lv_obj_t *cancel_btn);
// Let the physical keyboard's arrows / PgUp / PgDn scroll this object while it
// is the visible one. Register once at build time; deletion deregisters.
void ui_kbd_add_scrollable(lv_obj_t *obj);

// User-defined keyboard shortcuts (#233). Bindings store an ACTION ID, so the
// numbering in ui.c's k_kbd_actions[] is a persisted contract: append freely,
// never renumber. ui_kbd_bindings_set() validates and persists in one step.
#include "storage/settings.h"        // kbd_binding_t
int         ui_kbd_action_count(void);
const char *ui_kbd_action_name(int id);
int         ui_kbd_bindings_get(kbd_binding_t *out, int max);
void        ui_kbd_bindings_set(const kbd_binding_t *in, int n);
void        ui_kbd_bindings_reset_defaults(void);

// Register the physical keyboard's text callback (installs an internal handler
// that applies typed characters to the focused textarea on the LVGL thread).
// Call once after the UI is built.
void ui_kbd_bridge_init(void);

// ---- Basic/Advanced drawer membership (web "Tab5 config" table) -----------
// One entry per drawer section, in DRAWER ORDER, so the web page renders the
// table without knowing the grouping or inventing an order. The label comes
// from the same table the drawer itself is laid out from, so the page can never
// name a section the firmware does not have.
int  ui_drawer_map_count(void);
bool ui_drawer_map_entry(int idx, int *id, const char **group, const char **label,
                         bool *in_basic, bool *in_adv);
// Replace both masks (bit N = section id N) and re-lay the drawer. Safe from
// the httpd task: it takes the display lock itself.
void ui_drawer_map_set(uint64_t basic_mask, uint64_t adv_mask);
// Seeded from the group table's own expert flag, so the browser never has to
// know what the firmware considers advanced.
void ui_drawer_map_defaults(void);
// Current masks, so a partial update can leave untouched sections alone.
void ui_drawer_map_masks(uint64_t *basic, uint64_t *adv);
