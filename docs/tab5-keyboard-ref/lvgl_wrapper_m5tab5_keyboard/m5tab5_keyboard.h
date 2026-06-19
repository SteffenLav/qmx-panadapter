/*
 * SPDX-FileCopyrightText: 2025-2026 exia
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file m5tab5_keyboard.h
 * @brief M5Tab5 物理键盘实现
 *
 * M5Tab5 键盘是一个 14x5 的物理键盘，提供多模式、热键、模拟输入等功能
 */

#ifndef M5TAB5_KEYBOARD_H
#define M5TAB5_KEYBOARD_H

#include "lvgl_keyboard.h"
#include "m5tab5_keyboard_def.h"

#ifdef __cplusplus

/**
 * @brief M5Tab5 键盘
 *
 * 实现 M5Stack Tab5 设备的物理键盘功能，支持 14x5 网格布局
 * 提供多模式支持（小写、大写、符号）、Shift/Caps Lock 逻辑
 * 组合键支持（Ctrl、Alt）以及热键系统
 */
class M5Tab5Keyboard : public LvglKeyboard {
public:
    /**
     * @brief 构造函数
     */
    M5Tab5Keyboard();

    /**
     * @brief 析构函数
     */
    virtual ~M5Tab5Keyboard();

    /**
     * @brief 创建键盘 UI
     *
     * @param parent 父 LVGL 对象
     * @return 键盘容器对象指针
     */
    virtual lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief 设置按键禁用状态
     *
     * @param key_id 按键 ID (0-69)
     * @param disabled true=禁用，false=启用
     */
    virtual void setKeyDisabled(uint16_t key_id, bool disabled) override;

    /**
     * @brief 更新 UI 状态
     *
     * 根据当前键盘状态（大写锁定、符号模式等）更新 UI 显示
     */
    void updateUiState();

    // ========================================================================
    // 虚拟键盘模拟 API
    // ========================================================================

    /**
     * @brief 模拟单字符输入
     *
     * @param key_char 要模拟的字符（例如 "a", "1", "!"）
     */
    virtual void simulateCharacter(const char* key_char) override;

    /**
     * @brief 模拟字符串输入
     *
     * @param str 要模拟的字符串
     * @param interval_ms 字符间间隔时间（毫秒），默认 100ms
     */
    virtual void simulateString(const char* str, uint32_t interval_ms = 100) override;

    // ========================================================================
    // 布局约束
    // ========================================================================

    /**
     * @brief 获取首选宽高比
     * @return 首选宽高比值（从定义文件获取）
     */
    virtual float getPreferredAspectRatio() const override
    {
        return M5TAB5_KB_PREFERRED_ASPECT_RATIO;
    }

    /**
     * @brief 获取最小宽高比
     * @return 最小宽高比值（从定义文件获取）
     */
    virtual float getMinAspectRatio() const override
    {
        return M5TAB5_KB_MIN_ASPECT_RATIO;
    }

    /**
     * @brief 获取最大宽高比
     * @return 最大宽高比值（从定义文件获取）
     */
    virtual float getMaxAspectRatio() const override
    {
        return M5TAB5_KB_MAX_ASPECT_RATIO;
    }

    /**
     * @brief 获取最大屏幕高度占比
     * @return 最大屏幕高度占比值（0.0-1.0）
     */
    virtual float getMaxScreenHeightPct() const override
    {
        return M5TAB5_KB_MAX_SCREEN_HEIGHT_PCT;
    }

    // ========================================================================
    // 低级模拟 API（供高级使用）
    // ========================================================================

    /**
     * @brief 直接触发按键输入逻辑
     *
     * @param key_id 按键 ID
     */
    void simulateInput(uint16_t key_id);

    /**
     * @brief 模拟按键按下
     *
     * @param key_id 按键 ID
     */
    void simulateKeyPress(uint16_t key_id);

    /**
     * @brief 模拟按键按下
     *
     * @param key 按键定义指针
     */
    void simulateKeyPress(const tab5_key_t* key);

    /**
     * @brief 模拟按键释放
     *
     * @param key_id 按键 ID
     * @param trigger_click 是否触发点击事件，默认 false
     */
    void simulateKeyRelease(uint16_t key_id, bool trigger_click = false);

    /**
     * @brief 模拟按键释放
     *
     * @param key 按键定义指针
     * @param trigger_click 是否触发点击事件，默认 false
     */
    void simulateKeyRelease(const tab5_key_t* key, bool trigger_click = false);

    /**
     * @brief 模拟按键点击（按下并释放）
     *
     * @param key_id 按键 ID
     * @param hold_ms 按住时间（毫秒），默认 100ms
     */
    void simulateKeyClick(uint16_t key_id, uint32_t hold_ms = 100);

    /**
     * @brief 模拟按键点击（按下并释放）
     *
     * @param key 按键定义指针
     * @param hold_ms 按住时间（毫秒），默认 100ms
     */
    void simulateKeyClick(const tab5_key_t* key, uint32_t hold_ms = 100);

    /**
     * @brief 模拟按键序列
     *
     * @param key_ids 按键 ID 数组
     * @param count 按键数量
     * @param interval_ms 按键间隔时间（毫秒），默认 100ms
     */
    void simulateKeySequence(const uint16_t* key_ids, size_t count, uint32_t interval_ms = 100);

    /**
     * @brief 模拟组合键
     *
     * @param modifier_id 修饰键 ID (如 CTRL, ALT)
     * @param key_id 主按键 ID
     * @param hold_ms 按住时间（毫秒），默认 100ms
     */
    void simulateKeyCombination(uint16_t modifier_id, uint16_t key_id, uint32_t hold_ms = 100);

protected:
    // 键盘逻辑类型
    typedef enum {
        CAPS_STATE_LOWER = 0,
        CAPS_STATE_SINGLE_PENDING,  // 单击：下一个字符大写，蓝色闪烁
        CAPS_STATE_HOLDING,         // 按住：当前大写，蓝色闪烁
        CAPS_STATE_LOCKED           // 双击：锁定大写，蓝色常亮
    } caps_state_t;

    // 成员变量
    bool m_key_disabled[KEYBOARD_KEY_COUNT];      // 按键禁用状态数组
    lv_obj_t* m_key_buttons[KEYBOARD_KEY_COUNT];  // 按键对象引用数组
    caps_state_t m_caps_state;                    // 大写锁定状态
    bool m_sym_mode;                              // 符号模式
    bool m_sym_holding;                           // 符号键按住状态

    // 特殊按键 ID: 42=SYM, 43=Aa, 56=CTRL, 57=ALT
    static const int KEY_ID_SYM  = 42;
    static const int KEY_ID_AA   = 43;
    static const int KEY_ID_CTRL = 56;
    static const int KEY_ID_ALT  = 57;

    // 闪烁/定时器逻辑
    lv_timer_t* m_flash_timer;  // 闪烁定时器
    bool m_flash_on;            // 闪烁状态
    lv_obj_t* m_aa_indicator;   // Aa 指示器对象

    // 点击时间
    uint32_t m_last_aa_click_time;    // 上次 Aa 点击时间
    uint32_t m_aa_press_start_time;   // Aa 按下开始时间
    uint32_t m_sym_press_start_time;  // SYM 按下开始时间
    uint32_t m_last_aa_release_time;  // 上次 Aa 释放时间（用于防抖）

    // 逻辑标志
    bool m_ignore_next_aa_release;  // 忽略下一次 Aa 释放事件

    // 辅助函数
    bool isUpperMode() const;                 // 是否为大写模式
    kb_mode_t getCurrentMappingMode() const;  // 获取当前映射模式
    void startFlashTimer();                   // 启动闪烁定时器
    void stopFlashTimer();                    // 停止闪烁定时器

    // 虚拟键盘模拟辅助函数
    const tab5_key_t* findKeyById(uint16_t key_id) const;  // 根据 ID 查找按键
    lv_obj_t* findButtonById(uint16_t key_id) const;       // 根据 ID 查找按钮对象
    void sendKeyEvent(lv_obj_t* btn, const tab5_key_t* key, lv_event_code_t event_code);  // 发送按键事件
    void processKeyInput(const tab5_key_t* key);                                          // 处理按键输入逻辑

    // 事件处理函数
    static void eventHandlerStatic(lv_event_t* e);      // 静态事件处理器
    static void flashTimerCbStatic(lv_timer_t* timer);  // 静态闪烁定时器回调

    void handleKeyEvent(lv_event_t* e);    // 处理按键事件
    void handleShiftEvent(lv_event_t* e);  // 处理 Shift 事件
    void handleSymEvent(lv_event_t* e);    // 处理符号键事件
    void handleFlashTimer();               // 处理闪烁定时器
};

/**
 * @brief 获取全局 M5Tab5Keyboard 实例
 *
 * @return M5Tab5Keyboard 实例指针，如果未创建则返回 nullptr
 */
M5Tab5Keyboard* getM5Tab5KeyboardInstance();

/**
 * @brief 设置全局 M5Tab5Keyboard 实例
 *
 * @param instance M5Tab5Keyboard 实例指针
 */
void setM5Tab5KeyboardInstance(M5Tab5Keyboard* instance);

#endif  // __cplusplus

#endif  // M5TAB5_KEYBOARD_H
