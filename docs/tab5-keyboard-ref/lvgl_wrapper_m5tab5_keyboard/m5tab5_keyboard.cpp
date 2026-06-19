/*
 * SPDX-FileCopyrightText: 2025-2026 exia
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5tab5_keyboard.h"
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_24);

static const char* TAG = "M5Tab5Keyboard";

// --- Global Instance Access ---
M5Tab5Keyboard* getM5Tab5KeyboardInstance();
void setM5Tab5KeyboardInstance(M5Tab5Keyboard* instance);

// --- Implementation ---

M5Tab5Keyboard::M5Tab5Keyboard()
    : m_caps_state(CAPS_STATE_LOWER),
      m_sym_mode(false),
      m_sym_holding(false),
      m_flash_timer(nullptr),
      m_flash_on(false),
      m_aa_indicator(nullptr),
      m_last_aa_click_time(0),
      m_aa_press_start_time(0),
      m_sym_press_start_time(0),
      m_last_aa_release_time(0),
      m_ignore_next_aa_release(false)
{
    memset(m_key_disabled, 0, sizeof(m_key_disabled));
    memset(m_key_buttons, 0, sizeof(m_key_buttons));
}

M5Tab5Keyboard::~M5Tab5Keyboard()
{
    if (m_flash_timer) {
        lv_timer_del(m_flash_timer);
    }
    if (getM5Tab5KeyboardInstance() == this) {
        setM5Tab5KeyboardInstance(nullptr);
    }
}

bool M5Tab5Keyboard::isUpperMode() const
{
    return (m_caps_state != CAPS_STATE_LOWER);
}

kb_mode_t M5Tab5Keyboard::getCurrentMappingMode() const
{
    if (m_sym_holding) return KB_MODE_2;
    if (m_sym_mode) return KB_MODE_2;
    if (isUpperMode()) return KB_MODE_1;
    return KB_MODE_0;
}

void M5Tab5Keyboard::startFlashTimer()
{
    if (m_flash_timer == NULL) {
        m_flash_timer = lv_timer_create(flashTimerCbStatic, 200, this);
        m_flash_on    = true;
    } else {
        lv_timer_resume(m_flash_timer);
    }
}

void M5Tab5Keyboard::stopFlashTimer()
{
    if (m_flash_timer) {
        lv_timer_pause(m_flash_timer);
        m_flash_on = false;
    }
}

void M5Tab5Keyboard::flashTimerCbStatic(lv_timer_t* timer)
{
    M5Tab5Keyboard* kb = (M5Tab5Keyboard*)lv_timer_get_user_data(timer);
    if (kb) kb->handleFlashTimer();
}

void M5Tab5Keyboard::handleFlashTimer()
{
    m_flash_on = !m_flash_on;

    if (m_aa_indicator) {
        lv_color_t color;
        if (m_flash_on) {
            color = lv_color_hex(0x00FFFF);  // Blue ON
        } else {
            color = lv_color_hex(0x808080);  // Gray OFF
        }
        lv_obj_set_style_bg_color(m_aa_indicator, color, 0);
    }
}

void M5Tab5Keyboard::setKeyDisabled(uint16_t key_id, bool disabled)
{
    for (int i = 0; i < KEYBOARD_KEY_COUNT; i++) {
        if (tab5_key_map[i].id == key_id) {
            m_key_disabled[i] = disabled;
            updateUiState();
            return;
        }
    }
    ESP_LOGW(TAG, "Key ID %d not found", key_id);
}

void M5Tab5Keyboard::updateUiState()
{
    if (!m_kb_cont) return;

    kb_mode_t map_mode = getCurrentMappingMode();
    uint32_t count     = lv_obj_get_child_count(m_kb_cont);

    for (uint32_t i = 0; i < count; i++) {
        if (i >= KEYBOARD_KEY_COUNT) break;

        lv_obj_t* btn = lv_obj_get_child(m_kb_cont, i);
        if (!btn) continue;

        const tab5_key_t* key = &tab5_key_map[i];

        // 1. Check Disabled
        if (m_key_disabled[i]) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xD0D0D0), 0);
            lv_obj_t* label = lv_obj_get_child(btn, 0);
            if (label) lv_label_set_text(label, "");

            // Hide indicators
            if (lv_obj_get_child_cnt(btn) > 1) {
                lv_obj_t* indicator = lv_obj_get_child(btn, 1);
                if (indicator) lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }

        // 2. Normal State Bg
        lv_obj_set_style_bg_color(btn, lv_color_hex(key->bg_color), 0);

        // 3. Update Label
        lv_obj_t* label = lv_obj_get_child(btn, 0);
        if (label) {
            const char* text = key->labels[map_mode];
            bool is_fallback = false;

            if (map_mode == KB_MODE_2 && text == NULL) {
                // Fallback: if in upper mode, use Mode 1 (uppercase), otherwise Mode 0 (lowercase)
                text        = isUpperMode() ? key->labels[1] : key->labels[0];
                is_fallback = true;
            }

            if (text == NULL) text = "";
            lv_label_set_text(label, text);

            if (map_mode == KB_MODE_2 && !is_fallback) {
                lv_obj_set_style_text_color(label, lv_color_hex(0x4080FF), 0);
            } else {
                lv_obj_set_style_text_color(label, lv_color_hex(key->text_color), 0);
            }
        }

        // 4. Update Sub-Label (Index 2)
        if (lv_obj_get_child_cnt(btn) > 2) {
            lv_obj_t* sub_label = lv_obj_get_child(btn, 2);
            if (sub_label) {
                if (map_mode == KB_MODE_2) {
                    lv_obj_add_flag(sub_label, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(sub_label, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // 5. Update Indicator (Index 1)
        if (lv_obj_get_child_cnt(btn) > 1) {
            lv_obj_t* indicator = lv_obj_get_child(btn, 1);
            bool is_lockable    = (key->id == KEY_ID_SYM || key->id == KEY_ID_AA);

            if (indicator) {
                if (!is_lockable) {
                    lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_HIDDEN);
                    lv_color_t color = lv_color_hex(0x808080);  // Gray

                    if (key->id == KEY_ID_AA) {
                        if (m_caps_state == CAPS_STATE_LOCKED) {
                            color = lv_color_hex(0x00FFFF);
                        } else if (m_caps_state == CAPS_STATE_HOLDING) {
                            color = lv_color_hex(0x00FFFF);
                        } else if (m_caps_state == CAPS_STATE_SINGLE_PENDING) {
                            color = m_flash_on ? lv_color_hex(0x00FFFF) : lv_color_hex(0x808080);
                        }
                    } else if (key->id == KEY_ID_SYM) {
                        if (m_sym_mode || m_sym_holding) {
                            color = lv_color_hex(0x00FFFF);
                        }
                    }
                    lv_obj_set_style_bg_color(indicator, color, 0);
                }
            }
        }
    }
}

void M5Tab5Keyboard::handleShiftEvent(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        m_aa_press_start_time = lv_tick_get();
        ESP_LOGI(TAG, "Aa PRESSED: state=%d", m_caps_state);

        if (m_caps_state == CAPS_STATE_SINGLE_PENDING) {
            uint32_t now     = lv_tick_get();
            uint32_t elapsed = now - m_last_aa_click_time;
            ESP_LOGI(TAG, "SINGLE_PENDING detected, elapsed=%lu ms", elapsed);

            if (elapsed < 2000) {
                // Double Click -> Lock
                ESP_LOGI(TAG, "Double click detected! Locking...");
                m_caps_state = CAPS_STATE_LOCKED;
                stopFlashTimer();
                updateUiState();
                m_last_aa_click_time = 0;
                // Important: Next release should NOT unlock.
                m_ignore_next_aa_release = true;
                return;  // Must return here to prevent state change
            } else {
                // Timeout exceeded, cancel SINGLE_PENDING
                ESP_LOGI(TAG, "Timeout exceeded, canceling SINGLE_PENDING");
                m_caps_state = CAPS_STATE_LOWER;
                stopFlashTimer();
                updateUiState();
                m_last_aa_click_time = 0;
                return;  // Cancel, don't start new click
            }
        }

        if (m_caps_state == CAPS_STATE_HOLDING) {
            // Already holding, toggle off
            ESP_LOGI(TAG, "Already HOLDING, toggle to LOWER");
            m_caps_state = CAPS_STATE_LOWER;
            updateUiState();
            return;
        }

        if (m_caps_state != CAPS_STATE_LOCKED) {
            ESP_LOGI(TAG, "Setting to HOLDING");
            m_caps_state = CAPS_STATE_HOLDING;
            updateUiState();
        }
    } else if (code == LV_EVENT_RELEASED) {
        uint32_t now = lv_tick_get();

        // Debounce: ignore duplicate RELEASED events within 50ms
        if (now - m_last_aa_release_time < 50) {
            ESP_LOGW(TAG, "Aa RELEASED ignored (debounce): %lu ms since last", now - m_last_aa_release_time);
            return;
        }
        m_last_aa_release_time = now;

        ESP_LOGI(TAG, "Aa RELEASED: state=%d, ignore_flag=%d", m_caps_state, m_ignore_next_aa_release);

        // Did we just enter LOCKED state on the preceding PRESS?
        if (m_ignore_next_aa_release) {
            ESP_LOGI(TAG, "Ignoring release (just locked)");
            m_ignore_next_aa_release = false;
            return;
        }

        uint32_t press_duration = now - m_aa_press_start_time;
        ESP_LOGI(TAG, "Press duration: %lu ms", press_duration);

        if (m_caps_state == CAPS_STATE_HOLDING) {
            if (press_duration < 500) {
                // Short Click
                ESP_LOGI(TAG, "Short click -> SINGLE_PENDING");
                m_caps_state = CAPS_STATE_SINGLE_PENDING;
                startFlashTimer();
                updateUiState();
                m_last_aa_click_time = now;
            } else {
                // Long Hold -> Recover to Lower
                ESP_LOGI(TAG, "Long hold -> LOWER");
                m_caps_state = CAPS_STATE_LOWER;
                stopFlashTimer();
                updateUiState();
            }
        } else if (m_caps_state == CAPS_STATE_LOCKED) {
            if (press_duration < 500) {
                ESP_LOGI(TAG, "Unlocking from LOCKED");
                m_caps_state = CAPS_STATE_LOWER;  // Exit Lock
                stopFlashTimer();
                updateUiState();
            }
        }
    }
}

void M5Tab5Keyboard::handleSymEvent(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        // Press: Show symbols temporarily
        m_sym_press_start_time = lv_tick_get();
        m_sym_holding          = true;
        updateUiState();
    } else if (code == LV_EVENT_RELEASED) {
        // Release: Hide symbols
        m_sym_holding = false;
        updateUiState();
    }
}

void M5Tab5Keyboard::handleKeyEvent(lv_event_t* e)
{
    tab5_key_t* key = (tab5_key_t*)lv_event_get_user_data(e);
    if (!key) return;

    lv_event_code_t code = lv_event_get_code(e);

    // ========================================================================
    // 修饰键状态跟踪
    // ========================================================================
    if (key->id == KEY_ID_CTRL) {
        if (code == LV_EVENT_PRESSED) {
            m_ctrl_pressed = true;
            ESP_LOGD(TAG, "CTRL pressed");
        } else if (code == LV_EVENT_RELEASED) {
            m_ctrl_pressed = false;
            ESP_LOGD(TAG, "CTRL released");
        }
        return;  // CTRL 键本身不触发其他逻辑
    }

    if (key->id == KEY_ID_ALT) {
        if (code == LV_EVENT_PRESSED) {
            m_alt_pressed = true;
            ESP_LOGD(TAG, "ALT pressed");
        } else if (code == LV_EVENT_RELEASED) {
            m_alt_pressed = false;
            ESP_LOGD(TAG, "ALT released");
        }
        return;  // ALT 键本身不触发其他逻辑
    }

    // ========================================================================
    // 热键触发 - PRESSED 事件
    // 只有在有修饰键（CTRL/ALT）或特殊键（ESC）时才触发热键
    // ========================================================================
    if (code == LV_EVENT_PRESSED) {
        // 快速检查：只有修饰键按下或特殊键才触发热键
        bool has_modifier   = m_ctrl_pressed || m_alt_pressed;
        bool is_special_key = (key->id == KB_KEY_ESC);

        if (has_modifier || is_special_key) {
            hotkey_event_t event;
            event.key_id        = key->id;
            event.key_label     = key->labels[0];
            event.key_value     = key->values[0];
            event.event         = HOTKEY_EVENT_PRESSED;
            event.ctrl_pressed  = m_ctrl_pressed;
            event.alt_pressed   = m_alt_pressed;
            event.shift_pressed = isUpperMode();
            event.user_data     = nullptr;

            // 触发热键回调，如果返回 true 则拦截
            if (triggerHotkey(&event)) {
                ESP_LOGD(TAG, "Key event intercepted by hotkey: id=%d", key->id);
                return;  // 拦截，不继续处理
            }
        }
    }

    // Filter disabled
    // Need key index. Using key pointer arithmetic if possible, or ID search
    // Using ID search for safety
    int key_index = -1;
    for (int i = 0; i < KEYBOARD_KEY_COUNT; ++i) {
        if (&tab5_key_map[i] == key) {
            key_index = i;
            break;
        }
    }
    if (key_index >= 0 && m_key_disabled[key_index]) return;

    // Dispatch Special
    if (key->id == KEY_ID_AA) {
        handleShiftEvent(e);
        return;
    }
    if (key->id == KEY_ID_SYM) {
        handleSymEvent(e);
        return;
    }

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        processKeyInput(key);
    }
}

void M5Tab5Keyboard::processKeyInput(const tab5_key_t* key)
{
    if (!key) return;

    // 如果是修饰键本身，不输入
    if (key->id == KEY_ID_CTRL || key->id == KEY_ID_ALT) {
        return;
    }

    // 如果 CTRL 或 ALT 按下，不输入字符（等待组合键处理）
    if (m_ctrl_pressed || m_alt_pressed) {
        ESP_LOGD(TAG, "Modifier key pressed, skip input for key_id=%d", key->id);
        return;
    }

    // Generate output
    kb_mode_t mode  = getCurrentMappingMode();
    const char* val = key->values[mode];

    if (mode == KB_MODE_2 && val == NULL) {
        // Fallback: if in upper mode, use Mode 1 (uppercase), otherwise Mode 0 (lowercase)
        val = isUpperMode() ? key->values[1] : key->values[0];
    }
    if (val == NULL || strlen(val) == 0) return;

    // ========================================================================
    // 热键触发 - BEFORE_INPUT 事件
    // ========================================================================
    hotkey_event_t event;
    event.key_id        = key->id;
    event.key_label     = key->labels[mode];
    event.key_value     = val;
    event.event         = HOTKEY_EVENT_BEFORE_INPUT;
    event.ctrl_pressed  = m_ctrl_pressed;
    event.alt_pressed   = m_alt_pressed;
    event.shift_pressed = isUpperMode();
    event.user_data     = nullptr;

    // 触发热键回调，如果返回 true 则拦截输入
    if (triggerHotkey(&event)) {
        ESP_LOGD(TAG, "Input intercepted by hotkey: key_id=%d, value=%s", key->id, val);
        return;  // 拦截输入
    }

    // Send to Textarea
    sendTextToTextarea(val);

    // Auto-lower if single pending, but only for letter input (not symbols)
    // If in symbol mode (KB_MODE_2), preserve the caps state
    if (m_caps_state == CAPS_STATE_SINGLE_PENDING && mode != KB_MODE_2) {
        m_caps_state = CAPS_STATE_LOWER;
        stopFlashTimer();
        updateUiState();
    }
}

void M5Tab5Keyboard::eventHandlerStatic(lv_event_t* e)
{
    // Retrieve "this" pointer
    // We attach "this" to the object's parent or pass it as user_data?
    // Wait, lv_obj_add_event_cb accepts user_data, but we passed "key" as user_data!
    // We can't pass BOTH "key" and "this" in user_data easily without a struct.

    // Alternative: We know the button is a child of m_kb_cont.
    // We can attach "this" to m_kb_cont as user_data.

    lv_obj_t* btn  = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* cont = lv_obj_get_parent(btn);
    // Be safer
    if (!cont) return;
    M5Tab5Keyboard* kb = (M5Tab5Keyboard*)lv_obj_get_user_data(cont);

    if (kb) {
        kb->handleKeyEvent(e);
    }
}

lv_obj_t* M5Tab5Keyboard::create(lv_obj_t* parent)
{
    m_kb_cont = lv_obj_create(parent);

    // Attach "this" to the container so static handlers can find us
    lv_obj_set_user_data(m_kb_cont, this);

    // Calculate Dynamic Size based on Constraints
    lv_disp_t* disp = lv_obj_get_disp(parent);
    if (!disp) disp = lv_disp_get_default();
    int32_t screen_w = lv_disp_get_hor_res(disp);
    int32_t screen_h = lv_disp_get_ver_res(disp);

    // 1. Determine Target Width (default 98%)
    int32_t target_w = (int32_t)(screen_w * 0.98f);

    // 2. Calculate Target Height based on Preferred Aspect Ratio
    float ratio = getPreferredAspectRatio();
    if (ratio < 0.1f) ratio = 1.0f;  // Safety
    int32_t target_h = (int32_t)(target_w / ratio);

    // 3. Apply Min/Max Aspect Ratio Clamping
    float min_r = getMinAspectRatio();
    float max_r = getMaxAspectRatio();

    // Height = Width / Ratio.
    // If Height is too big (Ratio too small), clamp to Min Ratio
    // If Height is too small (Ratio too big), clamp to Max Ratio
    // Actually:
    // H_max = W / MinRatio
    // H_min = W / MaxRatio
    int32_t h_allowed_max = (int32_t)(target_w / min_r);
    int32_t h_allowed_min = (int32_t)(target_w / max_r);

    if (target_h > h_allowed_max) target_h = h_allowed_max;
    if (target_h < h_allowed_min) target_h = h_allowed_min;

    // 4. Apply Max Screen Height Percentage Limit
    int32_t screen_h_limit = (int32_t)(screen_h * getMaxScreenHeightPct());
    if (target_h > screen_h_limit) {
        target_h = screen_h_limit;
    }

    lv_obj_set_size(m_kb_cont, target_w, target_h);
    lv_obj_align(m_kb_cont, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_obj_set_style_bg_color(m_kb_cont, lv_color_hex(0x181818), 0);
    lv_obj_set_style_radius(m_kb_cont, 10, 0);
    lv_obj_set_style_border_width(m_kb_cont, 0, 0);
    lv_obj_set_style_pad_gap(m_kb_cont, 4, 0);
    lv_obj_clear_flag(m_kb_cont, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Grid descriptors
    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    lv_obj_set_layout(m_kb_cont, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(m_kb_cont, col_dsc, row_dsc);

    for (int i = 0; i < KEYBOARD_KEY_COUNT; i++) {
        const tab5_key_t* key = &tab5_key_map[i];

#if LVGL_VERSION_MAJOR >= 9
        lv_obj_t* btn = lv_button_create(m_kb_cont);
#else
        lv_obj_t* btn = lv_btn_create(m_kb_cont);
#endif
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, key->col, key->width, LV_GRID_ALIGN_STRETCH, key->row, 1);

        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(key->bg_color), 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

        // Save button reference for simulation API
        m_key_buttons[i] = btn;

        // Label (Child 0)
        const char* label_text = key->labels[0] ? key->labels[0] : "";
        lv_obj_t* label        = lv_label_create(btn);
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_color(label, lv_color_hex(key->text_color), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_center(label);

        // Indicator (Child 1)
        lv_obj_t* indicator = lv_obj_create(btn);
        lv_obj_set_size(indicator, 20, 3);
        lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_radius(indicator, 2, 0);
        lv_obj_set_style_bg_color(indicator, lv_color_hex(0x808080), 0);
        lv_obj_set_style_border_width(indicator, 0, 0);

        if (key->id == KEY_ID_AA) {
            m_aa_indicator = indicator;
            // Don't use CHECKABLE - it causes color change
            // Instead, we'll handle repeat prevention in event handler
        }
        if (key->id == KEY_ID_SYM) {
            // Don't use CHECKABLE - it causes color change
        }

        bool is_lockable = (key->id == KEY_ID_SYM || key->id == KEY_ID_AA);
        if (!is_lockable) lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);

        // Sub-Label (Child 2)
        // Optimized: only create if Mode 2 diff from Mode 0
        const char* l2 = key->labels[KB_MODE_2];
        const char* l0 = key->labels[0];

        if (l2 && l0 && strcmp(l2, l0) != 0 && strlen(l2) > 0) {
            lv_obj_t* sub_label = lv_label_create(btn);
            if (sub_label) {
                lv_obj_set_style_text_color(sub_label, lv_color_hex(0x4080FF), 0);
                lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_14, 0);
                lv_obj_align(sub_label, LV_ALIGN_TOP_RIGHT, -3, 3);
                lv_label_set_text(sub_label, l2);
            }
        }

        // Events
        // We use eventHandlerStatic, which retrieves 'this' from container user_data
        // We PASS 'key' as user_data to the callback
        if (key->id == KEY_ID_AA || key->id == KEY_ID_SYM || key->id == KEY_ID_CTRL || key->id == KEY_ID_ALT) {
            // Special keys: handle PRESSED and RELEASED for state tracking
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_PRESSED, (void*)key);
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_RELEASED, (void*)key);
        } else if (key->id == KB_KEY_ESC) {
            // ESC key: handle PRESSED for hotkey, CLICKED for normal input
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_PRESSED, (void*)key);
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_CLICKED, (void*)key);
        } else {
            // Normal keys: handle PRESSED (for hotkey with modifier), CLICKED and LONG_PRESSED_REPEAT
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_PRESSED, (void*)key);
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_CLICKED, (void*)key);
            lv_obj_add_event_cb(btn, eventHandlerStatic, LV_EVENT_LONG_PRESSED_REPEAT, (void*)key);
        }
    }

    updateUiState();
    return m_kb_cont;
}

// --- Virtual Keyboard Simulation Implementation ---

void M5Tab5Keyboard::simulateCharacter(const char* key_char)
{
    if (!key_char) return;

    for (int i = 0; i < KEYBOARD_KEY_COUNT; i++) {
        const tab5_key_t* key = &tab5_key_map[i];
        const char* val       = key->values[0];
        const char* lbl       = key->labels[0];

        // Match against value (e.g. "a") or label
        if ((val && strcmp(val, key_char) == 0) || (lbl && strcmp(lbl, key_char) == 0)) {
            simulateKeyClick(key, 100);
            return;
        }
    }
}

void M5Tab5Keyboard::simulateString(const char* str, uint32_t interval_ms)
{
    if (!str) return;

    ESP_LOGI(TAG, "Simulating string: %s", str);

    for (size_t i = 0; str[i] != '\0'; i++) {
        char current    = str[i];
        bool need_shift = false;
        bool need_sym   = false;

        // Check if we need shift for uppercase letters
        if (current >= 'A' && current <= 'Z') {
            need_shift = true;
            current    = current + 32;  // Convert to lowercase for key lookup
        }

        // Check if we need SYM for special characters
        // Comma ',' is at ID 65, Mode 2
        if (current == ',') {
            need_sym = true;
            current  = '.';  // The base key is '.'
        }

        // Convert char to string for simulateCharacter
        char key_str[2] = {current, '\0'};

        // Handle uppercase with Aa single-click logic
        if (need_shift) {
            // Single click Aa to enter SINGLE_PENDING state
            simulateKeyClick(43, 100);  // Click Aa
            lv_delay_ms(50);            // Wait for state to update

            // Then input the lowercase letter, it will be auto-capitalized
            simulateCharacter(key_str);
        }
        // Handle SYM combination
        else if (need_sym) {
            simulateKeyCombination(42, 65);  // Hold SYM, click '.'
        } else {
            // Normal character
            simulateCharacter(key_str);
        }

        // Delay between characters
        if (str[i + 1] != '\0') {
            lv_delay_ms(interval_ms);
        }
    }

    ESP_LOGI(TAG, "String simulation complete");
}

const tab5_key_t* M5Tab5Keyboard::findKeyById(uint16_t key_id) const
{
    for (int i = 0; i < KEYBOARD_KEY_COUNT; i++) {
        if (tab5_key_map[i].id == key_id) {
            return &tab5_key_map[i];
        }
    }
    ESP_LOGW(TAG, "Key ID %d not found", key_id);
    return nullptr;
}

lv_obj_t* M5Tab5Keyboard::findButtonById(uint16_t key_id) const
{
    for (int i = 0; i < KEYBOARD_KEY_COUNT; i++) {
        if (tab5_key_map[i].id == key_id) {
            return m_key_buttons[i];
        }
    }
    ESP_LOGW(TAG, "Button for key ID %d not found", key_id);
    return nullptr;
}

void M5Tab5Keyboard::sendKeyEvent(lv_obj_t* btn, const tab5_key_t* key, lv_event_code_t event_code)
{
    if (!btn || !key) {
        ESP_LOGE(TAG, "Invalid button or key for event simulation");
        return;
    }

    // Send event using LVGL 9 API
    lv_obj_send_event(btn, event_code, (void*)key);
}

void M5Tab5Keyboard::simulateInput(uint16_t key_id)
{
    const tab5_key_t* key = findKeyById(key_id);
    if (key) {
        processKeyInput(key);
    }
}

void M5Tab5Keyboard::simulateKeyPress(uint16_t key_id)
{
    const tab5_key_t* key = findKeyById(key_id);
    if (key) {
        simulateKeyPress(key);
    }
}

void M5Tab5Keyboard::simulateKeyPress(const tab5_key_t* key)
{
    if (!key) return;

    lv_obj_t* btn = findButtonById(key->id);
    if (!btn) return;

    ESP_LOGI(TAG, "Simulate key press: ID=%d", key->id);
    sendKeyEvent(btn, key, LV_EVENT_PRESSED);
}

void M5Tab5Keyboard::simulateKeyRelease(uint16_t key_id, bool trigger_click)
{
    const tab5_key_t* key = findKeyById(key_id);
    if (key) {
        simulateKeyRelease(key, trigger_click);
    }
}

void M5Tab5Keyboard::simulateKeyRelease(const tab5_key_t* key, bool trigger_click)
{
    if (!key) return;

    lv_obj_t* btn = findButtonById(key->id);
    if (!btn) return;

    ESP_LOGI(TAG, "Simulate key release: ID=%d, click=%d", key->id, trigger_click);
    sendKeyEvent(btn, key, LV_EVENT_RELEASED);

    // If requested, also send CLICKED event to trigger character input
    // But skip special keys (Aa/SYM) which might have different logic or not use CLICKED
    if (trigger_click) {
        if (key->id != KEY_ID_AA && key->id != KEY_ID_SYM) {
            sendKeyEvent(btn, key, LV_EVENT_CLICKED);
        }
    }
}

void M5Tab5Keyboard::simulateKeyClick(uint16_t key_id, uint32_t hold_ms)
{
    const tab5_key_t* key = findKeyById(key_id);
    if (key) {
        simulateKeyClick(key, hold_ms);
    }
}

void M5Tab5Keyboard::simulateKeyClick(const tab5_key_t* key, uint32_t hold_ms)
{
    if (!key) return;

    lv_obj_t* btn = findButtonById(key->id);
    if (!btn) return;

    ESP_LOGI(TAG, "Simulate key click: ID=%d, hold=%lu ms", key->id, hold_ms);

    // All keys use PRESSED/RELEASED for visual feedback
    sendKeyEvent(btn, key, LV_EVENT_PRESSED);
    lv_delay_ms(hold_ms);
    sendKeyEvent(btn, key, LV_EVENT_RELEASED);

    // For normal keys (not Aa/SYM), also send CLICKED event to trigger input
    if (key->id != KEY_ID_AA && key->id != KEY_ID_SYM) {
        sendKeyEvent(btn, key, LV_EVENT_CLICKED);
    }
}

void M5Tab5Keyboard::simulateKeySequence(const uint16_t* key_ids, size_t count, uint32_t interval_ms)
{
    if (!key_ids || count == 0) {
        ESP_LOGW(TAG, "Invalid key sequence");
        return;
    }

    ESP_LOGI(TAG, "Simulate key sequence: count=%d, interval=%lu ms", count, interval_ms);

    for (size_t i = 0; i < count; i++) {
        simulateKeyClick(key_ids[i], 100);
        if (i < count - 1) {
            lv_delay_ms(interval_ms);
        }
    }
}

void M5Tab5Keyboard::simulateKeyCombination(uint16_t modifier_id, uint16_t key_id, uint32_t hold_ms)
{
    const tab5_key_t* modifier_key = findKeyById(modifier_id);
    const tab5_key_t* target_key   = findKeyById(key_id);

    if (!modifier_key || !target_key) {
        ESP_LOGW(TAG, "Invalid key combination: modifier=%d, key=%d", modifier_id, key_id);
        return;
    }

    ESP_LOGI(TAG, "Simulate key combination: modifier=%d, key=%d", modifier_id, key_id);

    // Press modifier key
    simulateKeyPress(modifier_key);
    lv_delay_ms(100);

    // Click target key
    simulateKeyClick(target_key, hold_ms);
    lv_delay_ms(100);

    // Release modifier key
    simulateKeyRelease(modifier_key);

    // Extra delay to ensure state machine processes the release
    lv_delay_ms(100);

    // For Aa key (ID 43), force clear SINGLE_PENDING state after combination
    // This prevents the next character from being auto-capitalized
    if (modifier_id == KEY_ID_AA && m_caps_state == CAPS_STATE_SINGLE_PENDING) {
        ESP_LOGI(TAG, "Clearing SINGLE_PENDING after Aa combination");
        m_caps_state = CAPS_STATE_LOWER;
        stopFlashTimer();
        updateUiState();
    }

    // Extra delay after state clear
    lv_delay_ms(100);
}

// --- Global Instance for UI Access ---
static M5Tab5Keyboard* g_m5tab5_keyboard_instance = nullptr;

M5Tab5Keyboard* getM5Tab5KeyboardInstance()
{
    return g_m5tab5_keyboard_instance;
}

void setM5Tab5KeyboardInstance(M5Tab5Keyboard* instance)
{
    g_m5tab5_keyboard_instance = instance;
}
