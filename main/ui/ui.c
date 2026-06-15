#include "ui.h"
#include "ui_theme.h"
#include "render.h"
#include "render_waterfall.h"
#include "dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "cat.h"
#include "settings.h"
#include "wifi_config.h"
#include "memory_modal.h"
#include "identity_config.h"
#include "onboarding.h"
#include "iq_balance.h"
#include "diag_log.h"
#include "ui_mode.h"
#include "ui_clock.h"
#include "ft8_screen.h"
#include "ft8_screen_view.h"
#include "ft8_test.h"
#include "esp_lcd_touch.h"

static const char *TAG = "ui";

// Layout constants (1280x720)
#define TOP_BAR_H       60
#define DRAWER_W        520  /* Phase 5.10D Stage 2: settings drawer width (520 in v0.8.x to fit WiFi button + smoothing slider) */
#define BOTTOM_BAR_H    36
#define SPECTRUM_H      200
#define LABEL_BAR_H     32  /* Phase 5.10C: room for Montserrat 18 labels under tick marks */
// Phase 5.10E: QMX I/Q has a 12 kHz IF offset -- the signal at the QMX's
// tuned frequency lands at +12 kHz in the baseband. We compensate by
// shifting the displayed spectrum left by 12 kHz so the tuned signal
// appears at the visual center. Touch-to-tune math is unchanged because
// s_last_qmx_freq_hz is the dial reading, not the LO.
#define IF_OFFSET_HZ    12000

// Per-unit QMX IF calibration trim (Hz), loaded from NVS at init.
// Updated by drawer slider; used by ui_get_if_bin_shift() helper.
static int16_t s_cw_cal_hz = 0;  // loaded from NVS at boot, default -60

int ui_get_if_bin_shift(int n_bins)
{
    // IF_OFFSET_HZ (12 kHz) always. In CW mode add QMX CW LO offset + per-unit trim.
    // Integer math, rounded to nearest bin via half-step add when positive,
    // half-step subtract when negative.
    int total_hz = IF_OFFSET_HZ;
    const char *m = cat_get_mode_str();
    if (m && strcmp(m, "CW") == 0)
        total_hz += cat_get_cw_offset_hz() + (int)s_cw_cal_hz;
    int sign = (total_hz < 0) ? -1 : 1;
    int abs_hz = (total_hz < 0) ? -total_hz : total_hz;
    int shift = ((abs_hz * n_bins) + 24000) / 48000;  // +24000 = round to nearest
    return sign * shift;
}

void ui_set_cw_cal_hz(int16_t hz)
{
    if (hz < -200) hz = -200;
    if (hz >  200) hz =  200;
    s_cw_cal_hz = hz;
    settings_set_cw_cal_hz(hz);
    ESP_LOGI("ui", "IF cal set to %+d Hz", (int)hz);
}
// Zoom and pan state. zoom_factor=1.0 = full 48 kHz view.
// pan_offset_bins=0 = centered on dial freq. Not thread-safe —
// only written from LVGL task (touch callbacks) and read from render task.
// Zoom persisted to NVS; pan resets to 0 on boot/band change.
static float s_zoom_factor    = 1.0f;
static int   s_pan_offset_bins = 0;
static lv_obj_t *s_zoom_label  = NULL;  // top bar zoom indicator
static lv_obj_t *s_zoom_popup  = NULL;  // zoom preset dropdown panel

float ui_get_zoom_factor(void)    { return s_zoom_factor; }
int   ui_get_pan_offset_bins(void){ return s_pan_offset_bins; }

// Forward declaration: defined later, needed by ui_set_zoom() for
// passband-centered zoom (and exported via ui_get_passband_edges_hz()).
static void compute_passband_edges_hz(int32_t *out_low, int32_t *out_high);

// ---- Band preset popup ------------------------------------------------
// Per-band last-used frequency (session memory, not persisted).
// Index mirrors cat_get_band_list(). 0 = never visited, use center_hz.
static uint32_t s_band_last_hz[CAT_MAX_BANDS] = {0};

uint32_t ui_band_last_hz(uint32_t center_hz)
{
    int band_count = 0;
    const cat_band_entry_t *bands = cat_get_band_list(&band_count);
    for (int i = 0; i < band_count; i++) {
        if (bands[i].center_hz == center_hz) return s_band_last_hz[i];
    }
    return 0;
}

static lv_obj_t *s_band_popup = NULL;
static lv_obj_t *s_band_label;  // forward ref — defined below with other label statics

static void band_popup_close(void)
{
    if (s_band_popup) { lv_obj_delete(s_band_popup); s_band_popup = NULL; }
}

static void band_preset_cb(lv_event_t *e)
{
    uint32_t center_hz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    band_popup_close();
    // Use last-visited frequency on this band if we have one.
    int band_count = 0;
    const cat_band_entry_t *bands = cat_get_band_list(&band_count);
    uint32_t target = center_hz;
    for (int i = 0; i < band_count; i++) {
        if (bands[i].center_hz == center_hz && s_band_last_hz[i] != 0) {
            target = s_band_last_hz[i];
            break;
        }
    }
    cat_set_frequency(target);
}

static void band_overlay_cb(lv_event_t *e)
{
    (void)e;
    band_popup_close();
}

static void band_label_clicked_cb(lv_event_t *e);
static void band_popup_open(void)
{
    if (s_band_popup) { band_popup_close(); return; }
    int band_count = 0;
    const cat_band_entry_t *bands = cat_get_band_list(&band_count);
    if (band_count == 0) {
        ESP_LOGW("ui", "Band dropdown: no bands available (band_count=0)");
        return;
    }

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, band_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_band_popup = ov;

    int btn_h = 64;
    int panel_w = 140;
    // Extra 32px margin: covers any flex gap/padding the theme adds between
    // children, so the last row (15m) never gets clipped by the panel edge.
    int panel_h = band_count * btn_h + 32;
    // Fixed position just under the top bar, left-aligned under the band
    // label. lv_obj_get_coords(s_band_label, ...) was unreliable here (the
    // LVGL software-rotation pipeline can return stale/incorrect layout
    // coords), causing the panel to think it had far less room than the
    // ~660px actually available and clamp/scroll away entries (e.g. 15m).
    int panel_x = -2;
    int panel_y = TOP_BAR_H + 4;
    int max_h = DISPLAY_V_RES - panel_y - 4;
    bool needs_scroll = panel_h > max_h;
    if (needs_scroll) panel_h = max_h;

    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, panel_x, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_min_height(panel, 0, 0);
    lv_obj_set_style_min_width(panel, 0, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_set_style_pad_column(panel, 0, 0);
    if (needs_scroll) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    } else {
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    uint32_t cur_hz = cat_get_frequency();
    for (int i = 0; i < band_count; i++) {
        bool active = (cur_hz >= bands[i].center_hz - 1000000 &&
                       cur_hz <= bands[i].center_hz + 1000000);
        lv_obj_t *btn = lv_obj_create(panel);
        lv_obj_set_size(btn, panel_w, btn_h);
        lv_obj_set_style_min_height(btn, 0, 0);
        lv_obj_set_style_min_width(btn, 0, 0);
        lv_obj_set_style_max_height(btn, btn_h, 0);
        lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x2A2A00) : lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, band_preset_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)bands[i].center_hz);
        lv_obj_t *lbl = lv_label_create(btn);
        char bstr[12];
        snprintf(bstr, sizeof(bstr), "%sm", bands[i].name);
        lv_label_set_text(lbl, bstr);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, active ? lv_color_hex(UI_COLOR_ACCENT_GOLD) : lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);
    }
}

static void band_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    // Top-bar dropdowns are inert in FT8 mode - their oversized hit-zones
    // overlap the FT8 screen's own controls (e.g. the FT8 freq preset
    // popup), which would otherwise win the tap.
    if (ui_mode_get() == UI_MODE_FT8) return;
    band_popup_open();
}

// ---- Frequency entry keypad --------------------------------------------
// Phone-style keypad for direct frequency entry. Type a plain decimal
// number (e.g. "1.5" or "200.45"), then tap MHz or kHz to interpret that
// number in the chosen unit and convert it to a Hz value in the display
// (e.g. "1.5" + MHz -> 1500000, i.e. 1.500.000 Hz; "200.45" + kHz ->
// 200450, i.e. 200.450 Hz). Tap Enter to send the displayed Hz value to
// the QMX. No clamping/validation - the value is sent as-is via
// cat_set_frequency().
static lv_obj_t *s_freq_popup   = NULL;
static lv_obj_t *s_freq_display = NULL;
static char      s_freq_buf[16] = "";
static char      s_freq_disp[40] = "";
static ui_freq_picker_cb_t s_freq_picker_cb = NULL;

// Keypad digit layout: false = phone (1 2 3 top), true = 10-key/calculator
// (7 8 9 top). Toggled by the "10 Key"/"Phone" button. Persists per session.
static bool      s_freq_calc_layout = false;
static lv_obj_t *s_freq_digit_lbls[9];   // the nine digit-key labels (grid 0..8)
static lv_obj_t *s_freq_layout_lbl = NULL;  // label on the layout-toggle button

// Apply the current layout to the nine digit labels + the toggle button text.
static void freq_apply_keypad_layout(void)
{
    static const char *const phone[9] = { "1","2","3","4","5","6","7","8","9" };
    static const char *const calc[9]  = { "7","8","9","4","5","6","1","2","3" };
    const char *const *L = s_freq_calc_layout ? calc : phone;
    for (int i = 0; i < 9; i++) {
        if (s_freq_digit_lbls[i]) lv_label_set_text(s_freq_digit_lbls[i], L[i]);
    }
    // Button shows the layout it will switch TO when pressed.
    if (s_freq_layout_lbl) lv_label_set_text(s_freq_layout_lbl,
                                             s_freq_calc_layout ? "Phone" : "10 Key");
}

// Mode row on the freq keypad: DiGi / USB / LSB / CW. Selected mode is
// highlighted yellow and travels with the typed frequency.
static const char *const s_freq_modes[4] = {"DiGi", "USB", "LSB", "CW"};
static lv_obj_t *s_freq_mode_btns[4];
static char      s_freq_mode_sel[8] = "";

static void freq_mode_highlight(void)
{
    for (int i = 0; i < 4; i++) {
        if (!s_freq_mode_btns[i]) continue;
        bool sel = (strcmp(s_freq_modes[i], s_freq_mode_sel) == 0);
        // Dim yellow, same intensity as the Cancel/Enter buttons.
        lv_obj_set_style_bg_color(s_freq_mode_btns[i],
            sel ? lv_color_hex(0x55502A) : lv_color_hex(UI_COLOR_KEY_BG), 0);
    }
}

static void freq_mode_cb(lv_event_t *e)
{
    const char *mode = (const char *)lv_event_get_user_data(e);
    strncpy(s_freq_mode_sel, mode, sizeof(s_freq_mode_sel) - 1);
    s_freq_mode_sel[sizeof(s_freq_mode_sel) - 1] = '\0';
    freq_mode_highlight();
}

static void freq_popup_close(void)
{
    if (s_freq_popup) { lv_obj_delete(s_freq_popup); s_freq_popup = NULL; s_freq_display = NULL; }
}

// Full rebuild of s_freq_disp from s_freq_buf (standard right-anchored
// thousands grouping, e.g. "14074000" -> "14.074.000 Hz"). Called after any
// key except Delete. Delete instead trims s_freq_disp by one character in
// place (see freq_key_cb) so already-displayed digits/"." don't reflow.
static void freq_popup_refresh_display(void)
{
    if (!s_freq_display) return;
    if (!s_freq_buf[0]) {
        s_freq_disp[0] = '\0';
        lv_label_set_text(s_freq_display, "Enter freq");
        return;
    }
    if (strchr(s_freq_buf, '.')) {
        // Still typing a raw "MHz.kHz.Hz"-style number for MHz/kHz conversion.
        strncpy(s_freq_disp, s_freq_buf, sizeof(s_freq_disp) - 1);
        s_freq_disp[sizeof(s_freq_disp) - 1] = '\0';
        lv_label_set_text(s_freq_display, s_freq_disp);
        return;
    }
    // Pure-digit Hz value (after MHz/kHz conversion or plain Hz entry):
    // group every 3 digits from the right, e.g. "14074000" -> "14.074.000".
    size_t len = strlen(s_freq_buf);
    int oi = 0;
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) s_freq_disp[oi++] = '.';
        s_freq_disp[oi++] = s_freq_buf[i];
    }
    s_freq_disp[oi] = '\0';
    lv_label_set_text(s_freq_display, s_freq_disp);
}

static void freq_overlay_cb(lv_event_t *e)
{
    (void)e;
    freq_popup_close();
    if (s_freq_picker_cb) {
        ui_freq_picker_cb_t cb = s_freq_picker_cb;
        s_freq_picker_cb = NULL;
        cb(0, s_freq_mode_sel, false);
    }
}

// Parse s_freq_buf ("MHz[.kHz[.Hz]]") into a frequency in Hz.
// The kHz/Hz blocks (after the 1st/2nd ".") are zero-filled on the
// RIGHT up to 3 digits - e.g. "1.5" -> 1.500 MHz, not 1.005 MHz - so
// typed digits land in the most-significant positions of each block.
static uint32_t freq_buf_to_hz(const char *buf)
{
    uint32_t groups[3] = {0, 0, 0};
    int gdigits[3] = {0, 0, 0};
    int gi = 0;
    const char *p = buf;
    while (*p && gi < 3) {
        if (*p == '.') {
            gi++;
        } else if (*p >= '0' && *p <= '9' && gdigits[gi] < 3) {
            groups[gi] = groups[gi] * 10 + (uint32_t)(*p - '0');
            gdigits[gi]++;
        }
        p++;
    }
    if (gi == 0) {
        // No "." entered at all: treat the whole number as Hz directly.
        return groups[0];
    }
    for (int g = 1; g < 3; g++) {
        for (int d = gdigits[g]; d < 3; d++) groups[g] *= 10;
    }
    return groups[0] * 1000000UL + groups[1] * 1000UL + groups[2];
}

// True if the digit group currently being typed (after the last "." if
// any) already has 3 digits - each MHz.kHz.Hz block is capped at 3.
static bool freq_buf_group_full(const char *buf)
{
    const char *last_dot = strrchr(buf, '.');
    if (!last_dot) return false;
    size_t seg_digits = strlen(last_dot + 1);
    return seg_digits >= 3;
}

static void freq_key_cb(lv_event_t *e)
{
    char key = (char)(intptr_t)lv_event_get_user_data(e);
    size_t len = strlen(s_freq_buf);

    switch (key) {
        case 'C':  // Cancel
            freq_popup_close();
            if (s_freq_picker_cb) {
                ui_freq_picker_cb_t cb = s_freq_picker_cb;
                s_freq_picker_cb = NULL;
                cb(0, s_freq_mode_sel, false);
            }
            return;
        case 'E': {  // Enter
            if (s_freq_picker_cb) {
                uint32_t target_hz = s_freq_buf[0] ? freq_buf_to_hz(s_freq_buf) : 0;
                ui_freq_picker_cb_t cb = s_freq_picker_cb;
                s_freq_picker_cb = NULL;
                freq_popup_close();
                cb(target_hz, s_freq_mode_sel, true);
                return;
            }
            if (s_freq_buf[0]) {
                uint32_t target_hz = freq_buf_to_hz(s_freq_buf);
                esp_err_t err = cat_set_frequency(target_hz);
                ESP_LOGI("ui", "Freq keypad: '%s' -> %lu Hz (err=0x%x)",
                         s_freq_buf, (unsigned long)target_hz, err);
                if (err == ESP_OK) {
                    ui_update_frequency(target_hz);
                }
            }
            if (s_freq_mode_sel[0] && strcmp(s_freq_mode_sel, cat_get_mode_str()) != 0) {
                // cat_set_frequency() above just consumed the 200ms CAT
                // rate-limit slot; wait it out so this MD command isn't
                // silently dropped.
                vTaskDelay(pdMS_TO_TICKS(210));
                cat_set_mode(s_freq_mode_sel);
            }
            freq_popup_close();
            return;
        }
        case 'T':  // toggle keypad layout (phone <-> 10-key)
            s_freq_calc_layout = !s_freq_calc_layout;
            freq_apply_keypad_layout();
            return;
        case '#': {  // a digit key - read the digit from the button's own label
                     // (labels move when the layout toggles, codes don't).
            lv_obj_t *btn = lv_event_get_target(e);
            lv_obj_t *lbl = btn ? lv_obj_get_child(btn, 0) : NULL;
            const char *t = lbl ? lv_label_get_text(lbl) : NULL;
            if (t && t[0] >= '0' && t[0] <= '9' &&
                len + 1 < sizeof(s_freq_buf) &&
                !freq_buf_group_full(s_freq_buf)) {
                s_freq_buf[len] = t[0];
                s_freq_buf[len + 1] = '\0';
            }
            break;
        }
        case 'D': {  // Delete (backspace)
            // Trim the displayed string by one character in place, without
            // reformatting/reflowing the rest.
            size_t dlen = strlen(s_freq_disp);
            if (dlen == 0) return;
            char removed = s_freq_disp[dlen - 1];
            s_freq_disp[dlen - 1] = '\0';
            if (removed != '.') {
                size_t blen = strlen(s_freq_buf);
                if (blen > 0) s_freq_buf[blen - 1] = '\0';
            }
            lv_label_set_text(s_freq_display, s_freq_disp[0] ? s_freq_disp : "Enter freq");
            return;
        }
        case 'A':  // long-press Delete: clear everything
            s_freq_buf[0] = '\0';
            s_freq_disp[0] = '\0';
            lv_label_set_text(s_freq_display, "Enter freq");
            return;
        case '.':
            // Allow at most 2 dots (3 groups: MHz.kHz.Hz)
            {
                int dots = 0;
                for (const char *q = s_freq_buf; *q; q++) if (*q == '.') dots++;
                if (dots >= 2) break;
            }
            if (len + 1 < sizeof(s_freq_buf)) { s_freq_buf[len] = '.'; s_freq_buf[len + 1] = '\0'; }
            break;
        default:  // digit
            if (len + 1 < sizeof(s_freq_buf) && !freq_buf_group_full(s_freq_buf)) {
                s_freq_buf[len] = key; s_freq_buf[len + 1] = '\0';
            }
            break;
    }
    freq_popup_refresh_display();
}

static void freq_popup_build(void)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, freq_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_freq_popup = ov;

    // The DiGi/USB/LSB/CW mode row is only useful for the Memory channel
    // editor (which sets s_freq_picker_cb). The top-bar Freq keypad already
    // has a mode selector in the top bar, so it omits the row (and is shorter).
    bool show_mode = (s_freq_picker_cb != NULL);

    int panel_w = 504;  // 360 + 40%
    int panel_h = show_mode ? 580 : 508;  // 508 = 580 - mode row (64 + 8 gap)
    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Swallow taps on the panel so they don't fall through to the overlay
    // (which would close the popup).
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    int content_w = panel_w - 2 * 12;

    s_freq_display = lv_label_create(panel);
    lv_label_set_text(s_freq_display, "Enter freq");
    lv_obj_set_style_text_color(s_freq_display, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_text_font(s_freq_display, &lv_font_montserrat_32, 0);
    lv_obj_set_size(s_freq_display, content_w, 48);
    lv_obj_set_style_text_align(s_freq_display, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_freq_display, LV_ALIGN_TOP_MID, 0, 0);

    // 3x4 keypad grid. The nine digit cells (0..8) carry code '#': the actual
    // digit is read from the button's label at press time, so the layout
    // toggle just relabels them. Bottom row is fixed: . 0 <-
    static const char *const keys[12] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        ".", "0", LV_SYMBOL_LEFT,
    };
    static const char keycodes[12] = {
        '#', '#', '#',
        '#', '#', '#',
        '#', '#', '#',
        '.', '0', 'D',
    };

    int grid_top = 48 + 12;
    int cols = 3, rows = 4;
    int gap = 8;
    int cell_w = (content_w - (cols - 1) * gap) / cols;
    int cell_h = 64;

    for (int i = 0; i < 12; i++) {
        int col = i % cols;
        int row = i / cols;
        lv_obj_t *btn = lv_btn_create(panel);
        lv_obj_set_size(btn, cell_w, cell_h);
        lv_obj_set_pos(btn, col * (cell_w + gap), grid_top + row * (cell_h + gap));
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_add_event_cb(btn, freq_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)keycodes[i]);
        if (keycodes[i] == 'D') {
            // Long-press Delete clears the whole entry.
            lv_obj_add_event_cb(btn, freq_key_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)'A');
        }
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, keys[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
        if (i < 9) s_freq_digit_lbls[i] = lbl;  // for layout toggle
    }

    int btn_w = (content_w - gap) / 2;
    int btn_h = 64;

    // Row: [10 Key / Phone layout toggle]  [Clear].
    int unit_y = grid_top + rows * (cell_h + gap);

    lv_obj_t *layout_btn = lv_btn_create(panel);
    lv_obj_set_size(layout_btn, btn_w, btn_h);
    lv_obj_set_pos(layout_btn, 0, unit_y);
    lv_obj_set_style_bg_color(layout_btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_radius(layout_btn, 6, 0);
    lv_obj_add_event_cb(layout_btn, freq_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)'T');
    s_freq_layout_lbl = lv_label_create(layout_btn);
    lv_label_set_text(s_freq_layout_lbl, "10 Key");
    lv_obj_set_style_text_font(s_freq_layout_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_freq_layout_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_freq_layout_lbl);

    lv_obj_t *clear_btn = lv_btn_create(panel);
    lv_obj_set_size(clear_btn, btn_w, btn_h);
    lv_obj_set_pos(clear_btn, btn_w + gap, unit_y);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_radius(clear_btn, 6, 0);
    lv_obj_add_event_cb(clear_btn, freq_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)'A');
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear");
    lv_obj_set_style_text_font(clear_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(clear_lbl);

    // Reflect the persisted layout choice on the freshly-built digit keys.
    freq_apply_keypad_layout();

    // Mode row: DiGi / USB / LSB / CW. Only for the Memory channel editor;
    // the top-bar keypad omits it (mode lives in the top bar there).
    int mode_y = unit_y + btn_h + gap;
    if (show_mode) {
        int mode_w = (content_w - 3 * gap) / 4;
        for (int i = 0; i < 4; i++) {
            lv_obj_t *btn = lv_btn_create(panel);
            lv_obj_set_size(btn, mode_w, btn_h);
            lv_obj_set_pos(btn, i * (mode_w + gap), mode_y);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_add_event_cb(btn, freq_mode_cb, LV_EVENT_CLICKED, (void *)s_freq_modes[i]);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, s_freq_modes[i]);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
            lv_obj_center(lbl);
            s_freq_mode_btns[i] = btn;
        }
        freq_mode_highlight();
    } else {
        for (int i = 0; i < 4; i++) s_freq_mode_btns[i] = NULL;
    }

    // Cancel / Enter row - directly below the unit row when there's no mode row.
    int btn_y = (show_mode ? mode_y + btn_h + gap : unit_y + btn_h + gap);

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, btn_w, btn_h);
    lv_obj_set_pos(cancel_btn, 0, btn_y);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(UI_COLOR_DANGER_BORDER), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, freq_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)'C');
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *enter_btn = lv_btn_create(panel);
    lv_obj_set_size(enter_btn, btn_w, btn_h);
    lv_obj_set_pos(enter_btn, btn_w + gap, btn_y);
    lv_obj_set_style_bg_color(enter_btn, lv_color_hex(UI_COLOR_SUCCESS), 0);
    lv_obj_set_style_border_color(enter_btn, lv_color_hex(UI_COLOR_SUCCESS_BORDER), 0);
    lv_obj_set_style_border_width(enter_btn, 2, 0);
    lv_obj_set_style_radius(enter_btn, 6, 0);
    lv_obj_add_event_cb(enter_btn, freq_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)'E');
    lv_obj_t *enter_lbl = lv_label_create(enter_btn);
    lv_label_set_text(enter_lbl, "Save");
    lv_obj_set_style_text_font(enter_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(enter_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(enter_lbl);

    freq_popup_refresh_display();
}

static void freq_popup_open(void)
{
    if (s_freq_popup) { freq_popup_close(); return; }
    s_freq_picker_cb = NULL;
    // Top-bar Freq keypad opens with an empty field (type the new dial freq
    // from scratch). Memory's picker (ui_freq_picker_open) pre-fills instead.
    s_freq_buf[0] = '\0';
    const char *mode = cat_get_mode_str();
    strncpy(s_freq_mode_sel, mode[0] ? mode : "", sizeof(s_freq_mode_sel) - 1);
    s_freq_mode_sel[sizeof(s_freq_mode_sel) - 1] = '\0';
    freq_popup_build();
}

static void freq_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    freq_popup_open();
}

void ui_freq_picker_open(uint32_t initial_hz, const char *initial_mode, ui_freq_picker_cb_t cb)
{
    if (s_freq_popup) freq_popup_close();
    s_freq_picker_cb = cb;
    snprintf(s_freq_buf, sizeof(s_freq_buf), "%lu", (unsigned long)initial_hz);
    strncpy(s_freq_mode_sel, initial_mode && initial_mode[0] ? initial_mode : "", sizeof(s_freq_mode_sel) - 1);
    s_freq_mode_sel[sizeof(s_freq_mode_sel) - 1] = '\0';
    freq_popup_build();
}

// ---- BW preset popup --------------------------------------------------
static lv_obj_t *s_bw_popup = NULL;
static lv_obj_t *s_bw_label;  // forward ref

static void bw_popup_close(void)
{
    if (s_bw_popup) { lv_obj_delete(s_bw_popup); s_bw_popup = NULL; }
}

static void bw_preset_cb(lv_event_t *e)
{
    uint32_t hz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    bw_popup_close();
    const char *mode = cat_get_mode_str();
    if (strcmp(mode, "CW") == 0 || strcmp(mode, "CW-R") == 0) {
        cat_send_raw_cmd("MMCW|CW passband=%lu;", (unsigned long)hz);
    } else {
        // QMX Menu Manager: the SSB filter item is "Bandwidth" (NOT "Filter RX",
        // which the QMX silently ignored — confirmed via a live MM query probe
        // that only "MMSSB|Bandwidth;" answered, returning the current value).
        // Defer the write to the poll task (it owns the CDC pipe) so it can't
        // race the FA/MD/FW poll and get garbled into a ?; — that race was why
        // BW changes only worked intermittently. Update the label optimistically;
        // the FW poll confirms/corrects it within ~150 ms.
        cat_request_ssb_bandwidth(hz);
        ui_update_passband_width(hz);
    }
}

static void bw_overlay_cb(lv_event_t *e)
{
    (void)e;
    bw_popup_close();
}

static void bw_label_clicked_cb(lv_event_t *e);
static void bw_popup_open(void)
{
    if (s_bw_popup) { bw_popup_close(); return; }

    // BW adjustable in CW/CW-R and USB/LSB modes via CAT
    static const uint32_t cw_bw[]   = {50, 100, 150, 200, 250, 300, 400, 500};
    static const char    *cw_lbl[]  = {"50","100","150","200","250","300","400","500"};
    static const uint32_t ssb_bw[]  = {2500, 2700, 2900, 3200};
    static const char    *ssb_lbl[] = {"2.5k","2.7k","2.9k","3.2k"};
    const char *cur_mode = cat_get_mode_str();
    const uint32_t *bw_list;
    const char **lbl_list;
    int n_bw;
    if (strcmp(cur_mode, "CW") == 0 || strcmp(cur_mode, "CW-R") == 0) {
        bw_list = cw_bw;
        lbl_list = cw_lbl;
        n_bw = 8;
    } else if (strcmp(cur_mode, "USB") == 0 || strcmp(cur_mode, "LSB") == 0) {
        bw_list = ssb_bw;
        lbl_list = ssb_lbl;
        n_bw = 4;
    } else {
        return;
    }

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, bw_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_bw_popup = ov;

    int btn_h = 64;
    int panel_w = 140;
    int panel_h = n_bw * btn_h + 4;
    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_style_pad_top(panel, 0, 0);
    lv_obj_set_style_pad_bottom(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 0, 0);
    lv_obj_set_style_pad_right(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_area_t la;
    lv_obj_get_coords(s_bw_label, &la);
    int label_cx = (la.x1 + la.x2) / 2;
    lv_obj_set_pos(panel, label_cx - panel_w / 2 - 10, 60);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    uint32_t cur_bw = ui_get_passband_width_hz();
    for (int i = 0; i < n_bw; i++) {
        bool active = (cur_bw == bw_list[i]);
        lv_obj_t *btn = lv_obj_create(panel);
        lv_obj_set_size(btn, panel_w, btn_h);
        lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x2A2A00) : lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, bw_preset_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)bw_list[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, lbl_list[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, active ? lv_color_hex(UI_COLOR_ACCENT_GOLD) : lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);
    }
}

static void bw_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (ui_mode_get() == UI_MODE_FT8) return;
    bw_popup_open();
}

// ---- Mode preset popup ------------------------------------------------
static lv_obj_t *s_mode_popup = NULL;
static lv_obj_t *s_mode_label;  // forward ref — defined below with other label statics

static void mode_popup_close(void)
{
    if (s_mode_popup) { lv_obj_delete(s_mode_popup); s_mode_popup = NULL; }
}

static void mode_preset_cb(lv_event_t *e)
{
    const char *mode = (const char *)lv_event_get_user_data(e);
    mode_popup_close();
    cat_set_mode(mode);
}

static void mode_overlay_cb(lv_event_t *e)
{
    (void)e;
    mode_popup_close();
}

static void mode_label_clicked_cb(lv_event_t *e);
static void mode_popup_open(void)
{
    if (s_mode_popup) { mode_popup_close(); return; }

    static const char *modes[] = {"USB", "LSB", "CW", "DiGi"};
    int n_modes = 4;

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, mode_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_mode_popup = ov;

    int btn_h = 64;
    int panel_w = 140;
    int panel_h = n_modes * btn_h + 4;  // +4 so flex fits all rows
    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_style_pad_top(panel, 0, 0);
    lv_obj_set_style_pad_bottom(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 0, 0);
    lv_obj_set_style_pad_right(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_area_t la;
    lv_obj_get_coords(s_mode_label, &la);
    int label_cx = (la.x1 + la.x2) / 2;
    lv_obj_set_pos(panel, label_cx - panel_w / 2 - 20, 60);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    const char *cur_mode = cat_get_mode_str();
    for (int i = 0; i < n_modes; i++) {
        bool active = (strcmp(cur_mode, modes[i]) == 0);
        lv_obj_t *btn = lv_obj_create(panel);
        lv_obj_set_size(btn, panel_w, btn_h);
        lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x2A2A00) : lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, mode_preset_cb, LV_EVENT_CLICKED, (void *)modes[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, modes[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, active ? lv_color_hex(UI_COLOR_ACCENT_GOLD) : lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);
    }
}

static void mode_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (ui_mode_get() == UI_MODE_FT8) return;
    mode_popup_open();
}

// ---- Zoom preset popup ------------------------------------------------
static void zoom_popup_open(void);  // forward decl
static void zoom_popup_close(void)
{
    if (s_zoom_popup) { lv_obj_delete(s_zoom_popup); s_zoom_popup = NULL; }
}

static void zoom_preset_cb(lv_event_t *e)
{
    float z = *(float *)lv_event_get_user_data(e);
    zoom_popup_close();
    ui_set_zoom(z, 0);
}

static void zoom_overlay_cb(lv_event_t *e)
{
    (void)e;
    zoom_popup_close();
}

static const float ZOOM_PRESETS[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 24.0f};
static const char *ZOOM_LABELS[]  = {"x1",  "x2",  "x4",  "x8",  "x16", "x24"};
#define N_ZOOM_PRESETS 6

static void zoom_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (ui_mode_get() == UI_MODE_FT8) return;
    zoom_popup_open();
}

static void zoom_popup_open(void)
{
    if (s_zoom_popup) { zoom_popup_close(); return; }  // toggle

    // Full-screen transparent overlay catches outside taps
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, zoom_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_zoom_popup = ov;

    // Popup panel anchored below zoom label (right side, below top bar).
    // Extra 32px margin: covers any flex gap/padding the theme adds between
    // children, so the last row (x24) never gets clipped by the panel edge
    // (same fix as band_popup_open).
    int btn_h = 64;
    int panel_w = 140;
    int panel_h = N_ZOOM_PRESETS * btn_h + 64;
    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    // Fixed position near the right edge, under the top bar. Avoid
    // lv_obj_get_coords(s_zoom_label, ...) - the LVGL software-rotation
    // pipeline can return stale/incorrect layout coords (see band_popup_open).
    lv_obj_set_pos(panel, DISPLAY_H_RES - panel_w - 28, TOP_BAR_H + 4);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    float cur = s_zoom_factor;
    for (int i = 0; i < N_ZOOM_PRESETS; i++) {
        lv_obj_t *btn = lv_obj_create(panel);
        lv_obj_set_size(btn, panel_w, btn_h);
        lv_obj_set_style_bg_color(btn,
            (cur >= ZOOM_PRESETS[i] - 0.1f && cur <= ZOOM_PRESETS[i] + 0.1f)
            ? lv_color_hex(0x2A2A00) : lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, zoom_preset_cb, LV_EVENT_CLICKED,
                            (void *)&ZOOM_PRESETS[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, ZOOM_LABELS[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl,
            (cur >= ZOOM_PRESETS[i] - 0.1f && cur <= ZOOM_PRESETS[i] + 0.1f)
            ? lv_color_hex(UI_COLOR_ACCENT_GOLD) : lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);
    }
}

// Re-center the passband-centered pan offset (zoom > x1) and push it to the
// DSP zoom-FFT. No LVGL calls - safe to call from non-LVGL tasks (e.g. the
// CAT task, when mode/passband width changes).
static void recompute_zoom_pan(void)
{
    if (s_zoom_factor <= 1.0f) return;
    int32_t pb_low_hz, pb_high_hz;
    compute_passband_edges_hz(&pb_low_hz, &pb_high_hz);
    int32_t pb_center_hz = (pb_low_hz + pb_high_hz) / 2;
    float bin_width_hz = (float)DSP_SAMPLE_RATE_HZ / (float)DSP_FFT_SIZE;
    s_pan_offset_bins = (int)lroundf((float)pb_center_hz / bin_width_hz);
    dsp_set_zoom(s_zoom_factor, s_pan_offset_bins, ui_get_if_bin_shift(DSP_FFT_SIZE));
}

void ui_set_zoom(float zoom, int pan_bins)
{
    if (zoom < 1.0f)  zoom = 1.0f;
    if (zoom > 24.0f) zoom = 24.0f;
    // Above x1, center the passband (not the VFO) on screen: pan to the
    // passband's center-frequency bin offset from the VFO.
    if (zoom > 1.0f) {
        int32_t pb_low_hz, pb_high_hz;
        compute_passband_edges_hz(&pb_low_hz, &pb_high_hz);
        int32_t pb_center_hz = (pb_low_hz + pb_high_hz) / 2;
        float bin_width_hz = (float)DSP_SAMPLE_RATE_HZ / (float)DSP_FFT_SIZE;
        pan_bins = (int)lroundf((float)pb_center_hz / bin_width_hz);
    }
    s_zoom_factor     = zoom;
    s_pan_offset_bins = pan_bins;
    settings_set_zoom_factor(zoom);
    dsp_set_zoom(zoom, pan_bins, ui_get_if_bin_shift(DSP_FFT_SIZE));
    // Update zoom label
    if (s_zoom_label) {
        if (zoom <= 1.01f) {
            lv_obj_set_style_text_color(s_zoom_label, lv_color_hex(0xB060E0), 0);
            lv_label_set_text(s_zoom_label, "Zoom: x1.0");
        } else {
            char b[24];
            snprintf(b, sizeof(b), "Zoom: x%.1f", (double)zoom);
            lv_obj_set_style_text_color(s_zoom_label, lv_color_hex(0xB060E0), 0);
            lv_label_set_text(s_zoom_label, b);
        }
    }
}

#define WATERFALL_H     (DISPLAY_V_RES - TOP_BAR_H - SPECTRUM_H - LABEL_BAR_H - BOTTOM_BAR_H)

// Forward declarations (Phase 6.1 - touch-to-tune)
static void touch_event_cb(lv_event_t *e);
static void left_edge_swipe_cb(lv_event_t *e);
static void bottom_edge_swipe_cb(lv_event_t *e);
static void settings_button_cb(lv_event_t *e);  // Phase 5.10D
static void pinch_poll_cb(lv_timer_t *t);
static void update_freq_axis_labels(uint32_t center_hz);
static uint32_t s_last_qmx_freq_hz = 0;  // updated by ui_update_frequency
static char s_current_mode[8] = "USB";  // Phase 5.10F: latest CAT mode for snap-aware tuning
static char s_current_band[8] = "---";  // Phase 9 (v0.9.5): cached band string for web JSON
static uint32_t s_passband_width_hz = 0;  // Phase 5.10G: 0 = use mode default; else from CAT FW
static uint16_t s_cw_pitch_hz = 700;  // CW sidetone offset (Hz); applied to touch-tune in CW modes

// ---- Sticky per-mode settings (v0.16.0) --------------------------------
// Snapshot of freq/mode/passband/zoom taken on leaving a mode, restored
// the next time that mode is entered, so Panadapter and FT8 each keep
// their own independent "where I left it" state across mode swipes.
typedef struct {
    bool     valid;
    uint32_t freq_hz;
    char     mode[8];
    uint32_t passband_hz;
    float    zoom_factor;
    int      pan_offset_bins;
} ui_mode_snapshot_t;

static ui_mode_snapshot_t s_pan_snapshot = {0};
static ui_mode_snapshot_t s_ft8_snapshot = {0};

// CAT writes share a 200 ms TX rate-limiter; restoring freq+mode+bw
// back-to-back would drop all but the first. Stagger them on a 250 ms
// repeating timer (see memory_modal.c's recall_mode_timer_cb for the
// same pattern).
static ui_mode_snapshot_t s_restore_pending;
static int s_restore_step;

static void restore_apply_step(int step)
{
    switch (step) {
    case 0:
        if (s_restore_pending.freq_hz) cat_set_frequency(s_restore_pending.freq_hz);
        break;
    case 1:
        if (s_restore_pending.mode[0]) cat_set_mode(s_restore_pending.mode);
        break;
    case 2:
        if (s_restore_pending.passband_hz) {
            if (strcmp(s_restore_pending.mode, "CW") == 0 || strcmp(s_restore_pending.mode, "CW-R") == 0) {
                cat_send_raw_cmd("MMCW|CW passband=%lu;", (unsigned long)s_restore_pending.passband_hz);
            } else if (strcmp(s_restore_pending.mode, "USB") == 0 || strcmp(s_restore_pending.mode, "LSB") == 0) {
                cat_request_ssb_bandwidth(s_restore_pending.passband_hz);
                ui_update_passband_width(s_restore_pending.passband_hz);
            }
        }
        break;
    }
}

static void restore_timer_cb(lv_timer_t *t)
{
    restore_apply_step(s_restore_step);
    s_restore_step++;
    if (s_restore_step > 2) lv_timer_del(t);
}

// Begin restoring a saved mode snapshot: freq immediately, mode +250ms,
// passband +500ms. Also restores zoom/pan immediately (local, no CAT).
static void ui_restore_snapshot(const ui_mode_snapshot_t *snap)
{
    if (!snap->valid) return;
    s_restore_pending = *snap;
    restore_apply_step(0);
    s_restore_step = 1;
    lv_timer_t *t = lv_timer_create(restore_timer_cb, 250, NULL);
    lv_timer_set_repeat_count(t, 2);

    ui_set_zoom(snap->zoom_factor, snap->pan_offset_bins);
}

// Capture the current freq/mode/passband/zoom into a snapshot for the
// mode we're about to leave.
static void ui_save_snapshot(ui_mode_snapshot_t *snap)
{
    snap->valid           = true;
    snap->freq_hz         = cat_get_frequency();
    strncpy(snap->mode, s_current_mode, sizeof(snap->mode) - 1);
    snap->mode[sizeof(snap->mode) - 1] = '\0';
    snap->passband_hz     = s_passband_width_hz;
    snap->zoom_factor     = s_zoom_factor;
    snap->pan_offset_bins = s_pan_offset_bins;
}

uint16_t ui_get_cw_pitch_hz(void) { return s_cw_pitch_hz; }
int16_t  ui_get_if_cal_hz(void)   { return s_cw_cal_hz; }

// Touch-target cursor state (Phase 6.1)
static int s_target_x = -1;
static uint64_t s_target_until_us = 0;
static int64_t s_target_freq_hz = 0;  // snapped absolute freq for the live cursor tooltip
#define TARGET_DISPLAY_MS  600

// Multi-touch / zoom+pan state
extern esp_lcd_touch_handle_t bsp_display_get_touch_handle(void);
static esp_lcd_touch_handle_t s_tp = NULL;  // set in ui_init
static bool     s_pinch_active      = false;
static float    s_pinch_start_zoom  = 1.0f;
static int      s_pinch_start_dist  = 0;
static int      s_pinch_start_pan   = 0;   // pan at pinch start
static int      s_pinch_mid_x       = 0;   // midpoint x at pinch start
static uint64_t s_last_tap_us       = 0;   // for double-tap detection
static int      s_last_tap_x        = -1;
#define DOUBLE_TAP_MS   500
#define DOUBLE_TAP_PX   120

// Right-edge swipe-to-open-drawer (replaces the burger button)
static bool s_edge_swipe_candidate  = false;
static int  s_edge_swipe_start_x    = -1;
#define EDGE_SWIPE_ZONE_PX   30   // touch must start within this many px of the right edge
#define EDGE_SWIPE_MIN_DX    60   // must move left by at least this many px to open the drawer

// Counter-swipe (drag right) closes the open drawer, either starting on the
// drawer itself (drawer_touch_cb) or anywhere on the spectrum/waterfall
// to its left (touch_event_cb, via s_screen_swipe_start_x).
static int  s_drawer_swipe_start_x  = -1;
static int  s_screen_swipe_start_x  = -1;
#define DRAWER_SWIPE_MIN_DX  60

// Left-edge swipe (drag right) toggles Panadapter <-> FT8 mode, and
// bottom-edge swipe (drag up) opens the memory-channel modal. These use
// dedicated always-on-top overlay strips (left_edge_swipe_cb /
// bottom_edge_swipe_cb) so they work regardless of UI mode, unlike the
// spectrum/waterfall-only gestures above.
static int  s_left_edge_swipe_start_x   = -1;
static int  s_bottom_edge_swipe_start_y = -1;
#define BOTTOM_EDGE_ZONE_PX  60
#define EDGE_SWIPE_MIN_DY    60
// (s_last_qmx_freq_hz declared at top of file)

// Widget handles
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_smeter_bar = NULL;
static lv_obj_t *s_band_label = NULL;   // Phase 5.10D: dedicated band slot
static lv_obj_t *s_mode_label = NULL;
static lv_obj_t *s_spectrum_obj = NULL;
static lv_obj_t *s_waterfall_obj = NULL;
static lv_obj_t *s_label_bar = NULL;
static lv_obj_t *s_status_label = NULL;  // legacy: single label, kept for compatibility (unused after Phase 5.13)
static lv_obj_t *s_bot_left   = NULL;
static lv_obj_t *s_bot_batt_icon = NULL;  /* battery glyph, colored by charge level */
static lv_obj_t *s_bot_batt_slash = NULL; /* red diagonal stroke over the glyph when no pack is attached */
static lv_obj_t *s_bot_center_suffix = NULL;
static ui_clock_t s_bot_clock;
static bool       s_bot_clock_valid = false;
static lv_obj_t *s_bot_wifi_ssid = NULL;
static ui_rssi_t s_bot_rssi;
static bool       s_bot_rssi_valid = false;
static lv_obj_t *s_bot_wifi_suffix = NULL;
static lv_obj_t *s_bot_version = NULL; /* firmware version, between battery and clock */
static lv_obj_t *s_burger_btn = NULL;  // right-edge drawer grip handle (kept for foreground move after all UI built)
static lv_obj_t *s_left_edge_grip = NULL;
static lv_obj_t *s_bottom_edge_grip = NULL;
static lv_obj_t *s_switch_iq  = NULL;  // Phase B: IQ balance toggle in settings drawer
static lv_obj_t *s_switch_flat = NULL; // Phase 5.12: flat-spectrum toggle in settings drawer
static lv_obj_t *s_switch_diag = NULL; // diagnostic comms-log toggle in settings drawer

// Phase 5.10D Stage 2: settings drawer state
static lv_obj_t *s_drawer = NULL;
static bool s_drawer_open = false;
// Scrim covers the screen area to the left of the open drawer: blocks touches
// to underlying FT8/Panadapter content and supports a rightward swipe-to-close
// gesture that works regardless of which content is hidden behind it.
static lv_obj_t *s_drawer_scrim = NULL;
static int s_drawer_scrim_swipe_start_x = -1;
// FT8 mode only needs WiFi/Callsign+Grid/Brightness; everything else is
// hidden and those three are restacked near the top. Each drawer section
// lives in its own transparent container so it can be hidden/repositioned
// as a unit. Indices below double as keys into s_drawer_sections[].
#define DRAWER_SEC_IQ         0
#define DRAWER_SEC_FLAT       1
#define DRAWER_SEC_PRESETS    2
#define DRAWER_SEC_WIFI       3
#define DRAWER_SEC_IDENTITY   4
#define DRAWER_SEC_DBRANGE    5
#define DRAWER_SEC_SMOOTHING  6
#define DRAWER_SEC_CW         7
#define DRAWER_SEC_IFCAL      8
#define DRAWER_SEC_BRIGHTNESS 9
#define DRAWER_SEC_CMAP       10
#define DRAWER_SEC_DIAG       11
#define N_DRAWER_SECTIONS     12
static lv_obj_t *s_drawer_sections[N_DRAWER_SECTIONS];
static int       s_drawer_section_y[N_DRAWER_SECTIONS];
// Phase 5.10D Stage 2b: drawer widgets we need to keep handles to
static lv_obj_t *s_slider_db_min = NULL;
static lv_obj_t *s_slider_db_max = NULL;
static lv_obj_t *s_slider_alpha = NULL;
static lv_obj_t *s_lbl_db_min = NULL;
static lv_obj_t *s_lbl_db_max = NULL;
static lv_obj_t *s_lbl_alpha = NULL;
static lv_obj_t *s_slider_cwpitch = NULL;
static lv_obj_t *s_lbl_ifcal    = NULL;
static lv_obj_t *s_slider_ifcal = NULL;
static lv_obj_t *s_lbl_cwpitch = NULL;
static lv_obj_t *s_dropdown_cmap = NULL;
static lv_obj_t *s_slider_brightness = NULL;
static uint8_t s_saved_ui_mode = UI_MODE_PANADAPTER;
static lv_obj_t *s_lbl_brightness = NULL;
static lv_obj_t *s_tune_tooltip  = NULL;  // freq label above finger during tap-to-tune
static lv_obj_t *s_bw_label      = NULL;  // passband width in top bar
static void drawer_preset_normal_cb(lv_event_t *e);
static void drawer_preset_dx_cb(lv_event_t *e);
static void drawer_preset_strong_cb(lv_event_t *e);
static void drawer_wifi_btn_cb(lv_event_t *e);
static void drawer_identity_btn_cb(lv_event_t *e);
static void ui_show_memories(void);
static void ui_toggle_mode(void);
static void drawer_slider_db_min_cb(lv_event_t *e);
static void drawer_slider_db_max_cb(lv_event_t *e);
static void drawer_slider_alpha_cb(lv_event_t *e);
static void drawer_slider_ifcal_cb(lv_event_t *e)
{
    (void)e;
    if (!s_slider_ifcal) return;
    int v = (int)lv_slider_get_value(s_slider_ifcal);
    // Step is 5 Hz on the LVGL side; round to nearest 5 in case of slop.
    int snapped = ((v + (v >= 0 ? 2 : -2)) / 5) * 5;
    if (snapped < -100) snapped = -100;
    if (snapped >  100) snapped =  100;
    ui_set_cw_cal_hz((int16_t)snapped);
    if (s_lbl_ifcal) {
        char b[24];
        snprintf(b, sizeof(b), "CW trim: %+d Hz", snapped);
        lv_label_set_text(s_lbl_ifcal, b);
    }
}

static void drawer_slider_cwpitch_cb(lv_event_t *e);
static void drawer_dropdown_cmap_cb(lv_event_t *e);
static void drawer_dropdown_cmap_open_cb(lv_event_t *e);
static void drawer_slider_brightness_cb(lv_event_t *e);
static void drawer_switch_flat_cb(lv_event_t *e);
static void drawer_switch_diag_cb(lv_event_t *e);
bool ui_get_flat_mode(void);
void ui_set_flat_mode(bool on);
static void drawer_apply_preset(int db_min, int db_max, float alpha);
static void drawer_build(void);
static void drawer_set_ft8_mode(bool ft8);
static void drawer_open(void);
static void drawer_close(void);
static void drawer_anim_x_cb(void *obj, int32_t v);
static void drawer_touch_cb(lv_event_t *e);
static void drawer_scrim_cb(lv_event_t *e);
static void iq_balance_toggle_cb(lv_event_t *e);

// Phase 5.5: static defaults -- manual Ref/Range, user-controlled later
// (internal arbitrary dB scale; ~80=noise floor, ~125=strong signal on test rig)
static float DB_MIN_DISPLAY = -130.0f;  /* dBm, calibrated scale */
static float DB_MAX_DISPLAY = -30.0f;  /* dBm, headroom for S9+40 */

// Forward decl so build_spectrum can call this

static lv_obj_t *s_wf_canvas = NULL;
static uint8_t *s_wf_canvas_buf = NULL;

// Spectrum dB labels (Phase 5.4)
static lv_obj_t *s_db_max_label = NULL;
static lv_obj_t *s_db_min_label = NULL;

// Spectrum canvas (Phase 5.1)
static lv_obj_t *s_spec_canvas = NULL;
static uint8_t *s_spec_canvas_buf = NULL;

// "Breathing" opacity animation for edge-swipe grip handles: fades between
// near-invisible and the normal resting opacity so the user notices the
// hidden swipe handles exist.
static void grip_breathe_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void grip_start_breathing(lv_obj_t *grip)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, grip);
    lv_anim_set_exec_cb(&a, grip_breathe_anim_cb);
    lv_anim_set_values(&a, LV_OPA_10, LV_OPA_60);
    lv_anim_set_time(&a, 1400);
    lv_anim_set_playback_time(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// lv_anim_delete_all() (used by screenshot capture to freeze the UI for a
// stable snapshot) wipes out the infinite-repeat breathing animations on the
// edge-swipe grip handles too. Call this right after a snapshot to resume
// breathing on all three grips.
void ui_restart_edge_grip_anims(void)
{
    if (s_burger_btn) grip_start_breathing(s_burger_btn);
    if (s_left_edge_grip) grip_start_breathing(s_left_edge_grip);
    if (s_bottom_edge_grip) grip_start_breathing(s_bottom_edge_grip);
}

// ==== Top bar ====
static void build_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, DISPLAY_H_RES, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Phase 5.10D: top-bar layout -- Band | Mode | [center: Freq] | S-meter
    s_band_label = lv_label_create(bar);
    lv_label_set_text(s_band_label, "Band: ---");
    lv_obj_set_style_text_color(s_band_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_band_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_band_label, LV_ALIGN_LEFT_MID, 8, 0);
    // Extend the band label's touch target well down into the spectrum
    // area — there's no reason to support tap-to-tune in the top-left
    // corner, so make it easy to hit the band selector instead.
    lv_obj_set_ext_click_area(s_band_label, 110);
    lv_obj_add_flag(s_band_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_band_label, band_label_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_mode_label = lv_label_create(bar);
    lv_label_set_text(s_mode_label, "Mode: USB");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_LEFT_MID, 188, 0);
    lv_obj_set_ext_click_area(s_mode_label, 90);
    lv_obj_add_flag(s_mode_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mode_label, mode_label_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_bw_label = lv_label_create(bar);
    lv_label_set_text(s_bw_label, "BW: ---");
    lv_obj_set_style_text_color(s_bw_label, lv_color_hex(0xC0C0FF), 0);
    lv_obj_set_style_text_font(s_bw_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bw_label, LV_ALIGN_LEFT_MID, 350, 0);
    lv_obj_set_ext_click_area(s_bw_label, 90);
    lv_obj_add_flag(s_bw_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_bw_label, bw_label_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_freq_label = lv_label_create(bar);
    lv_label_set_text(s_freq_label, "Freq: 14.074.000 Hz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_32, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_CENTER, 30, 0);
    lv_obj_add_flag(s_freq_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_freq_label, 20);
    lv_obj_add_event_cb(s_freq_label, freq_label_clicked_cb, LV_EVENT_CLICKED, NULL);

    // S-meter: a tick-labeled scale (S1/3/5/7/9/+20) with a moving bar
    // below it, replacing the old "Signal: SX+Y" text label. Vertically
    // centered in the 60px top bar.
    {
        const int smeter_w = 220;   // bar width; SMETER_MAX maps across this
        const int smeter_h = 56;
        const int smeter_off = 20;  // left margin so the "S1" label isn't clipped
        lv_obj_t *smeter_cont = lv_obj_create(bar);
        lv_obj_set_size(smeter_cont, smeter_w + 40 + smeter_off, smeter_h);  // extra width for label overhang
        lv_obj_align(smeter_cont, LV_ALIGN_CENTER, 353, 4);
        lv_obj_set_style_bg_opa(smeter_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(smeter_cont, 0, 0);
        lv_obj_set_style_pad_all(smeter_cont, 0, 0);
        lv_obj_clear_flag(smeter_cont, LV_OBJ_FLAG_SCROLLABLE);

        // Tick labels positioned by their value on the SMETER_MAX scale
        // (S1=0, S9=48, +20=68 -- see smeter_value_for_units()).
        static const struct { const char *txt; int value; } ticks[] = {
            { "S1",  0 }, { "3", 12 }, { "5", 24 }, { "7", 36 },
            { "9", 48 }, { "+20", 68 },
        };
        for (size_t i = 0; i < sizeof(ticks)/sizeof(ticks[0]); i++) {
            lv_obj_t *lbl = lv_label_create(smeter_cont);
            lv_label_set_text(lbl, ticks[i].txt);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            const int lbl_w = 36;
            lv_obj_set_size(lbl, lbl_w, LV_SIZE_CONTENT);
            int tick_x = smeter_off + ticks[i].value * smeter_w / 68;
            lv_obj_set_pos(lbl, tick_x - lbl_w / 2, 0);
        }

        // Small tick marks just above the bar at S1,S3..S9,+10,+20.
        static const int tick_marks[] = { 0, 6, 12, 18, 24, 30, 36, 42, 48, 58, 68 };
        for (size_t i = 0; i < sizeof(tick_marks)/sizeof(tick_marks[0]); i++) {
            lv_obj_t *t = lv_obj_create(smeter_cont);
            lv_obj_set_size(t, 2, 6);
            lv_obj_set_pos(t, smeter_off + tick_marks[i] * smeter_w / 68, 26);
            lv_obj_set_style_bg_color(t, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(t, 0, 0);
            lv_obj_set_style_radius(t, 0, 0);
            lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        }

        // Moving level bar underneath the scale (half the original height).
        s_smeter_bar = lv_bar_create(smeter_cont);
        lv_obj_set_size(s_smeter_bar, smeter_w, 11);
        lv_obj_set_pos(s_smeter_bar, smeter_off, 34);
        lv_bar_set_range(s_smeter_bar, 0, 68);
        lv_bar_set_value(s_smeter_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_smeter_bar, lv_color_hex(0x303030), 0);
        lv_obj_set_style_bg_opa(s_smeter_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_smeter_bar, 0, 0);
        lv_obj_set_style_radius(s_smeter_bar, 3, 0);
        lv_obj_set_style_bg_color(s_smeter_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_smeter_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_smeter_bar, 3, LV_PART_INDICATOR);
    }

    // Edge-swipe replaces the burger button: a slim grip on the right
    // screen edge hints that the settings drawer lives off-screen there.
    // Tapping the grip also opens the drawer (fallback for the swipe).
    s_burger_btn = lv_obj_create(parent);  /* parent = screen; reused as the grip handle */
    lv_obj_set_size(s_burger_btn, 4, 120);
    lv_obj_align(s_burger_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_burger_btn, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_bg_opa(s_burger_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_burger_btn, 0, 0);
    lv_obj_set_style_radius(s_burger_btn, 5, 0);
    lv_obj_set_style_shadow_width(s_burger_btn, 0, 0);
    lv_obj_set_style_pad_all(s_burger_btn, 0, 0);
    lv_obj_clear_flag(s_burger_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_burger_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_burger_btn, settings_button_cb, LV_EVENT_CLICKED, NULL);
    grip_start_breathing(s_burger_btn);

    // Zoom indicator: amber always.
    s_zoom_label = lv_label_create(bar);
    lv_label_set_text(s_zoom_label, "Zoom: x1.0");
    lv_obj_set_style_text_color(s_zoom_label, lv_color_hex(0xB060E0), 0);
    lv_obj_set_style_text_font(s_zoom_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_zoom_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s_zoom_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_zoom_label, 20);
    lv_obj_add_event_cb(s_zoom_label, zoom_label_clicked_cb, LV_EVENT_CLICKED, NULL);
}

// ==== Spectrum region (Phase 5.1: real-time line graph) ====
static void build_spectrum(lv_obj_t *parent)
{
    s_spectrum_obj = lv_obj_create(parent);
    lv_obj_set_size(s_spectrum_obj, DISPLAY_H_RES, SPECTRUM_H);
    lv_obj_align(s_spectrum_obj, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H);
    lv_obj_set_style_bg_color(s_spectrum_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(s_spectrum_obj, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(s_spectrum_obj, 1, 0);
    lv_obj_set_style_radius(s_spectrum_obj, 0, 0);
    lv_obj_set_style_pad_all(s_spectrum_obj, 0, 0);
    lv_obj_clear_flag(s_spectrum_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1280 x 200 x 2 bytes = 512 KB in PSRAM
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, SPECTRUM_H, 16,
                                         LV_DRAW_BUF_STRIDE_ALIGN);
    s_spec_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_spec_canvas_buf) {
        ESP_LOGE(TAG, "Failed to alloc spectrum canvas (%zu bytes)", buf_size);
        return;
    }
    memset(s_spec_canvas_buf, 0, buf_size);

    s_spec_canvas = lv_canvas_create(s_spectrum_obj);
    lv_obj_add_flag(s_spectrum_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_spectrum_obj, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_spectrum_obj, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_spectrum_obj, touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_canvas_set_buffer(s_spec_canvas, s_spec_canvas_buf,
                         DISPLAY_H_RES, SPECTRUM_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_spec_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Phase 5.4: dB range labels (top-left and bottom-left of spectrum)
    s_db_max_label = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_db_max_label, "");
    lv_obj_set_style_text_color(s_db_max_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_db_max_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_db_max_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_db_max_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_db_max_label, 3, 0);
    lv_obj_align(s_db_max_label, LV_ALIGN_TOP_LEFT, 4, 2);

    s_db_min_label = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_db_min_label, "");
    lv_obj_set_style_text_color(s_db_min_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_db_min_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_db_min_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_db_min_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_db_min_label, 3, 0);
    lv_obj_align(s_db_min_label, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    // Phase 5.5: show static defaults immediately (no autoscale to update them)
    char buf_max[16], buf_min[16];
    snprintf(buf_max, sizeof(buf_max), "%.0f dBm", (double)DB_MAX_DISPLAY);
    snprintf(buf_min, sizeof(buf_min), "%.0f dBm", (double)DB_MIN_DISPLAY);
    lv_label_set_text(s_db_max_label, buf_max);
    lv_label_set_text(s_db_min_label, buf_min);
}

// ==== Label band (Phase 5.3): black strip between spectrum and waterfall with offset ticks ====
static lv_obj_t *s_label_canvas = NULL;
static uint8_t *s_label_canvas_buf = NULL;
// Phase 5.10C: handles for the 5 tick labels under the spectrum, so we
// can update them with actual MHz when the VFO changes.
static lv_obj_t *s_tick_labels[5] = { NULL, NULL, NULL, NULL, NULL };

static void build_label_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    s_label_bar = bar;
    lv_obj_set_size(bar, DISPLAY_H_RES, LABEL_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H + SPECTRUM_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Tick canvas: DISPLAY_H_RES x LABEL_BAR_H, drawn once
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, LABEL_BAR_H, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_label_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (s_label_canvas_buf) {
        memset(s_label_canvas_buf, 0, buf_size);
        // Draw ticks: major at multiples of 12 kHz (top-aligned, taller),
        // minor at multiples of 3 kHz (top-aligned, shorter)
        uint16_t *px = (uint16_t *)s_label_canvas_buf;
        const uint16_t major_color = 0xC618;  // light grey
        const uint16_t minor_color = 0x8410;  // medium grey
        const int center_x = DISPLAY_H_RES / 2;
        const float px_per_khz = (float)DISPLAY_H_RES / 48.0f;  // 26.67 at 1280 / 15 at 720
        for (int khz = -24; khz <= 24; khz += 3) {
            int x = center_x + (int)(khz * px_per_khz);
            if (x < 0 || x >= DISPLAY_H_RES) continue;
            int is_major = (khz % 12 == 0);
            int h = is_major ? 10 : 5;
            uint16_t color = is_major ? major_color : minor_color;
            for (int y = 0; y < h; y++) {
                px[y * DISPLAY_H_RES + x] = color;
            }
        }
        // Thin separator line where the freq-axis band meets the waterfall,
        // matching the dB scale grid-line colour in the spectrum above.
        for (int x = 0; x < DISPLAY_H_RES; x++) {
            px[(LABEL_BAR_H - 1) * DISPLAY_H_RES + x] = 0x4208;
        }
        s_label_canvas = lv_canvas_create(bar);
        lv_canvas_set_buffer(s_label_canvas, s_label_canvas_buf,
                             DISPLAY_H_RES, LABEL_BAR_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_label_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    // Labels sit below the ticks. Phase 5.10C: store handles so the labels
    // can be rewritten with actual MHz on every VFO change.
    const int tick_xs[5] = { 0, 320, 640, 960, 1280 };
    for (int i = 0; i < 5; i++) {
        s_tick_labels[i] = lv_label_create(bar);
        lv_label_set_text(s_tick_labels[i], "--.---");
        lv_obj_set_style_text_color(s_tick_labels[i], lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(s_tick_labels[i], &lv_font_montserrat_18, 0);
        if (i == 0) {
            lv_obj_align(s_tick_labels[i], LV_ALIGN_BOTTOM_LEFT, 2, -3);
        } else if (i == 4) {
            lv_obj_align(s_tick_labels[i], LV_ALIGN_BOTTOM_RIGHT, -2, -3);
        } else {
            lv_obj_align(s_tick_labels[i], LV_ALIGN_BOTTOM_LEFT, tick_xs[i] - 28, -3);
        }
    }
}

// Phase 5.10C: rewrite the 5 tick labels with absolute MHz centered on VFO.
// At 48 kHz span, ticks are at -24/-12/0/+12/+24 kHz. Format as 7.000 / 14.012 etc.
static void update_freq_axis_labels(uint32_t center_hz)
{
    // Zoomed view: full span = sample_rate / zoom_factor.
    // 5 ticks at display positions 0, 1/4, 1/2, 3/4, 1 of display width.
    // Pan shifts the center by pan_bins * (sample_rate / N) Hz.
    int32_t span_hz = (int32_t)(48000.0f / s_zoom_factor);
    int32_t pan_hz  = (int32_t)((int64_t)s_pan_offset_bins * 48000 / DSP_FFT_SIZE);
    // Tick positions: -span/2, -span/4, 0, +span/4, +span/2 relative to panned center.
    for (int i = 0; i < 5; i++) {
        if (!s_tick_labels[i]) continue;
        int32_t offset = pan_hz + (span_hz * (i - 2)) / 4;
        int32_t hz = (int32_t)center_hz + offset;
        if (hz < 0) hz = 0;
        char buf[20];
        // High zoom: show Hz resolution; low zoom: kHz is enough.
        if (span_hz < 10000) {
            // Show MM.KKK.HHH
            snprintf(buf, sizeof(buf), "%lu.%03lu.%03lu",
                     (unsigned long)(hz / 1000000),
                     (unsigned long)((hz / 1000) % 1000),
                     (unsigned long)(hz % 1000));
        } else {
            snprintf(buf, sizeof(buf), "%lu.%03lu",
                     (unsigned long)(hz / 1000000),
                     (unsigned long)((hz / 1000) % 1000));
        }
        lv_label_set_text(s_tick_labels[i], buf);
    }
}

// ==== Waterfall region (placeholder gradient from Phase 1) ====
static void build_waterfall(lv_obj_t *parent)
{
    s_waterfall_obj = lv_obj_create(parent);
    lv_obj_set_size(s_waterfall_obj, DISPLAY_H_RES, WATERFALL_H);
    lv_obj_align(s_waterfall_obj, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H + SPECTRUM_H + LABEL_BAR_H);
    lv_obj_set_style_bg_color(s_waterfall_obj, lv_color_hex(0x000010), 0);
    lv_obj_set_style_border_width(s_waterfall_obj, 0, 0);
    lv_obj_set_style_radius(s_waterfall_obj, 0, 0);
    lv_obj_set_style_pad_all(s_waterfall_obj, 0, 0);
    lv_obj_clear_flag(s_waterfall_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Allocate 2x WATERFALL_H so we can use the "double buffer" scroll trick:
    // new rows are written to both write_head and write_head+WATERFALL_H positions,
    // and the canvas view pointer moves through the buffer instead of memmove'ing.
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, WATERFALL_H * 2, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_wf_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_wf_canvas_buf) {
        ESP_LOGE(TAG, "Failed to alloc waterfall canvas (%zu bytes)", buf_size);
        return;
    }

    s_wf_canvas = lv_canvas_create(s_waterfall_obj);
    lv_obj_add_flag(s_waterfall_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_canvas_set_buffer(s_wf_canvas, s_wf_canvas_buf,
                         DISPLAY_H_RES, WATERFALL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_wf_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Initialize entire 2x buffer to black (waterfall starts empty)
    memset(s_wf_canvas_buf, 0, (size_t)DISPLAY_H_RES * WATERFALL_H * 2 * 2);    lv_obj_invalidate(s_wf_canvas);
}

// ==== Bottom status bar ====
static void build_bottom_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, DISPLAY_H_RES, BOTTOM_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A1014), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 3-zone bottom bar: battery (left), UTC clock (center), WiFi (right).
    // Battery icon is its own label so it can be colored by charge level
    // independently of the percentage/voltage text.
    s_bot_batt_icon = lv_label_create(bar);
    lv_label_set_text(s_bot_batt_icon, "");
    lv_obj_set_style_text_color(s_bot_batt_icon, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_bot_batt_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bot_batt_icon, LV_ALIGN_LEFT_MID, 8, 0);

    // Red diagonal stroke drawn over the battery glyph, shown only when no pack
    // is attached (see ui_set_bottom_battery_absent / battery_present). The
    // points array must persist for the line's lifetime, hence static.
    static lv_point_precise_t batt_slash_pts[2] = { {0, 22}, {22, 2} };
    s_bot_batt_slash = lv_line_create(bar);
    lv_line_set_points(s_bot_batt_slash, batt_slash_pts, 2);
    lv_obj_set_style_line_color(s_bot_batt_slash, lv_color_hex(0xFF5050), 0);
    lv_obj_set_style_line_width(s_bot_batt_slash, 3, 0);
    lv_obj_set_style_line_rounded(s_bot_batt_slash, true, 0);
    lv_obj_align(s_bot_batt_slash, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_flag(s_bot_batt_slash, LV_OBJ_FLAG_HIDDEN);

    s_bot_left = lv_label_create(bar);
    lv_label_set_text(s_bot_left, "");
    lv_obj_set_style_text_color(s_bot_left, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_bot_left, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bot_left, LV_ALIGN_LEFT_MID, 44, 0);

    // UTC clock, centered in the bottom bar. Built from fixed-width
    // per-character cells (ui_clock) so digit-width changes in the
    // proportional font don't make the clock bounce left/right.
    {
        const lv_font_t *font = &lv_font_montserrat_24;
        const lv_coord_t cell_w = 15;
        const lv_coord_t clock_w = 7 * cell_w;  // 6 digit cells + 2 half-width colon cells
        const char *suffix = " UTC";
        lv_coord_t suffix_w = lv_txt_get_width(suffix, strlen(suffix), font, 0);
        lv_coord_t total_w = clock_w + suffix_w;
        lv_coord_t x0 = (DISPLAY_H_RES - total_w) / 2;

        ui_clock_init(&s_bot_clock, bar, x0, 0, font, lv_color_hex(UI_COLOR_TEXT_SECONDARY), cell_w);
        s_bot_clock_valid = true;

        s_bot_center_suffix = lv_label_create(bar);
        lv_label_set_text(s_bot_center_suffix, suffix);
        lv_obj_set_style_text_color(s_bot_center_suffix, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(s_bot_center_suffix, font, 0);
        lv_obj_align(s_bot_center_suffix, LV_ALIGN_LEFT_MID, x0 + clock_w, 0);
    }


    // Firmware version, centered between the battery text and the UTC clock.
    s_bot_version = lv_label_create(bar);
    lv_label_set_text(s_bot_version, "");
    lv_obj_set_style_text_color(s_bot_version, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_bot_version, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bot_version, LV_ALIGN_CENTER, -250, 0);

    // WiFi status: icon+SSID, RSSI, and IP at fixed x positions so the
    // per-second-changing RSSI digits (glyph-width jitter, same issue as
    // the UTC clock) can't shift the icon/SSID to their left. The SSID
    // label is right-aligned in a generous box ending exactly where the
    // RSSI cells start, so a longer SSID pushes the icon further left
    // (rather than being clipped) while the RSSI position stays fixed.
    {
        const lv_font_t *font = &lv_font_montserrat_24;
        const lv_coord_t cell_w = 14;
        const lv_coord_t rssi_w = 3 * cell_w;
        const lv_coord_t suffix_w = 260;   // "dBm  192.168.123.123"
        const lv_coord_t ssid_w = 220;     // icon + SSID, right-aligned
        const lv_coord_t x_rssi = DISPLAY_H_RES - 8 - rssi_w - suffix_w;
        const lv_coord_t x0 = x_rssi - ssid_w;

        s_bot_wifi_ssid = lv_label_create(bar);
        lv_label_set_text(s_bot_wifi_ssid, "");
        lv_obj_set_style_text_color(s_bot_wifi_ssid, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(s_bot_wifi_ssid, font, 0);
        lv_obj_set_pos(s_bot_wifi_ssid, x0, 0);
        lv_obj_set_width(s_bot_wifi_ssid, ssid_w);
        lv_label_set_long_mode(s_bot_wifi_ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_bot_wifi_ssid, LV_TEXT_ALIGN_RIGHT, 0);

        ui_rssi_init(&s_bot_rssi, bar, x_rssi, 0, font, lv_color_hex(UI_COLOR_TEXT_SECONDARY), cell_w);
        s_bot_rssi_valid = true;

        s_bot_wifi_suffix = lv_label_create(bar);
        lv_label_set_text(s_bot_wifi_suffix, "");
        lv_obj_set_style_text_color(s_bot_wifi_suffix, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(s_bot_wifi_suffix, font, 0);
        lv_obj_set_pos(s_bot_wifi_suffix, x0 + ssid_w + rssi_w, 0);
        lv_obj_set_width(s_bot_wifi_suffix, suffix_w);
        lv_label_set_long_mode(s_bot_wifi_suffix, LV_LABEL_LONG_DOT);
    }
}

// ==== Public API ====
// Operator "engraved" signature - a faint vertical watermark near the
// bottom-right corner, reading "Stef OZ1LAV" bottom-to-top (rotated 90 deg
// CCW). Created last (after the edge-swipe strips) so it draws on top of
// the opaque waterfall/bottom-bar that would otherwise hide it there - a
// background-pinned label was tried first but ended up fully covered by
// the waterfall canvas. Non-clickable, so it never intercepts touches
// (LVGL skips non-clickable objects when hit-testing, falling through to
// the edge-swipe strips/grips beneath it).
static void build_signature(lv_obj_t *scr)
{
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Stef OZ1LAV");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x355C8A), 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_60, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);

    // Rotate 90 deg CCW (2700 in LVGL's 0.1-deg clockwise units) about its
    // own center. The rotation is purely visual - the object's hit-test box
    // stays at its unrotated w x h, but it's non-clickable so that's moot.
    // After rotation the visible glyphs occupy an h(wide) x w(tall) rect
    // centered on the same point, so position that center so the rotated
    // rect's right edge sits near the screen's right edge and its bottom
    // edge sits 20px above the screen bottom.
    lv_obj_update_layout(lbl);
    lv_coord_t w = lv_obj_get_width(lbl);
    lv_coord_t h = lv_obj_get_height(lbl);
    lv_obj_set_style_transform_pivot_x(lbl, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(lbl, h / 2, 0);
    lv_obj_set_style_transform_rotation(lbl, 2700, 0);

    lv_coord_t margin = 4;
    lv_coord_t cx = DISPLAY_H_RES - margin - h / 2;
    lv_coord_t cy = DISPLAY_V_RES - 35 - w / 2;
    lv_obj_set_pos(lbl, cx - w / 2, cy - h / 2);
}

void ui_init(lv_display_t *disp)
{
    display_lock(portMAX_DELAY);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_top_bar(scr);
    build_spectrum(scr);
    build_label_bar(scr);
    build_waterfall(scr);
    build_bottom_bar(scr);

    // Pre-build modals at boot, when internal heap is at maximum (~199 KB free).
    // This avoids the fragmentation cliff that breaks modal_build() at runtime
    // (~70 KB free post-services). Modals are show/hide singletons.
    wifi_config_modal_init();
    memory_modal_init();
    identity_config_modal_init();
    onboarding_init();   // builds the first-boot WiFi prompt + schedules the one-time flow

    // Pre-build the settings drawer at boot for the same reason. Drawer is
    // smaller than each modal (~30-50 objects) but still hit the cliff when
    // built lazily at first burger tap (post-WiFi/audio/services).
    drawer_build();

    ft8_screen_view_init(scr);

    display_unlock();

    // Phase 5.10I: ensure the oversized burger sits on top of everything
    if (s_burger_btn) lv_obj_move_foreground(s_burger_btn);

    // Enlarged touch targets for every top-bar dropdown: each label's own
    // ext_click_area doesn't win hit-testing against the spectrum's
    // tap-to-tune handler (different parent/z-order), so add dedicated
    // transparent overlay buttons on top of everything, each spanning the
    // full 200px height from the top of the screen down into the spectrum.
    // No tap-to-tune is needed in the top strip anyway.
    {
        static const struct {
            int x, w;
            lv_event_cb_t cb;
        } hit_zones[] = {
            { 0,    180, band_label_clicked_cb },  // Band
            { 180,  165, mode_label_clicked_cb },  // Mode
            { 345,  165, bw_label_clicked_cb   },  // BW
            { 580,  280, freq_label_clicked_cb },  // Freq
            { 1090, 190, zoom_label_clicked_cb },  // Zoom
        };
        for (size_t i = 0; i < sizeof(hit_zones) / sizeof(hit_zones[0]); i++) {
            lv_obj_t *hit = lv_obj_create(scr);
            lv_obj_set_size(hit, hit_zones[i].w, 200);
            lv_obj_set_pos(hit, hit_zones[i].x, 0);
            lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(hit, 0, 0);
            lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(hit, hit_zones[i].cb, LV_EVENT_CLICKED, NULL);
            lv_obj_move_foreground(hit);
        }
    }

    // Left-edge and bottom-edge gesture strips: transparent overlays kept
    // in the foreground in both Panadapter and FT8 modes (unlike the
    // spectrum/waterfall canvases, which are hidden in FT8 mode).
    // Left edge: swipe right to toggle Panadapter <-> FT8.
    {
        lv_obj_t *strip = lv_obj_create(scr);
        lv_obj_set_size(strip, EDGE_SWIPE_ZONE_PX, DISPLAY_V_RES);
        lv_obj_set_pos(strip, 0, 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(strip, 0, 0);
        lv_obj_set_style_pad_all(strip, 0, 0);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(strip, left_edge_swipe_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(strip, left_edge_swipe_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_move_foreground(strip);

        // Tiny grip handle indicator, vertically centered, flush with the
        // screen's left edge.
        lv_obj_t *grip = lv_obj_create(strip);
        lv_obj_set_size(grip, 4, 120);
        lv_obj_align(grip, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(grip, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_opa(grip, LV_OPA_30, 0);
        lv_obj_set_style_border_width(grip, 0, 0);
        lv_obj_set_style_radius(grip, 5, 0);
        lv_obj_set_style_shadow_width(grip, 0, 0);
        lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(grip, LV_OBJ_FLAG_CLICKABLE);
        grip_start_breathing(grip);
        s_left_edge_grip = grip;
    }
    // Bottom edge: swipe up to open the memory-channel modal.
    {
        lv_obj_t *strip = lv_obj_create(scr);
        lv_obj_set_size(strip, DISPLAY_H_RES, BOTTOM_EDGE_ZONE_PX);
        lv_obj_set_pos(strip, 0, DISPLAY_V_RES - BOTTOM_EDGE_ZONE_PX);
        lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(strip, 0, 0);
        lv_obj_set_style_pad_all(strip, 0, 0);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(strip, bottom_edge_swipe_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(strip, bottom_edge_swipe_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_move_foreground(strip);

        // Tiny grip handle indicator, horizontally centered, flush with the
        // screen's bottom edge.
        lv_obj_t *grip = lv_obj_create(strip);
        lv_obj_set_size(grip, 120, 4);
        lv_obj_align(grip, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(grip, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_opa(grip, LV_OPA_30, 0);
        lv_obj_set_style_border_width(grip, 0, 0);
        lv_obj_set_style_radius(grip, 5, 0);
        lv_obj_set_style_shadow_width(grip, 0, 0);
        lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(grip, LV_OBJ_FLAG_CLICKABLE);
        grip_start_breathing(grip);
        s_bottom_edge_grip = grip;
    }

    // Operator signature watermark - created last so it draws on top of the
    // opaque waterfall/bottom-bar in the bottom-right corner. Non-clickable,
    // so it never steals touches from the edge-swipe strips beneath it.
    build_signature(scr);

    ESP_LOGI(TAG, "UI built: top=%dpx spectrum=%dpx labels=%dpx waterfall=%dpx bottom=%dpx",
             TOP_BAR_H, SPECTRUM_H, LABEL_BAR_H, WATERFALL_H, BOTTOM_BAR_H);
    // Load CW trim from NVS so bin shift is correct from first frame.
    {
        qmx_settings_t s;
        settings_load_all(&s);
        s_cw_cal_hz = s.cw_cal_hz;
        ESP_LOGI(TAG, "CW trim loaded from NVS: %d Hz", (int)s_cw_cal_hz);
        // Load zoom from NVS; pan always resets to 0 on boot.
        if (s.zoom_factor >= 1.0f && s.zoom_factor <= 24.0f)
            s_zoom_factor = s.zoom_factor;
        ui_set_zoom(s_zoom_factor, 0);
        ESP_LOGI(TAG, "Zoom loaded from NVS: %.1f", (double)s_zoom_factor);
        // Apply saved backlight brightness.
        display_set_brightness(s.brightness_pct);
        ESP_LOGI(TAG, "Brightness loaded from NVS: %u%%", (unsigned)s.brightness_pct);
        // Last UI mode (Panadapter/FT8) is restored later by
        // ui_apply_saved_mode(), called from main.c once the FT8/CAT/audio
        // subsystems have been initialized (ft8_screen_view_show() and
        // ft8_self_test() depend on mutexes set up by ft8_screen_init()
        // etc., which haven't run yet at this point in boot).
        s_saved_ui_mode = s.last_ui_mode;
    }
    // Floating freq tooltip shown above finger during tap-to-tune.
    s_tune_tooltip = lv_label_create(lv_screen_active());
    lv_label_set_text(s_tune_tooltip, "");
    lv_obj_set_style_text_color(s_tune_tooltip, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(s_tune_tooltip, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(s_tune_tooltip, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_tune_tooltip, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_tune_tooltip, 4, 0);
    lv_obj_add_flag(s_tune_tooltip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_tune_tooltip);

    // Grab touch handle for multi-touch polling in touch_event_cb.
    s_tp = bsp_display_get_touch_handle();
    ESP_LOGI(TAG, "Touch handle: %s", s_tp ? "OK" : "NULL");
    // Pinch/pan polling timer: 50 ms, reads raw touch driver directly.
    lv_timer_create(pinch_poll_cb, 50, NULL);
}

// Pinch/pan polling timer callback. Runs on LVGL task (core 0) every 50 ms.
// Reads raw touch driver coords directly so two-finger gestures are detected
// independently of LVGL single-pointer event routing.
static void pinch_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_tp) return;
    esp_lcd_touch_read_data(s_tp);
    uint8_t npts = s_tp->data.points;
    if (npts < 2) {
        if (s_pinch_active) {
            ESP_LOGI("pinch", "Pinch end: zoom=%.1f pan=%d", (double)s_zoom_factor, s_pan_offset_bins);
            s_pinch_active = false;
        }
        return;
    }
    // Two fingers detected. Under sw_rotate+LV_DISPLAY_ROTATION_90,
    // raw panel coords are portrait (720x1280). Landscape x = panel y.
    int lx0 = (int)s_tp->data.coords[0].y;
    int lx1 = (int)s_tp->data.coords[1].y;
    int dist = lx1 - lx0;
    if (dist < 0) dist = -dist;
    if (dist < 4) dist = 4;
    int mid_x = (lx0 + lx1) / 2;
    if (!s_pinch_active) {
        s_pinch_active     = true;
        s_pinch_start_dist = dist;
        s_pinch_start_zoom = s_zoom_factor;
        s_pinch_start_pan  = s_pan_offset_bins;
        s_pinch_mid_x      = mid_x;
        ESP_LOGI("pinch", "Pinch start: dist=%d zoom=%.1f", dist, (double)s_zoom_factor);
        return;
    }
    // Update zoom from spread ratio.
    float new_zoom = s_pinch_start_zoom * (float)dist / (float)s_pinch_start_dist;
    if (new_zoom < 1.0f)  new_zoom = 1.0f;
    if (new_zoom > 24.0f) new_zoom = 24.0f;
    // Update pan from midpoint shift.
    int N = DSP_FFT_SIZE;
    int window_bins = (int)((float)N / new_zoom);
    if (window_bins < 4) window_bins = 4;
    int pan_delta_px   = mid_x - s_pinch_mid_x;  // positive = fingers moved right = view moves right = lower freqs
    int pan_delta_bins = (pan_delta_px * window_bins) / DISPLAY_H_RES;
    int new_pan = s_pinch_start_pan + pan_delta_bins;
    int max_pan = (N - window_bins) / 2;
    if (new_pan < -max_pan) new_pan = -max_pan;
    if (new_pan >  max_pan) new_pan =  max_pan;
    ui_set_zoom(new_zoom, new_pan);
    s_target_until_us = 0;  // suppress tune cursor during pinch
}

// Phase 5.10: forward declaration for band_from_freq (defined below)
static const char *band_from_freq(uint32_t freq_hz);
static void update_freq_axis_labels(uint32_t center_hz);  // Phase 5.10C

void ui_update_frequency(uint32_t freq_hz)
{
    // Update per-band session memory.
    {
        int band_count = 0;
        const cat_band_entry_t *bands = cat_get_band_list(&band_count);
        for (int i = 0; i < band_count; i++) {
            if (freq_hz >= bands[i].center_hz - 1500000 &&
                freq_hz <= bands[i].center_hz + 1500000) {
                s_band_last_hz[i] = freq_hz;
                break;
            }
        }
    }
    s_last_qmx_freq_hz = freq_hz;
    // Reset pan to 0 on freq change — new center is the tuned freq.
    s_pan_offset_bins = 0;
    // At zoom > x1, re-derive the passband-centered pan around the new VFO
    // freq (and push it to the DSP zoom-FFT) — otherwise the zoom-FFT keeps
    // centering on the old target while the overlay lines/labels above
    // recompute using pan=0, knocking them out of sync with the spectrum.
    recompute_zoom_pan();
    settings_set_last_vfo(freq_hz);
    if (!s_freq_label) return;
    char buf[32];
    uint32_t mhz = freq_hz / 1000000;
    uint32_t khz = (freq_hz / 1000) % 1000;
    uint32_t hz  = freq_hz % 1000;
    snprintf(buf, sizeof(buf), "Freq: %lu.%03lu.%03lu Hz", mhz, khz, hz);
    if (display_lock(100)) {
        lv_label_set_text(s_freq_label, buf);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_frequency: freq label lock timeout");
    }
    // Phase 5.10: derive band and push to UI
    const char *band = band_from_freq(freq_hz);
    if (band) ui_update_band(band);
    // Phase 5.10C: refresh the frequency axis labels under the spectrum
    if (display_lock(100)) {
        update_freq_axis_labels(freq_hz);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_frequency: axis label lock timeout");
    }
}

// Phase 5.10: map QMX frequency to ham-band name.
// Returns NULL outside known band ranges.
static const char *band_from_freq(uint32_t freq_hz)
{
    if (freq_hz >= 1800000  && freq_hz < 2000000)  return "160m";
    if (freq_hz >= 3500000  && freq_hz < 4000000)  return "80m";
    if (freq_hz >= 5330500  && freq_hz < 5406500)  return "60m";
    if (freq_hz >= 7000000  && freq_hz < 7300000)  return "40m";
    if (freq_hz >= 10100000 && freq_hz < 10150000) return "30m";
    if (freq_hz >= 14000000 && freq_hz < 14350000) return "20m";
    if (freq_hz >= 17900000 && freq_hz < 18500000) return "17m";
    if (freq_hz >= 21000000 && freq_hz < 21450000) return "15m";
    if (freq_hz >= 24890000 && freq_hz < 24990000) return "12m";
    if (freq_hz >= 28000000 && freq_hz < 29700000) return "10m";
    if (freq_hz >= 50000000 && freq_hz < 54000000) return "6m";
    return NULL;
}

void ui_update_mode(const char *mode)
{
    // Phase 5.10F: cache for snap-step lookup in touch handler
    if (mode) {
        bool changed = strncmp(s_current_mode, mode, sizeof(s_current_mode) - 1) != 0;
        strncpy(s_current_mode, mode, sizeof(s_current_mode) - 1);
        s_current_mode[sizeof(s_current_mode) - 1] = '\0';
        // The passband (bw) shape is mode-dependent, so the passband-centered
        // pan offset (zoom > x1) needs to be recomputed once the real mode is
        // known from CAT - otherwise the freq/bw cursor lines stay where they
        // were placed using the boot-time default mode.
        if (changed) {
            recompute_zoom_pan();
        }
    }
    if (!s_mode_label || !mode) return;
    if (display_lock(100)) {
        char buf[32]; snprintf(buf, sizeof(buf), "Mode: %s", mode); lv_label_set_text(s_mode_label, buf);
        lv_obj_invalidate(s_mode_label);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_mode: display_lock timeout for '%s'", mode);
    }
}

// Cheap, side-effect-free band label refresh: derives the band name from
// freq_hz and pushes it via ui_update_band(). Safe to call on every CAT FA
// poll (unlike ui_update_frequency, which resets pan offset and is gated
// on frequency change).
void ui_refresh_band_label(uint32_t freq_hz)
{
    const char *band = band_from_freq(freq_hz);
    if (band) ui_update_band(band);
}

void ui_update_band(const char *band)
{
    if (!s_band_label || !band) return;
    strncpy(s_current_band, band, sizeof(s_current_band) - 1);
    s_current_band[sizeof(s_current_band) - 1] = '\0';
    if (display_lock(100)) {
        char buf[32]; snprintf(buf, sizeof(buf), "Band: %s", band); lv_label_set_text(s_band_label, buf);
        lv_obj_invalidate(s_band_label);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_band: display_lock timeout for '%s'", band);
    }
}

// Band/Mode/BW/Zoom top-bar controls open popups that don't apply in FT8
// mode (frequency/mode/passband there are driven by the FT8 screen itself,
// and zoom is a panadapter-only concept). The click handlers already ignore
// taps while ui_mode == UI_MODE_FT8; this dims the labels too so it's
// visually obvious they're inert.
static void top_bar_set_ft8_dim(bool dim)
{
    lv_opa_t opa = dim ? LV_OPA_30 : LV_OPA_COVER;
    if (s_band_label) lv_obj_set_style_text_opa(s_band_label, opa, 0);
    if (s_mode_label) lv_obj_set_style_text_opa(s_mode_label, opa, 0);
    if (s_bw_label)   lv_obj_set_style_text_opa(s_bw_label, opa, 0);
    if (s_zoom_label) lv_obj_set_style_text_opa(s_zoom_label, opa, 0);
}

// Phase 5.10G: receive passband width from CAT (FW response) or
// fallback to 0 = use per-mode defaults. Called by cat.c.
void ui_update_passband_width(uint32_t hz)
{
    bool changed = (hz != s_passband_width_hz);
    if (hz > 0 && changed) {
        ESP_LOGI("ui", "Passband width = %lu Hz (CAT FW)", (unsigned long)hz);
    }
    s_passband_width_hz = hz;
    // Re-center the passband-centered pan (zoom > x1) now that the real
    // width is known from CAT - keeps the freq/bw cursor lines in sync
    // with the waterfall instead of frozen at the boot-time default.
    if (changed) {
        recompute_zoom_pan();
    }
    if (s_bw_label && display_lock(20)) {
        char buf[20];
        if (hz >= 1000) snprintf(buf, sizeof(buf), "BW: %lu.%01lu kHz", (unsigned long)(hz/1000), (unsigned long)((hz%1000)/100));
        else            snprintf(buf, sizeof(buf), "BW: %lu Hz", (unsigned long)hz);
        lv_label_set_text(s_bw_label, buf);
        display_unlock();
    }
}

// Helper: returns (low, high) passband edges in Hz, relative to VFO,
// from current mode + (optional) CAT FW width. CW is symmetric around VFO,
// USB is +200 to +(200+width), LSB is -(200+width) to -200, etc.
static void compute_passband_edges_hz(int32_t *out_low, int32_t *out_high)
{
    // Mode defaults if CAT FW didn't report
    uint32_t w = s_passband_width_hz;
    int32_t low, high;
    if (strstr(s_current_mode, "CW")) {
        if (w == 0) w = 300;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    } else if (strstr(s_current_mode, "USB")) {
        if (w == 0) w = 2700;
        low = 200;
        high = 200 + (int32_t)w;
    } else if (strstr(s_current_mode, "LSB")) {
        if (w == 0) w = 2700;
        low = -(200 + (int32_t)w);
        high = -200;
    } else if (strstr(s_current_mode, "AM")) {
        if (w == 0) w = 6000;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    } else if (strstr(s_current_mode, "FM")) {
        if (w == 0) w = 10000;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    } else if (strstr(s_current_mode, "DiGi") || strstr(s_current_mode, "RTTY")
               || strstr(s_current_mode, "FT") || strstr(s_current_mode, "DIG")) {
        if (w == 0) w = 2700;
        low = 200;
        high = 200 + (int32_t)w;
    } else {
        // Unknown mode: a small symmetric default
        if (w == 0) w = 2700;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    }
    *out_low = low;
    *out_high = high;
}

// Exported wrapper for render_waterfall.c's noise-floor-within-passband calc.
void ui_get_passband_edges_hz(int32_t *out_low, int32_t *out_high)
{
    compute_passband_edges_hz(out_low, out_high);
}


// Map S-units to the bar's 0..68 scale: S1=0, each S-unit below S9 is 6 dB
// (S9=48), and above S9 each unit is 1 dB up to +20 (matches ui_update_smeter's
// caller: s_units = 9 + dB above S9). Scale tick labels assume this mapping.
static int smeter_value_for_units(int s_units)
{
    if (s_units <= 1) return 0;
    if (s_units <= 9) return (s_units - 1) * 6;
    int v = 48 + (s_units - 9);
    return (v > 68) ? 68 : v;
}

void ui_update_smeter(int s_units)
{
    if (!s_smeter_bar) return;
    if (s_units < 0) s_units = 0;
    if (s_units > 108) s_units = 108;  // clamp at S9+99
    int value = smeter_value_for_units(s_units);
    if (display_lock(100)) {
        lv_bar_set_value(s_smeter_bar, value, LV_ANIM_ON);
        display_unlock();
    }
}


void ui_set_db_range(float db_min, float db_max)
{
    // Clamp to sane bounds
    if (db_max - db_min < 10.0f) return;  // ignore degenerate ranges
    DB_MIN_DISPLAY = db_min;
    DB_MAX_DISPLAY = db_max;
    // Refresh labels so any caller (menu, one-shot auto, NVS load) sees the change
    ui_set_db_labels(db_min, db_max);
}

void ui_set_db_labels(float db_min, float db_max)
{
    if (!s_db_max_label || !s_db_min_label) return;
    char buf_max[16], buf_min[16];
    snprintf(buf_max, sizeof(buf_max), "%.0f dBm", (double)db_max);
    snprintf(buf_min, sizeof(buf_min), "%.0f dBm", (double)db_min);
    if (display_lock(20)) {
        lv_label_set_text(s_db_max_label, buf_max);
        lv_label_set_text(s_db_min_label, buf_min);
        display_unlock();
    }
}

static inline int db_to_y(float db)
{
    if (db < DB_MIN_DISPLAY) db = DB_MIN_DISPLAY;
    if (db > DB_MAX_DISPLAY) db = DB_MAX_DISPLAY;
    float norm = (db - DB_MIN_DISPLAY) / (DB_MAX_DISPLAY - DB_MIN_DISPLAY);
    int y = (int)((1.0f - norm) * (float)(SPECTRUM_H - 1));
    if (y < 0) y = 0;
    if (y > SPECTRUM_H - 1) y = SPECTRUM_H - 1;
    return y;
}

/* Phase 5.12: flat-spectrum mode (per-bin floor tracking) */
#define FLAT_SMOOTH_ALPHA     0.25f
#define FLAT_FLOOR_UP_ALPHA   0.002f
#define FLAT_FLOOR_DOWN_ALPHA 0.08f
#define FLAT_FLOOR_BIAS_DB    6.0f
#define FLAT_RANGE_DB         30.0f
static float *s_flat_smooth = NULL;
static float *s_flat_floor  = NULL;
static bool   s_flat_ready  = false;
static bool   s_flat_mode   = true;  /* TODO: drawer toggle + NVS */

void ui_push_spectrum(const float *bins, int n_bins)
{
    if (!s_spec_canvas_buf || !bins || n_bins <= 0) return;
    if (!display_lock(20)) return;

    uint16_t *px = (uint16_t *)s_spec_canvas_buf;
    static int s_prev_y_top = 0;
    const uint16_t fg = 0x07E0;  // green in RGB565
    const uint16_t grid_color = 0x4208;  // dim grey grid lines

    // Clear canvas to black
    memset(px, 0, (size_t)DISPLAY_H_RES * SPECTRUM_H * 2);

    // Passband band: a faint tint between the passband-edge lines, same hue
    // as those lines but very low "opacity" (blended against the black
    // background). Drawn first so the grid lines and spectrum curve overdraw it.
    {
        int32_t pb_low_hz, pb_high_hz;
        compute_passband_edges_hz(&pb_low_hz, &pb_high_hz);
        int32_t pan_hz_pb = (int32_t)((int64_t)s_pan_offset_bins * 48000 / DSP_FFT_SIZE);
        int32_t span_hz_pb = (int32_t)(48000.0f / s_zoom_factor);
        int edge_x_lo = (int)((int64_t)(pb_low_hz  - pan_hz_pb) * DISPLAY_H_RES / span_hz_pb) + DISPLAY_H_RES / 2;
        int edge_x_hi = (int)((int64_t)(pb_high_hz - pan_hz_pb) * DISPLAY_H_RES / span_hz_pb) + DISPLAY_H_RES / 2;
        if (edge_x_lo > edge_x_hi) { int t = edge_x_lo; edge_x_lo = edge_x_hi; edge_x_hi = t; }
        if (edge_x_lo < 0) edge_x_lo = 0;
        if (edge_x_hi >= DISPLAY_H_RES) edge_x_hi = DISPLAY_H_RES - 1;
        const uint16_t band_color = 0x3188;  // ~25% of BW-label color (0xC0C0FF) over black
        for (int x = edge_x_lo; x <= edge_x_hi; x++) {
            for (int y = 0; y < SPECTRUM_H; y++) {
                px[y * DISPLAY_H_RES + x] = band_color;
            }
        }
    }

    // dB grid lines (Phase 5.3) - draw before spectrum so green overdraws on hits
    // Phase 5.12: suppressed in flat mode (dBm axis meaningless when shown as dB-above-floor)
    if (!s_flat_mode)
    {
        const float grid_dbs[5] = { -120.0f, -100.0f, -80.0f, -60.0f, -40.0f };
        for (int g = 0; g < 5; g++) {
            int gy = db_to_y(grid_dbs[g]);
            if (gy >= 0 && gy < SPECTRUM_H) {
                uint16_t *row = px + gy * DISPLAY_H_RES;
                for (int x = 0; x < DISPLAY_H_RES; x++) row[x] = grid_color;
            }
        }
    }

    int N = n_bins;

    // v0.16.0 zoom-FFT: if a higher-resolution spectrum centered on the pan
    // target is available (zoom >= x2), display that instead, applying only
    // the residual zoom (zoom_factor / decim) on top of it. center_bin=0
    // because the zoom-FFT already mixed the pan target to DC.
    const float *use_bins = bins;
    float eff_zoom = s_zoom_factor;
    int center_bin;
    const float *zoom_spec = (N == DSP_FFT_SIZE) ? dsp_get_zoom_spectrum() : NULL;
    if (zoom_spec) {
        use_bins = zoom_spec;
        eff_zoom = dsp_get_zoom_residual();
        center_bin = 0;
    } else {
        center_bin = ((ui_get_if_bin_shift(N) + s_pan_offset_bins) % N + N) % N;
    }

    /* Phase 5.12: per-bin floor + smooth update, once per frame */
    if (s_flat_mode) {
        if (!s_flat_smooth) {
            s_flat_smooth = heap_caps_malloc(n_bins * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_flat_floor  = heap_caps_malloc(n_bins * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_flat_smooth || !s_flat_floor) s_flat_mode = false;
        }
        if (s_flat_mode) {
            if (!s_flat_ready) {
                float sum = 0.0f;
                for (int b = 0; b < n_bins; b++) { s_flat_smooth[b] = use_bins[b]; sum += use_bins[b]; }
                float avg = sum / (float)n_bins;
                for (int b = 0; b < n_bins; b++) s_flat_floor[b] = avg;
                s_flat_ready = true;
            } else {
                for (int b = 0; b < n_bins; b++) {
                    s_flat_smooth[b] += FLAT_SMOOTH_ALPHA * (use_bins[b] - s_flat_smooth[b]);
                    float d = s_flat_smooth[b] - s_flat_floor[b];
                    float a = (d > 0.0f) ? FLAT_FLOOR_UP_ALPHA : FLAT_FLOOR_DOWN_ALPHA;
                    s_flat_floor[b] += a * d;
                }
            }
        }
    }

    // Zoom+pan: window_bins = how many FFT bins span the display.
    int window_bins = (int)((float)N / eff_zoom);
    if (window_bins < 4) window_bins = 4;
    if (window_bins > N) window_bins = N;
    int bin_start  = center_bin - window_bins / 2;

    for (int x = 0; x < DISPLAY_H_RES; x++) {
        int b = bin_start + (int)((float)x * (float)window_bins / (float)DISPLAY_H_RES);
        int bin = ((b % N) + N) % N;

        int y_top;
        if (s_flat_mode) {
            /* 5-tap spatial smooth on (smooth - floor), then map to flat-axis y. */
            float sum = 0.0f;
            int   cnt = 0;
            for (int dx = -2; dx <= 2; dx++) {
                int xn = x + dx;
                if (xn < 0 || xn >= DISPLAY_H_RES) continue;
                int sn = bin_start + (int)((float)xn * (float)window_bins / (float)DISPLAY_H_RES);
                int bn = ((sn % N) + N) % N;
                sum += s_flat_smooth[bn] - s_flat_floor[bn];
                cnt++;
            }
            float v = sum / (float)cnt - FLAT_FLOOR_BIAS_DB;
            if (v < 0.0f) v = 0.0f;
            if (v > FLAT_RANGE_DB) v = FLAT_RANGE_DB;
            y_top = SPECTRUM_H - 1 - (int)(v * (SPECTRUM_H - 1) / FLAT_RANGE_DB);
        } else {
            y_top = db_to_y(use_bins[bin]);
        }
        if (y_top < 0) y_top = 0;
        if (y_top >= SPECTRUM_H) y_top = SPECTRUM_H - 1;
        // Phase 5.9: continuous spectrum curve. Connect this column's y_top to
        // the previous column's y_top with a bright line, then fill the area
        // below with a dim green (matches docs/panadapter-mockup-ideal.svg).
        const uint16_t fg_dim = 0x01C0;  // ~25% green in RGB565
        // Connect from prev_y to y_top vertically so the curve is continuous,
        // not a series of disconnected column tops.
        int y_a = (x > 0) ? s_prev_y_top : y_top;
        int y_b = y_top;
        int y_lo = (y_a < y_b) ? y_a : y_b;
        int y_hi = (y_a > y_b) ? y_a : y_b;
        for (int y = y_lo; y <= y_hi; y++) {
            px[y * DISPLAY_H_RES + x] = fg;
        }
        // Dim fill from just below the connecting line down to the bottom.
        for (int y = y_hi + 1; y < SPECTRUM_H; y++) {
            px[y * DISPLAY_H_RES + x] = fg_dim;
        }
        s_prev_y_top = y_top;
    }

    // Center cursor: amber 1-px vertical line at canvas center (where QMX is tuned)
    {
        // Phase 5.10G: passband edges (2 px grey lines)
        int32_t pb_low_hz, pb_high_hz;
        compute_passband_edges_hz(&pb_low_hz, &pb_high_hz);
        const uint16_t pb_color = 0xBDFF;  /* matches BW label color (0xC0C0FF) */
        for (int side = 0; side < 2; side++) {
            int32_t edge_hz = (side == 0) ? pb_low_hz : pb_high_hz;
            /* Edge frequency in Hz -> screen x, accounting for zoom and pan. */
            int32_t pan_hz = (int32_t)((int64_t)s_pan_offset_bins * 48000 / DSP_FFT_SIZE);
            int32_t span_hz_pb = (int32_t)(48000.0f / s_zoom_factor);
            int edge_x = (int)((int64_t)(edge_hz - pan_hz) * DISPLAY_H_RES / span_hz_pb) + DISPLAY_H_RES / 2;
            if (edge_x < 0 || edge_x >= DISPLAY_H_RES) continue;
            for (int y = 0; y < SPECTRUM_H; y++) {
                px[y * DISPLAY_H_RES + edge_x] = pb_color;
                if (edge_x + 1 < DISPLAY_H_RES) px[y * DISPLAY_H_RES + edge_x + 1] = pb_color;
            }
        }

        // Amber VFO line: at 0 Hz relative to dial, shifted by pan like the
        // passband edges above so it tracks the actual tuned frequency
        // (no longer screen-center once zoom>x1 re-centers on the passband).
        const uint16_t center_color = 0xFEA0;  /* matches Freq label color (UI_COLOR_ACCENT_GOLD) */
        int32_t pan_hz_vfo = (int32_t)((int64_t)s_pan_offset_bins * 48000 / DSP_FFT_SIZE);
        int32_t span_hz_vfo = (int32_t)(48000.0f / s_zoom_factor);
        int cx = (int)((int64_t)(0 - pan_hz_vfo) * DISPLAY_H_RES / span_hz_vfo) + DISPLAY_H_RES / 2;
        if (cx >= 0 && cx < DISPLAY_H_RES) {
            for (int y = 0; y < SPECTRUM_H; y++) {
                px[y * DISPLAY_H_RES + cx] = center_color;
            }
        }
    }
    // Target cursor: cyan 1-px vertical line at last touched x, ~600 ms
    if (s_target_x >= 0) {
        uint64_t now = esp_timer_get_time();
        if (now < s_target_until_us) {
            const uint16_t target_color = 0x07FF;
            int tx = s_target_x;
            if (tx >= 0 && tx < DISPLAY_H_RES) {
                for (int y = 0; y < SPECTRUM_H; y++) {
                    px[y * DISPLAY_H_RES + tx] = target_color;
                }
            }
            // Update floating freq tooltip.
            if (s_tune_tooltip && s_last_qmx_freq_hz > 0) {
                int64_t tip_hz = s_target_freq_hz;
                if (tip_hz > 0) {
                    char tbuf[24];
                    snprintf(tbuf, sizeof(tbuf), "%lu.%03lu.%03lu",
                        (unsigned long)(tip_hz / 1000000),
                        (unsigned long)((tip_hz / 1000) % 1000),
                        (unsigned long)(tip_hz % 1000));
                    lv_label_set_text(s_tune_tooltip, tbuf);
                    // Position above finger, clamped to screen.
                    int tip_x = tx - 50;
                    if (tip_x < 0) tip_x = 0;
                    if (tip_x > DISPLAY_H_RES - 120) tip_x = DISPLAY_H_RES - 120;
                    int tip_y = TOP_BAR_H + 4;
                    lv_obj_set_pos(s_tune_tooltip, tip_x, tip_y);
                    lv_obj_clear_flag(s_tune_tooltip, LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            s_target_x = -1;
            if (s_tune_tooltip) lv_obj_add_flag(s_tune_tooltip, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_tune_tooltip) {
        lv_obj_add_flag(s_tune_tooltip, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(s_spec_canvas);
    // Update freq axis labels every frame so zoom/pan changes are reflected.
    update_freq_axis_labels(s_last_qmx_freq_hz);
    display_unlock();
}

// Moving-pointer (double-buffer) scroll. We allocate a 2*WATERFALL_H buffer.
// Each tick we write the new row to position s_wf_head AND to s_wf_head + WATERFALL_H,
// then we increment s_wf_head (mod WATERFALL_H). The canvas's buffer pointer is set
// to (base + s_wf_head*row_bytes), giving LVGL a contiguous WATERFALL_H view of the
// freshest data. No memmove needed.
//
// Visual order: row 0 of the LVGL canvas view = newest row (top); WATERFALL_H-1 = oldest.
// To achieve this, after incrementing s_wf_head, the "newest" row in the buffer is at
// (s_wf_head - 1) mod WATERFALL_H, and we want the canvas view to start with it at row 0.
// That means the view base = the position of the newest row.
static int s_wf_head = 0;  // next write position (0..WATERFALL_H-1)

// (touch-target cursor state declared near top of file)

// Waterfall scroll via moving-pointer trick.
// build_waterfall() allocates the canvas buffer at 2x WATERFALL_H height so we
// can write each new row to BOTH s_wf_head and s_wf_head + WATERFALL_H. The
// head DECREMENTS each tick, so the just-written row is at the TOP of the
// WATERFALL_H window that starts at s_wf_head. We point the canvas's buffer at
// that window. No memmove needed; ~100 us/tick instead of ~92 ms.
void ui_push_waterfall_row(const uint8_t *rgb565_row)
{
    if (!s_wf_canvas_buf || !rgb565_row) return;

    const size_t row_bytes = DISPLAY_H_RES * 2;  // RGB565 = 2 B/px

    if (!display_lock(20)) return;

    // Decrement head (with wrap), then write the new row at head and head+WATERFALL_H.
    s_wf_head = (s_wf_head + WATERFALL_H - 1) % WATERFALL_H;
    memcpy(s_wf_canvas_buf +  s_wf_head                * row_bytes, rgb565_row, row_bytes);
    memcpy(s_wf_canvas_buf + (s_wf_head + WATERFALL_H) * row_bytes, rgb565_row, row_bytes);

    // Point the canvas at the WATERFALL_H window starting at s_wf_head.
    // Newest row sits at view row 0 (top), oldest at view row WATERFALL_H-1 (bottom).
    lv_canvas_set_buffer(s_wf_canvas,
                         s_wf_canvas_buf + s_wf_head * row_bytes,
                         DISPLAY_H_RES, WATERFALL_H, LV_COLOR_FORMAT_RGB565);

    // Overlay cyan cursor on the newest waterfall row only.
    // Drawing on all rows would permanently burn cyan into old rows.
    if (s_target_x >= 0 && esp_timer_get_time() < s_target_until_us) {
        const uint16_t cyan = 0x07FF;
        uint16_t *row0 = (uint16_t *)(s_wf_canvas_buf + s_wf_head * row_bytes);
        int tx = s_target_x;
        if (tx >= 0 && tx < DISPLAY_H_RES) {
            row0[tx] = cyan;
            if (tx + 1 < DISPLAY_H_RES) row0[tx + 1] = cyan;
        }
    }
    lv_obj_invalidate(s_wf_canvas);
    display_unlock();
}

void ui_set_bottom_left(const char *text)
{
    if (!s_bot_left) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_left, text ? text : "");
        display_unlock();
    }
}

void ui_set_bottom_battery(const char *icon, uint32_t icon_color_hex, const char *text)
{
    if (!s_bot_batt_icon || !s_bot_left) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_batt_icon, icon ? icon : "");
        lv_obj_set_style_text_color(s_bot_batt_icon, lv_color_hex(icon_color_hex), 0);
        lv_label_set_text(s_bot_left, text ? text : "");
        if (s_bot_batt_slash) lv_obj_add_flag(s_bot_batt_slash, LV_OBJ_FLAG_HIDDEN);
        display_unlock();
    }
}

// No battery pack attached: a static red battery glyph with a diagonal stroke
// through it, and no percentage/voltage text. Latched by battery_present(), so
// it's set once and "left" — no flicker.
void ui_set_bottom_battery_absent(void)
{
    if (!s_bot_batt_icon || !s_bot_left) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
        lv_obj_set_style_text_color(s_bot_batt_icon, lv_color_hex(0xFF5050), 0);
        lv_label_set_text(s_bot_left, "");
        if (s_bot_batt_slash) lv_obj_clear_flag(s_bot_batt_slash, LV_OBJ_FLAG_HIDDEN);
        display_unlock();
    }
}

void ui_set_bottom_version(const char *text)
{
    if (!s_bot_version) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_version, text ? text : "");
        display_unlock();
    }
}

void ui_set_bottom_clock(int h, int m, int s, bool valid)
{
    if (!s_bot_clock_valid) return;
    if (display_lock(20)) {
        if (valid) {
            ui_clock_set_time(&s_bot_clock, h, m, s);
        } else {
            lv_label_set_text(s_bot_clock.cells[0], "-");
            lv_label_set_text(s_bot_clock.cells[1], "-");
            lv_label_set_text(s_bot_clock.cells[3], "-");
            lv_label_set_text(s_bot_clock.cells[4], "-");
            lv_label_set_text(s_bot_clock.cells[6], "-");
            lv_label_set_text(s_bot_clock.cells[7], "-");
        }
        display_unlock();
    }
}

void ui_set_bottom_wifi(const char *icon_ssid, bool show_rssi, int rssi_dbm, const char *suffix)
{
    if (!s_bot_wifi_ssid) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_wifi_ssid, icon_ssid ? icon_ssid : "");
        if (s_bot_rssi_valid) {
            if (show_rssi) {
                ui_rssi_set(&s_bot_rssi, rssi_dbm);
                for (int i = 0; i < 3; i++) lv_obj_clear_flag(s_bot_rssi.cells[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                for (int i = 0; i < 3; i++) lv_obj_add_flag(s_bot_rssi.cells[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_label_set_text(s_bot_wifi_suffix, suffix ? suffix : "");
        display_unlock();
    }
}

void ui_set_fps_text(const char *text)
{
    (void)text;  // legacy no-op; status.c now uses zone setters
    if (!s_status_label) return;
    if (display_lock(20)) {
        lv_label_set_text(s_status_label, text);
        display_unlock();
    }
}










// =============================================================================
// Touch-to-tune (Phase 6.1)
// =============================================================================

#define UAC_SAMPLE_RATE   48000     // I/Q sample rate from QMX
#define DSP_FFT_SIZE_HZ   48000     // full FFT span = sample rate (complex FFT)
#define TUNE_ROUND_HZ     10        // round target frequency to nearest 10 Hz

// (s_last_qmx_freq_hz declared at top of file)

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    // p.x is screen-x; matches canvas-x because spectrum/waterfall are full-width at x=0.
    if (p.x < 0 || p.x >= DISPLAY_H_RES) return;

    if (code == LV_EVENT_PRESSED) {
        // Touch-down near the right screen edge: track as a candidate for
        // the swipe-to-open-drawer gesture instead of the tune cursor.
        s_edge_swipe_candidate = (p.x >= DISPLAY_H_RES - EDGE_SWIPE_ZONE_PX);
        s_edge_swipe_start_x   = (int)p.x;
        // Track every touch-down x so a rightward swipe anywhere on the
        // spectrum/waterfall can close the drawer when it's open.
        s_screen_swipe_start_x = (int)p.x;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (s_pinch_active) return;  // pinch timer owns gesture
        if (s_edge_swipe_candidate) return;  // edge-swipe owns this gesture
        if (s_drawer_open) return;  // possible close-swipe owns this gesture
        // Snap the live cursor to the same mode-aware grid used on release,
        // so the line jumps from snap to snap and shows exactly where the
        // freq will land.
        {
            int dx = (int)p.x - DISPLAY_H_RES / 2;
            int32_t offset_hz = (int32_t)((int64_t)dx * UAC_SAMPLE_RATE / (int)(DISPLAY_H_RES * s_zoom_factor));
            int32_t pan_hz = (int32_t)((int64_t)s_pan_offset_bins * UAC_SAMPLE_RATE / DSP_FFT_SIZE);
            offset_hz += pan_hz;
            int32_t snap = 10;
            if (strstr(s_current_mode, "USB") || strstr(s_current_mode, "LSB")) snap = 250;
            else if (strstr(s_current_mode, "FT") || strstr(s_current_mode, "DIG") || strstr(s_current_mode, "RTTY")
                     || strstr(s_current_mode, "DiGi")) snap = 500;
            else if (strstr(s_current_mode, "AM") || strstr(s_current_mode, "FM")) snap = 1000;
            else if (strstr(s_current_mode, "CW")) snap = 10;
            // Snap the absolute target frequency to the grid (e.g. ...200,
            // 300, 400 Hz), not the touch offset — otherwise the grid is
            // shifted by the VFO's own offset from a snap multiple.
            int64_t target_hz = (int64_t)s_last_qmx_freq_hz + offset_hz;
            int64_t rounded_target = ((target_hz + (target_hz >= 0 ? snap/2 : -snap/2)) / snap) * snap;
            int32_t rounded = (int32_t)(rounded_target - (int64_t)s_last_qmx_freq_hz);
            int32_t snapped_dx = (int32_t)((int64_t)(rounded - pan_hz) * (int)(DISPLAY_H_RES * s_zoom_factor) / UAC_SAMPLE_RATE);
            s_target_x = DISPLAY_H_RES / 2 + snapped_dx;
            s_target_freq_hz = rounded_target;
        }
        s_target_until_us = esp_timer_get_time() + 200000;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        // If releasing a pinch, clear pinch state and skip tune.
        if (s_pinch_active) {
            s_pinch_active = false;
            return;
        }
        // Drawer is open: a rightward swipe anywhere on the spectrum or
        // waterfall closes it, regardless of where it started.
        if (s_drawer_open) {
            int dx = (int)p.x - s_screen_swipe_start_x;
            s_edge_swipe_candidate = false;
            if (dx >= DRAWER_SWIPE_MIN_DX) {
                drawer_close();
            }
            return;
        }
        // Edge-swipe release: open the drawer if dragged far enough left
        // from the right edge, and skip tap-to-tune either way.
        if (s_edge_swipe_candidate) {
            int dx = s_edge_swipe_start_x - (int)p.x;
            s_edge_swipe_candidate = false;
            if (dx >= EDGE_SWIPE_MIN_DX) {
                drawer_open();
            }
            return;
        }
        // Double-tap: reset zoom+pan to 1.0/0.
        uint64_t now_us = esp_timer_get_time();
        if (s_last_tap_x >= 0 &&
            (now_us - s_last_tap_us) < (uint64_t)DOUBLE_TAP_MS * 1000 &&
            abs((int)p.x - s_last_tap_x) < DOUBLE_TAP_PX) {
            ESP_LOGI("ui_touch", "Double-tap: reset zoom+pan");
            ui_set_zoom(1.0f, 0);
            s_last_tap_x = -1;
            return;
        }
        s_last_tap_us = now_us;
        s_last_tap_x  = (int)p.x;
        if (s_last_qmx_freq_hz == 0) return;  // no freq known yet, can't tune
        // Top-bar dropdown deadzones: band/mode/BW/zoom/burger overlay
        // buttons each span the full top 200px (see hit_zones in ui_init).
        // Tap-to-tune isn't needed under those columns; the gap between BW
        // and Zoom (x=510..1090) remains tunable.
        if (p.y < 200 && (p.x < 510 || p.x >= 1090)) {
            ESP_LOGI("ui_touch", "RELEASED in top-bar dropdown deadzone (x=%d y=%d) - ignored", (int)p.x, (int)p.y);
            return;
        }

        // Compute target frequency from final touch position.
        // When zoomed in, each pixel covers fewer Hz; pan shifts the center.
        int dx = (int)p.x - DISPLAY_H_RES / 2;
        // Effective Hz per pixel = (sample_rate / zoom) / display_width
        int32_t offset_hz = (int32_t)((int64_t)dx * UAC_SAMPLE_RATE / (int)(DISPLAY_H_RES * s_zoom_factor));
        // Add pan offset in Hz (pan_bins -> Hz)
        int32_t pan_hz = (int32_t)((int64_t)s_pan_offset_bins * UAC_SAMPLE_RATE / DSP_FFT_SIZE);
        offset_hz += pan_hz;
        // Snap to strongest bin within +/-700 Hz of touch (handler falls through if no peak).
        int32_t snapped_hz = offset_hz;
        // Only snap to peak when not zoomed in — at high zoom the tap is precise enough.
        if (s_zoom_factor <= 1.5f && dsp_find_peak_hz_around(offset_hz, 700, &snapped_hz) == ESP_OK) {
            if (snapped_hz != offset_hz) {
                ESP_LOGI("ui_touch", "snap-to-peak: %ld -> %ld Hz", (long)offset_hz, (long)snapped_hz);
            }
            offset_hz = snapped_hz;
        }
        // Phase 5.10F: mode-aware snap. CW=10 Hz (precision), SSB=250 Hz
        // (voice channels), FT8/data=500 Hz, AM/FM=1 kHz.
        int32_t snap = 10;
        if (strstr(s_current_mode, "USB") || strstr(s_current_mode, "LSB")) snap = 250;
        else if (strstr(s_current_mode, "FT") || strstr(s_current_mode, "DIG") || strstr(s_current_mode, "RTTY")
                 || strstr(s_current_mode, "DiGi")) snap = 500;
        else if (strstr(s_current_mode, "AM") || strstr(s_current_mode, "FM")) snap = 1000;
        else if (strstr(s_current_mode, "CW")) snap = 10;
        // Snap the absolute target frequency to the grid (matches the live
        // cursor preview during PRESSING).
        int64_t target_hz_unrounded = (int64_t)s_last_qmx_freq_hz + offset_hz;
        int64_t target = ((target_hz_unrounded + (target_hz_unrounded >= 0 ? snap/2 : -snap/2)) / snap) * snap;
        int32_t rounded = (int32_t)(target - (int64_t)s_last_qmx_freq_hz);
        if (target < 0) return;
        uint32_t target_hz = (uint32_t)target;

        esp_err_t err = cat_set_frequency(target_hz);
        ESP_LOGI("ui_touch", "RELEASED x=%d dx=%d off=%ld tgt=%lu err=0x%x",
                 (int)p.x, dx, (long)rounded, (unsigned long)target_hz, err);
        // Phase 5.10H: optimistically update the on-screen freq label
        // immediately so the user sees their target before the CAT FA
        // poll confirms (~300 ms later). If the QMX rejects the tune,
        // the next CAT FA will correct the display.
        if (err == ESP_OK) {
            ui_update_frequency(target_hz);
        }
        // Let the cursor linger briefly after release, then clear
        s_target_until_us = esp_timer_get_time() + 200000;
    }
}

// Left-edge swipe (drag right) toggles Panadapter <-> FT8 mode. Lives on a
// dedicated transparent overlay strip in the screen foreground so it works
// in both modes, unlike the spectrum/waterfall handlers above which are
// hidden in FT8 mode.
static void left_edge_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_left_edge_swipe_start_x = (int)p.x;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_left_edge_swipe_start_x >= 0 &&
            (int)p.x - s_left_edge_swipe_start_x >= EDGE_SWIPE_MIN_DX) {
            ui_toggle_mode();
        }
        s_left_edge_swipe_start_x = -1;
    }
}

// Bottom-edge swipe (drag up) opens the memory-channel modal. Same
// always-on-top overlay approach as left_edge_swipe_cb.
static void bottom_edge_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_bottom_edge_swipe_start_y = (int)p.y;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_bottom_edge_swipe_start_y >= 0 &&
            s_bottom_edge_swipe_start_y - (int)p.y >= EDGE_SWIPE_MIN_DY) {
            ui_show_memories();
        }
        s_bottom_edge_swipe_start_y = -1;
    }
}

void ui_set_cw_pitch_hz(uint16_t hz)
{
    if (hz < 300 || hz > 1200) return;  // sanity clamp
    s_cw_pitch_hz = hz;
    settings_set_cw_pitch_hz(hz);
    ESP_LOGI(TAG, "CW pitch set to %u Hz", (unsigned)hz);
    // Sync to QMX CW offset setting via Menu Manager CAT command.
    // With Auto-offset/tone=YES (QMX default), setting CW center also
    // updates CW offset and sidetone to match.
    cat_send_raw_cmd("MMCW|CW center=%u;", (unsigned)hz);
}

// Hook into ui_update_frequency to track latest known QMX frequency


















// Phase 5.10D Stage 2: burger menu click -- toggle settings drawer
static void settings_button_cb(lv_event_t *e)
{
    (void)e;
    if (s_drawer_open) {
        drawer_close();
    } else {
        drawer_open();
    }
}

// Animate x position. Used for slide-in/out.
static void drawer_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

#define MODE_SLIDE_TIME_MS 250

// Generic slide helper: animates obj's x from `from` to `to`.
static void slide_x_anim(lv_obj_t *obj, int32_t from, int32_t to, lv_anim_ready_cb_t ready_cb)
{
    if (!obj) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, drawer_anim_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, MODE_SLIDE_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

// Ready callback for widgets sliding out during a mode-toggle: hide and
// reset x=0 so they're back in place (but hidden) for the next toggle.
static void mode_slide_out_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(obj, 0);
}

// Ready callback for the FT8 container sliding out: use the proper hide API
// (logs + sets HIDDEN), then reset x=0.
static void ft8_slide_out_ready_cb(lv_anim_t *a)
{
    (void)a;
    ft8_screen_view_hide();
    lv_obj_t *ft8 = ft8_screen_view_get_container();
    if (ft8) lv_obj_set_x(ft8, 0);
}

// Counter-swipe (drag right) on the drawer background closes it; replaces
// the close-X button.
static void drawer_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_drawer_swipe_start_x = (int)p.x;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_drawer_swipe_start_x >= 0 &&
            (int)p.x - s_drawer_swipe_start_x >= DRAWER_SWIPE_MIN_DX) {
            drawer_close();
        }
        s_drawer_swipe_start_x = -1;
    }
}

// Counter-swipe (drag right) on the scrim closes the drawer. The scrim covers
// the FT8/Panadapter area to the left of the drawer while it's open, so this
// also restores the close-swipe in FT8 mode where s_spectrum_obj/s_waterfall_obj
// are hidden and can't receive touch_event_cb's close-swipe logic.
static void drawer_scrim_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_drawer_scrim_swipe_start_x = (int)p.x;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_drawer_scrim_swipe_start_x >= 0 &&
            (int)p.x - s_drawer_scrim_swipe_start_x >= DRAWER_SWIPE_MIN_DX) {
            drawer_close();
        }
        s_drawer_scrim_swipe_start_x = -1;
    }
}

static void iq_balance_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    iq_balance_set_enabled(on);
    settings_set_iq_enabled(on);
    if (on) iq_balance_reset();
}

// Create a transparent, full-width, non-scrollable container for one
// drawer section. Children are positioned relative to its (0,0) top-left,
// same as they used to be positioned relative to s_drawer's top-left.
// sec_idx records the container + its normal (Panadapter-mode) y so
// drawer_set_ft8_mode() can hide/restack sections for FT8 mode.
static lv_obj_t *drawer_section(int sec_idx, int y, int h)
{
    lv_obj_t *c = lv_obj_create(s_drawer);
    lv_obj_set_size(c, DRAWER_W - 32, h);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_drawer_sections[sec_idx]   = c;
    s_drawer_section_y[sec_idx]  = y;
    return c;
}

// Build the drawer once. Hidden off-screen on the right initially.
static void drawer_build(void)
{
    if (s_drawer) return;

    lv_obj_t *scr = lv_screen_active();

    // Scrim: covers the area left of the drawer, blocks touches to underlying
    // content, and closes the drawer on a rightward swipe.
    s_drawer_scrim = lv_obj_create(scr);
    lv_obj_set_size(s_drawer_scrim, DISPLAY_H_RES - DRAWER_W, DISPLAY_V_RES);
    lv_obj_set_pos(s_drawer_scrim, 0, 0);
    lv_obj_set_style_bg_opa(s_drawer_scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_drawer_scrim, 0, 0);
    lv_obj_set_style_radius(s_drawer_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_drawer_scrim, 0, 0);
    lv_obj_set_scrollbar_mode(s_drawer_scrim, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_drawer_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_drawer_scrim, drawer_scrim_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_drawer_scrim, drawer_scrim_cb, LV_EVENT_RELEASED, NULL);

    s_drawer = lv_obj_create(scr);
    lv_obj_set_size(s_drawer, DRAWER_W, DISPLAY_V_RES);
    // Park off-screen to the right
    lv_obj_set_pos(s_drawer, DISPLAY_H_RES, 0);
    lv_obj_set_style_bg_color(s_drawer, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_drawer, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_drawer, 1, 0);
    lv_obj_set_style_border_side(s_drawer, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(s_drawer, 0, 0);
    lv_obj_set_style_pad_all(s_drawer, 16, 0);
    // Drawer scrolls vertically — content overflows once CW section is added.
    lv_obj_set_scroll_dir(s_drawer, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_drawer, LV_SCROLLBAR_MODE_AUTO);

    // Title bar with "Settings" (swipe right anywhere on the drawer to close)
    lv_obj_t *title = lv_label_create(s_drawer);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_add_event_cb(s_drawer, drawer_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_drawer, drawer_touch_cb, LV_EVENT_RELEASED, NULL);

    // === Phase 5.10D Stage 2b: presets + sliders ===
    // v0.8.x layout: 520 wide drawer, _24pt fonts, IQ row moved below title,
    // presets 3-across in a single row to free vertical space.
    int y = 96;

    // Diagnostic log ON/OFF row -- kept at the top for easy access. When on,
    // the Tab5 captures all log output (incl. per-line CAT TX/RX) to a buffer
    // downloadable from the web UI at http://<tab5-ip>/api/log, and streams it
    // to the USB serial console.
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_DIAG, y, 56);
        lv_obj_t *diag_lbl = lv_label_create(sec);
        lv_label_set_text(diag_lbl, "Diagnostic log");
        lv_obj_set_style_text_color(diag_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(diag_lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(diag_lbl, LV_ALIGN_TOP_LEFT, 0, 6);
        s_switch_diag = lv_switch_create(sec);
        lv_obj_set_size(s_switch_diag, 72, 36);
        lv_obj_align(s_switch_diag, LV_ALIGN_TOP_RIGHT, 0, 0);
        if (diag_log_enabled()) lv_obj_add_state(s_switch_diag, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_switch_diag, drawer_switch_diag_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 56;
    }

    // IQ balance ON/OFF row -- full width, well clear of the close button
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_IQ, y, 56);
        lv_obj_t *iq_lbl = lv_label_create(sec);
        lv_label_set_text(iq_lbl, "IQ Balance");
        lv_obj_set_style_text_color(iq_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(iq_lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(iq_lbl, LV_ALIGN_TOP_LEFT, 0, 6);
        s_switch_iq = lv_switch_create(sec);
        lv_obj_set_size(s_switch_iq, 72, 36);
        lv_obj_align(s_switch_iq, LV_ALIGN_TOP_RIGHT, 0, 0);
        if (iq_balance_is_enabled()) lv_obj_add_state(s_switch_iq, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_switch_iq, iq_balance_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 56;
    }

    // Phase 5.12: Flat Spectrum ON/OFF row
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_FLAT, y, 56);
        lv_obj_t *flat_lbl = lv_label_create(sec);
        lv_label_set_text(flat_lbl, "Flat Spectrum");
        lv_obj_set_style_text_color(flat_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(flat_lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(flat_lbl, LV_ALIGN_TOP_LEFT, 0, 6);
        s_switch_flat = lv_switch_create(sec);
        lv_obj_set_size(s_switch_flat, 72, 36);
        lv_obj_align(s_switch_flat, LV_ALIGN_TOP_RIGHT, 0, 0);
        if (ui_get_flat_mode()) lv_obj_add_state(s_switch_flat, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_switch_flat, drawer_switch_flat_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 56;
    }
    // Presets section: header + three buttons side-by-side
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_PRESETS, y, 108);
        lv_obj_t *presets_hdr = lv_label_create(sec);
        lv_label_set_text(presets_hdr, "Presets");
        lv_obj_set_style_text_color(presets_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(presets_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(presets_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        const char *preset_names[3] = { "HF Normal", "HF DX", "Strong Sig." };
        lv_event_cb_t preset_cbs[3] = {
            drawer_preset_normal_cb,
            drawer_preset_dx_cb,
            drawer_preset_strong_cb,
        };
        const int row_w   = DRAWER_W - 32;   // inner usable width
        const int gap     = 8;
        const int btn_w   = (row_w - 2 * gap) / 3;
        const int btn_h   = 56;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *btn = lv_btn_create(sec);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_align(btn, LV_ALIGN_TOP_LEFT, i * (btn_w + gap), 36);
            lv_obj_add_event_cb(btn, preset_cbs[i], LV_EVENT_CLICKED, NULL);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, preset_names[i]);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
            lv_obj_center(lbl);
        }
        y += 108;
    }

    // WiFi configuration button -- full width
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_WIFI, y, 72);
        lv_obj_t *btn = lv_btn_create(sec);
        lv_obj_set_size(btn, DRAWER_W - 32, 56);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_add_event_cb(btn, drawer_wifi_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "WiFi setup");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_center(lbl);
        y += 72;
    }
    // Operator identity button -- full width (callsign + grid for FT8 TX)
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_IDENTITY, y, 72);
        lv_obj_t *btn = lv_btn_create(sec);
        lv_obj_set_size(btn, DRAWER_W - 32, 56);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_add_event_cb(btn, drawer_identity_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "Callsign & Grid square");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_center(lbl);
        y += 72;
    }

    // dB Range section
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_DBRANGE, y, 212);
        lv_obj_t *db_hdr = lv_label_create(sec);
        lv_label_set_text(db_hdr, "dB Range");
        lv_obj_set_style_text_color(db_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(db_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(db_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        // dB min slider
        s_lbl_db_min = lv_label_create(sec);
        lv_label_set_text(s_lbl_db_min, "Min: -130 dBm");
        lv_obj_set_style_text_color(s_lbl_db_min, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_lbl_db_min, &lv_font_montserrat_24, 0);
        lv_obj_align(s_lbl_db_min, LV_ALIGN_TOP_LEFT, 0, 40);

        s_slider_db_min = lv_slider_create(sec);
        lv_obj_set_size(s_slider_db_min, DRAWER_W - 32, 30);
        lv_slider_set_range(s_slider_db_min, -150, -50);
        lv_slider_set_value(s_slider_db_min, -130, LV_ANIM_OFF);
        lv_obj_align(s_slider_db_min, LV_ALIGN_TOP_LEFT, 0, 70);
        lv_obj_add_event_cb(s_slider_db_min, drawer_slider_db_min_cb, LV_EVENT_VALUE_CHANGED, NULL);

        // dB max slider
        s_lbl_db_max = lv_label_create(sec);
        lv_label_set_text(s_lbl_db_max, "Max: -30 dBm");
        lv_obj_set_style_text_color(s_lbl_db_max, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_lbl_db_max, &lv_font_montserrat_24, 0);
        lv_obj_align(s_lbl_db_max, LV_ALIGN_TOP_LEFT, 0, 122);

        s_slider_db_max = lv_slider_create(sec);
        lv_obj_set_size(s_slider_db_max, DRAWER_W - 32, 30);
        lv_slider_set_range(s_slider_db_max, -50, 10);
        lv_slider_set_value(s_slider_db_max, -30, LV_ANIM_OFF);
        lv_obj_align(s_slider_db_max, LV_ALIGN_TOP_LEFT, 0, 152);
        lv_obj_add_event_cb(s_slider_db_max, drawer_slider_db_max_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 212;
    }

    // Smoothing section
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_SMOOTHING, y, 130);
        lv_obj_t *sm_hdr = lv_label_create(sec);
        lv_label_set_text(sm_hdr, "Smoothing");
        lv_obj_set_style_text_color(sm_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(sm_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(sm_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        s_lbl_alpha = lv_label_create(sec);
        lv_label_set_text(s_lbl_alpha, "Alpha: 0.40");
        lv_obj_set_style_text_color(s_lbl_alpha, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_lbl_alpha, &lv_font_montserrat_24, 0);
        lv_obj_align(s_lbl_alpha, LV_ALIGN_TOP_LEFT, 0, 40);

        s_slider_alpha = lv_slider_create(sec);
        lv_obj_set_size(s_slider_alpha, DRAWER_W - 32, 30);
        lv_slider_set_range(s_slider_alpha, 5, 100);   // = alpha 0.05..1.00
        lv_slider_set_value(s_slider_alpha, 40, LV_ANIM_OFF);
        lv_obj_align(s_slider_alpha, LV_ALIGN_TOP_LEFT, 0, 70);
        lv_obj_add_event_cb(s_slider_alpha, drawer_slider_alpha_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 130;
    }

    // CW section
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_CW, y, 130);
        lv_obj_t *cw_hdr = lv_label_create(sec);
        lv_label_set_text(cw_hdr, "CW");
        lv_obj_set_style_text_color(cw_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(cw_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(cw_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        s_lbl_cwpitch = lv_label_create(sec);
        lv_label_set_text(s_lbl_cwpitch, "CW center: 700 Hz");
        lv_obj_set_style_text_color(s_lbl_cwpitch, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_lbl_cwpitch, &lv_font_montserrat_24, 0);
        lv_obj_align(s_lbl_cwpitch, LV_ALIGN_TOP_LEFT, 0, 40);

        s_slider_cwpitch = lv_slider_create(sec);
        lv_obj_set_size(s_slider_cwpitch, DRAWER_W - 32, 30);
        lv_slider_set_range(s_slider_cwpitch, 600, 800);
        lv_slider_set_value(s_slider_cwpitch, (int)s_cw_pitch_hz, LV_ANIM_OFF);
        lv_obj_align(s_slider_cwpitch, LV_ALIGN_TOP_LEFT, 0, 70);
        lv_obj_add_event_cb(s_slider_cwpitch, drawer_slider_cwpitch_cb, LV_EVENT_VALUE_CHANGED, NULL);
        char cwbuf[24];
        snprintf(cwbuf, sizeof(cwbuf), "CW center: %u Hz", (unsigned)s_cw_pitch_hz);
        lv_label_set_text(s_lbl_cwpitch, cwbuf);
        y += 130;
    }

    // IF calibration section (per-unit QMX oscillator trim)
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_IFCAL, y, 130);
        lv_obj_t *ifcal_hdr = lv_label_create(sec);
        lv_label_set_text(ifcal_hdr, "IF calibration");
        lv_obj_set_style_text_color(ifcal_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(ifcal_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(ifcal_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        s_lbl_ifcal = lv_label_create(sec);
        char ifbuf[24];
        {
            qmx_settings_t scfg2;
            settings_load_all(&scfg2);
            snprintf(ifbuf, sizeof(ifbuf), "CW trim: %+d Hz", (int)scfg2.cw_cal_hz);
        }
        lv_label_set_text(s_lbl_ifcal, ifbuf);
        lv_obj_set_style_text_color(s_lbl_ifcal, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_lbl_ifcal, &lv_font_montserrat_24, 0);
        lv_obj_align(s_lbl_ifcal, LV_ALIGN_TOP_LEFT, 0, 40);

        s_slider_ifcal = lv_slider_create(sec);
        lv_obj_set_size(s_slider_ifcal, DRAWER_W - 32, 30);
        lv_slider_set_range(s_slider_ifcal, -100, 100);
        {
            qmx_settings_t scfg3;
            settings_load_all(&scfg3);
            lv_slider_set_value(s_slider_ifcal, (int)scfg3.cw_cal_hz, LV_ANIM_OFF);
        }
        lv_obj_align(s_slider_ifcal, LV_ALIGN_TOP_LEFT, 0, 70);
        lv_obj_add_event_cb(s_slider_ifcal, drawer_slider_ifcal_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 130;
    }

    // Display brightness section
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_BRIGHTNESS, y, 130);
        lv_obj_t *bl_hdr = lv_label_create(sec);
        lv_label_set_text(bl_hdr, "Display");
        lv_obj_set_style_text_color(bl_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(bl_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(bl_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        s_lbl_brightness = lv_label_create(sec);
        char blbuf[24];
        uint8_t bl_pct = 100;
        {
            qmx_settings_t scfg4;
            settings_load_all(&scfg4);
            bl_pct = scfg4.brightness_pct;
        }
        snprintf(blbuf, sizeof(blbuf), "Brightness: %u%%", (unsigned)bl_pct);
        lv_label_set_text(s_lbl_brightness, blbuf);
        lv_obj_set_style_text_color(s_lbl_brightness, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_lbl_brightness, &lv_font_montserrat_24, 0);
        lv_obj_align(s_lbl_brightness, LV_ALIGN_TOP_LEFT, 0, 40);

        s_slider_brightness = lv_slider_create(sec);
        lv_obj_set_size(s_slider_brightness, DRAWER_W - 32, 30);
        lv_slider_set_range(s_slider_brightness, 10, 100);
        lv_slider_set_value(s_slider_brightness, bl_pct, LV_ANIM_OFF);
        lv_obj_align(s_slider_brightness, LV_ALIGN_TOP_LEFT, 0, 70);
        lv_obj_add_event_cb(s_slider_brightness, drawer_slider_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 130;
    }

    // Waterfall colour-map section
    {
        lv_obj_t *sec = drawer_section(DRAWER_SEC_CMAP, y, 100);
        lv_obj_t *cmap_hdr = lv_label_create(sec);
        lv_label_set_text(cmap_hdr, "Waterfall colour map");
        lv_obj_set_style_text_color(cmap_hdr, lv_color_hex(0xA0E0A0), 0);
        lv_obj_set_style_text_font(cmap_hdr, &lv_font_montserrat_24, 0);
        lv_obj_align(cmap_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        s_dropdown_cmap = lv_dropdown_create(sec);
        lv_dropdown_set_options(s_dropdown_cmap, "Thermal\nViridis\nTurbo\nGrayscale");
        lv_obj_set_size(s_dropdown_cmap, DRAWER_W - 32, 50);
        lv_obj_align(s_dropdown_cmap, LV_ALIGN_TOP_LEFT, 0, 40);
        lv_obj_set_style_text_font(s_dropdown_cmap, &lv_font_montserrat_24, 0);
        {
            qmx_settings_t scfg;
            settings_load_all(&scfg);
            if (scfg.colormap_idx < 4) lv_dropdown_set_selected(s_dropdown_cmap, scfg.colormap_idx);
        }
        lv_obj_add_event_cb(s_dropdown_cmap, drawer_dropdown_cmap_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_dropdown_cmap, drawer_dropdown_cmap_open_cb, LV_EVENT_CLICKED, NULL);
        y += 100;
    }

    ESP_LOGI(TAG, "Settings drawer built (off-screen at x=%d)", DISPLAY_H_RES);

    // Apply current UI mode's section visibility (drawer is pre-built at
    // boot before ui_apply_saved_mode() runs).
    drawer_set_ft8_mode(ui_mode_get() == UI_MODE_FT8);
}

static void drawer_open(void)
{
    drawer_build();  // lazy build on first open
    if (!s_drawer || s_drawer_open) return;
    if (s_drawer_scrim) {
        lv_obj_clear_flag(s_drawer_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_drawer_scrim);
    }
    lv_obj_move_foreground(s_drawer);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, drawer_anim_x_cb);
    lv_anim_set_values(&a, DISPLAY_H_RES, DISPLAY_H_RES - DRAWER_W);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    s_drawer_open = true;
    ESP_LOGI(TAG, "Settings drawer open");
}

static void drawer_close(void)
{
    if (!s_drawer || !s_drawer_open) return;
    if (s_drawer_scrim) lv_obj_add_flag(s_drawer_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, drawer_anim_x_cb);
    lv_anim_set_values(&a, DISPLAY_H_RES - DRAWER_W, DISPLAY_H_RES);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
    s_drawer_open = false;
    ESP_LOGI(TAG, "Settings drawer closed");
}

// FT8 mode keeps the Diagnostic log toggle (top, always accessible), WiFi
// setup, Callsign & Grid, and Display brightness; everything else (IQ/flat
// toggles, presets, dB range, smoothing, CW, IF calibration, colour map) is
// irrelevant there. Hide the rest and restack the kept sections near the top
// of the drawer. Called on every mode switch (and once at boot via drawer_build()).
static void drawer_set_ft8_mode(bool ft8)
{
    if (!s_drawer) return;
    static const int keep[]   = { DRAWER_SEC_DIAG, DRAWER_SEC_WIFI, DRAWER_SEC_IDENTITY, DRAWER_SEC_BRIGHTNESS };
    static const int keep_h[] = { 56, 72, 72, 130 };
    const int n_keep = sizeof(keep) / sizeof(keep[0]);

    for (int i = 0; i < N_DRAWER_SECTIONS; i++) {
        if (!s_drawer_sections[i]) continue;
        bool kept = false;
        for (int k = 0; k < n_keep; k++) {
            if (keep[k] == i) { kept = true; break; }
        }
        if (ft8 && !kept) {
            lv_obj_add_flag(s_drawer_sections[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_drawer_sections[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (ft8) {
        int y = 96;
        for (int k = 0; k < n_keep; k++) {
            lv_obj_set_pos(s_drawer_sections[keep[k]], 0, y);
            y += keep_h[k];
        }
    } else {
        for (int i = 0; i < N_DRAWER_SECTIONS; i++) {
            if (s_drawer_sections[i]) lv_obj_set_pos(s_drawer_sections[i], 0, s_drawer_section_y[i]);
        }
    }
}

// === Phase 5.10D Stage 2b: drawer button + slider callbacks ===

static void drawer_apply_preset(int db_min, int db_max, float alpha)
{
    if (s_slider_db_min) lv_slider_set_value(s_slider_db_min, db_min, LV_ANIM_OFF);
    if (s_slider_db_max) lv_slider_set_value(s_slider_db_max, db_max, LV_ANIM_OFF);
    if (s_slider_alpha)  lv_slider_set_value(s_slider_alpha, (int)(alpha * 100.0f + 0.5f), LV_ANIM_OFF);

    char buf[24];
    if (s_lbl_db_min) {
        snprintf(buf, sizeof(buf), "Min: %d dBm", db_min);
        lv_label_set_text(s_lbl_db_min, buf);
    }
    if (s_lbl_db_max) {
        snprintf(buf, sizeof(buf), "Max: %d dBm", db_max);
        lv_label_set_text(s_lbl_db_max, buf);
    }
    if (s_lbl_alpha) {
        snprintf(buf, sizeof(buf), "Alpha: %.2f", (double)alpha);
        lv_label_set_text(s_lbl_alpha, buf);
    }

    ui_set_db_range((float)db_min, (float)db_max);
    render_set_ema_alpha(alpha);
    settings_set_db_min((float)db_min);
    settings_set_db_max((float)db_max);
    settings_set_ema_alpha(alpha);
}

/* Phase 5.12: flat-spectrum mode accessor + drawer callback */
bool ui_get_flat_mode(void)
{
    return s_flat_mode;
}

// Re-seed the flat-spectrum per-bin floor from the next frame. Called by
// audio.c when the first real audio samples arrive after a UAC stream
// (re)start, so a stale floor from before a QMX power cycle doesn't linger.
void ui_flat_mode_reset(void)
{
    s_flat_ready = false;
    render_waterfall_floor_reset();
}

void ui_set_flat_mode(bool on)
{
    s_flat_mode  = on;
    s_flat_ready = false;  /* re-seed floor on next draw */
    if (s_switch_flat) {
        if (on) lv_obj_add_state(s_switch_flat, LV_STATE_CHECKED);
        else    lv_obj_remove_state(s_switch_flat, LV_STATE_CHECKED);
    }
    // Hide absolute dBm labels in flat mode (axis is dB-above-floor; labels misleading).
    if (s_db_min_label && s_db_max_label) {
        if (on) {
            lv_obj_add_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void drawer_switch_flat_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_flat_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    s_flat_ready = false;  /* re-seed floor next time flat mode draws */
    ESP_LOGI(TAG, "flat-spectrum mode: %s", s_flat_mode ? "ON" : "OFF");
    settings_set_flat_mode(s_flat_mode);
    if (s_db_min_label && s_db_max_label) {
        if (s_flat_mode) {
            lv_obj_add_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void drawer_switch_diag_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    diag_log_set_enabled(on);
    settings_set_diag_log(on);
    ESP_LOGI(TAG, "diagnostic logging: %s", on ? "ON" : "OFF");
}

static void drawer_preset_normal_cb(lv_event_t *e)  { (void)e; drawer_apply_preset(-130, -30, 0.40f); }
static void drawer_preset_dx_cb(lv_event_t *e)      { (void)e; drawer_apply_preset(-130, -50, 0.60f); }
static void drawer_preset_strong_cb(lv_event_t *e)  { (void)e; drawer_apply_preset(-110, -20, 0.20f); }
static void drawer_wifi_btn_cb(lv_event_t *e)       { (void)e; wifi_config_modal_show(); }
static void drawer_identity_btn_cb(lv_event_t *e)   { (void)e; identity_config_modal_show(); }

static void drawer_slider_db_min_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    char buf[24];
    snprintf(buf, sizeof(buf), "Min: %d dBm", v);
    if (s_lbl_db_min) lv_label_set_text(s_lbl_db_min, buf);
    int max_v = s_slider_db_max ? (int)lv_slider_get_value(s_slider_db_max) : -30;
    if (v >= max_v) v = max_v - 5;
    ui_set_db_range((float)v, (float)max_v);
    settings_set_db_min((float)v);
    settings_set_db_max((float)max_v);
}

static void drawer_slider_db_max_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    char buf[24];
    snprintf(buf, sizeof(buf), "Max: %d dBm", v);
    if (s_lbl_db_max) lv_label_set_text(s_lbl_db_max, buf);
    int min_v = s_slider_db_min ? (int)lv_slider_get_value(s_slider_db_min) : -130;
    if (v <= min_v) v = min_v + 5;
    ui_set_db_range((float)min_v, (float)v);
    settings_set_db_min((float)min_v);
    settings_set_db_max((float)v);
}

static void drawer_slider_alpha_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    float alpha = (float)v / 100.0f;
    char buf[24];
    snprintf(buf, sizeof(buf), "Alpha: %.2f", (double)alpha);
    if (s_lbl_alpha) lv_label_set_text(s_lbl_alpha, buf);
    render_set_ema_alpha(alpha);
    settings_set_ema_alpha(alpha);
}

static void drawer_slider_cwpitch_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    // Snap to nearest 50 Hz (valid QMX CW center values are 50 Hz apart)
    int snapped = ((v + 25) / 50) * 50;
    if (snapped < 600) snapped = 600;
    if (snapped > 800) snapped = 800;
    ui_set_cw_pitch_hz((uint16_t)snapped);
    char buf[24];
    snprintf(buf, sizeof(buf), "CW center: %d Hz", snapped);
    if (s_lbl_cwpitch) lv_label_set_text(s_lbl_cwpitch, buf);
}

static void drawer_slider_brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    display_set_brightness(v);
    settings_set_brightness_pct((uint8_t)v);
    if (s_lbl_brightness) {
        char b[24];
        snprintf(b, sizeof(b), "Brightness: %d%%", v);
        lv_label_set_text(s_lbl_brightness, b);
    }
}

static void drawer_dropdown_cmap_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t idx = (uint8_t)lv_dropdown_get_selected(dd);
    render_waterfall_set_colormap(idx);
    settings_set_colormap_idx(idx);
}

static void drawer_dropdown_cmap_open_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_text_font(list, &lv_font_montserrat_24, 0);
    }
}

// ---- Phase 9 (v0.9.5): read-only getters for web JSON ------------------

const char *ui_get_mode_str(void) { return s_current_mode; }
const char *ui_get_band_str(void) { return s_current_band; }
uint32_t ui_get_passband_width_hz(void) { return s_passband_width_hz; }

// Step 4c.1 v0.10: drawer mode toggle.
//
// Tap flips ui_mode between PANADAPTER and FT8. On entry to FT8 we
// respawn ft8_task (it self-deletes on mode-exit, so we re-spawn
// each time the user re-enters FT8 mode). On exit, fft_task drops
// back to its panadapter path (DC blocker + FFT + spectrum push)
// and the waterfall resumes.
//
// 4c.2 will add the LVGL screen switch alongside this.

// Restore the UI mode persisted at the last toggle. Must be called from
// main.c after ft8_screen_init()/ft8_status_init()/ft8_tx_init()/ft8_qso_init()
// (and audio/cat init) have run -- ft8_screen_view_show() and ft8_self_test()
// touch state set up by those.
void ui_apply_saved_mode(void)
{
    ESP_LOGI(TAG, "ui_apply_saved_mode: last_ui_mode from NVS = %u", (unsigned)s_saved_ui_mode);
    if (s_saved_ui_mode != UI_MODE_FT8) {
        ui_mode_set(UI_MODE_PANADAPTER);
        drawer_set_ft8_mode(false);
        ESP_LOGI(TAG, "UI mode restored from NVS: Panadapter");
        return;
    }
    ui_mode_set(UI_MODE_FT8);
    if (s_spectrum_obj)  lv_obj_add_flag(s_spectrum_obj,  LV_OBJ_FLAG_HIDDEN);
    if (s_label_bar)     lv_obj_add_flag(s_label_bar,     LV_OBJ_FLAG_HIDDEN);
    if (s_waterfall_obj) lv_obj_add_flag(s_waterfall_obj, LV_OBJ_FLAG_HIDDEN);
    top_bar_set_ft8_dim(true);
    drawer_set_ft8_mode(true);
    // FT8 is a digital mode - force the radio into DiGi regardless of
    // whatever mode (e.g. CW) was active in Panadapter mode.
    if (strcmp(cat_get_mode_str(), "DiGi") != 0) cat_set_mode("DIGI");
    ft8_screen_view_show();
    ft8_self_test();
    ESP_LOGI(TAG, "UI mode restored from NVS: FT8");
}

// Toggle between Panadapter and FT8 mode. Triggered by a left-edge swipe
// (drag right) on the spectrum/waterfall; replaces the drawer's mode button.
static void ui_toggle_mode(void)
{
    ui_mode_t cur = ui_mode_get();
    ui_mode_t next = (cur == UI_MODE_FT8) ? UI_MODE_PANADAPTER : UI_MODE_FT8;
    ESP_LOGI(TAG, "Mode toggle: %s -> %s",
             cur  == UI_MODE_FT8 ? "FT8" : "Panadapter",
             next == UI_MODE_FT8 ? "FT8" : "Panadapter");
    ui_mode_set(next);
    settings_set_last_ui_mode((uint8_t)next);
    top_bar_set_ft8_dim(next == UI_MODE_FT8);
    drawer_set_ft8_mode(next == UI_MODE_FT8);

    lv_obj_t *ft8 = ft8_screen_view_get_container();

    // Swap visible widgets with a slide animation. Top/bottom bars stay
    // visible in both modes (and unanimated).
    if (next == UI_MODE_FT8) {
        if (ft8) {
            lv_obj_set_x(ft8, -DISPLAY_H_RES);
            lv_obj_clear_flag(ft8, LV_OBJ_FLAG_HIDDEN);
        }
        // Sticky settings: remember where Panadapter was left, restore
        // where FT8 was left (or just force DiGi on the very first entry).
        ui_save_snapshot(&s_pan_snapshot);
        if (s_ft8_snapshot.valid) {
            ui_restore_snapshot(&s_ft8_snapshot);
        } else if (strcmp(cat_get_mode_str(), "DiGi") != 0) {
            cat_set_mode("DIGI");
        }
        ft8_screen_view_show();
        // Respawn the FT8 task; it self-deleted on previous exit.
        ft8_self_test();
        slide_x_anim(ft8, -DISPLAY_H_RES, 0, NULL);
        if (s_spectrum_obj)  slide_x_anim(s_spectrum_obj,  0, DISPLAY_H_RES, mode_slide_out_ready_cb);
        if (s_label_bar)     slide_x_anim(s_label_bar,     0, DISPLAY_H_RES, mode_slide_out_ready_cb);
        if (s_waterfall_obj) slide_x_anim(s_waterfall_obj, 0, DISPLAY_H_RES, mode_slide_out_ready_cb);
    } else {
        // Sticky settings: remember where FT8 was left, restore where
        // Panadapter was left (band/mode/bw/freq/zoom).
        ui_save_snapshot(&s_ft8_snapshot);
        ui_restore_snapshot(&s_pan_snapshot);
        if (s_spectrum_obj)  { lv_obj_set_x(s_spectrum_obj,  -DISPLAY_H_RES); lv_obj_clear_flag(s_spectrum_obj,  LV_OBJ_FLAG_HIDDEN); }
        if (s_label_bar)     { lv_obj_set_x(s_label_bar,     -DISPLAY_H_RES); lv_obj_clear_flag(s_label_bar,     LV_OBJ_FLAG_HIDDEN); }
        if (s_waterfall_obj) { lv_obj_set_x(s_waterfall_obj, -DISPLAY_H_RES); lv_obj_clear_flag(s_waterfall_obj, LV_OBJ_FLAG_HIDDEN); }
        slide_x_anim(s_spectrum_obj,  -DISPLAY_H_RES, 0, NULL);
        slide_x_anim(s_label_bar,     -DISPLAY_H_RES, 0, NULL);
        slide_x_anim(s_waterfall_obj, -DISPLAY_H_RES, 0, NULL);
        slide_x_anim(ft8, 0, DISPLAY_H_RES, ft8_slide_out_ready_cb);
    }
    // Close the drawer (if open) after toggling. UX nicety, and (4c.1
    // finding) an open drawer keeps LVGL busy enough to starve audio/fft
    // tasks and stall the next FT8 capture.
    drawer_close();
}

// Open the memory-channel modal. Triggered by a bottom-edge swipe (drag up)
// on the spectrum/waterfall; replaces the drawer's "Memories" button.
static void ui_show_memories(void)
{
    drawer_close();
    memory_modal_show();
}
