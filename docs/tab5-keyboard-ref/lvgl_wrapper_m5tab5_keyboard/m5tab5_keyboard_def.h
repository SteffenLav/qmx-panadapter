/*
 * SPDX-FileCopyrightText: 2025-2026 exia
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file m5tab5_keyboard_def.h
 * @brief M5Tab5 键盘定义文件
 *
 * 包含 M5Tab5 键盘的所有常量定义、枚举、结构体和按键映射表
 */

#ifndef M5TAB5_KEYBOARD_DEF_H
#define M5TAB5_KEYBOARD_DEF_H

#include <stdint.h>
#include <stddef.h>

/** @defgroup M5TAB5_KB_LAYOUT 键盘布局参数
 * @{
 */

/** 键盘行数 */
#define KEYBOARD_ROWS 5

/** 键盘列数 */
#define KEYBOARD_COLS 14

/** 按键总数 */
#define KEYBOARD_KEY_COUNT (KEYBOARD_ROWS * KEYBOARD_COLS)

/** @} */

/** @defgroup M5TAB5_KB_ASPECT 键盘宽高比限制
 * @{
 */

/** 首选宽高比 (1254/432) */
#define M5TAB5_KB_PREFERRED_ASPECT_RATIO 2.9f

/** 最小宽高比 (防止太高) */
#define M5TAB5_KB_MIN_ASPECT_RATIO 2.0f

/** 最大宽高比 (防止太扁) */
#define M5TAB5_KB_MAX_ASPECT_RATIO 4.0f

/** 最大屏幕高度占比 */
#define M5TAB5_KB_MAX_SCREEN_HEIGHT_PCT 0.60f

/** @} */

/** @defgroup M5TAB5_KB_HOTKEY 特殊按键 ID 定义
 * @{
 */

/** 匹配所有按键 */
#define M5TAB5_HOTKEY_ANY_KEY 0xFFFF

/** @} */

/** @} */

/** @defgroup M5TAB5_KB_HOTKEY_MACROS M5TAB5 键盘便捷热键注册
 * @{
 *
 * @note 热键系统要求必须有修饰键（CTRL/ALT）或者是特殊键（ESC）
 * @note 普通字符键不能单独作为热键触发条件
 * @note 使用 kb_key_id_t 枚举值指定按键
 */

/**
 * @brief Ctrl+键（只有 Ctrl，不包括 Ctrl+Alt）
 *
 * @param kb 键盘对象指针
 * @param key_id 按键 ID（kb_key_id_t 枚举）
 * @param callback 回调函数
 *
 * @return 热键 ID
 */
#define M5TAB5_REGISTER_HOTKEY_CTRL(kb, key_id, callback) \
    (kb)->registerHotkey(key_id, HOTKEY_EVENT_PRESSED, callback, nullptr, true, false, false, true, false)

/**
 * @brief Alt+键（只有 Alt，不包括 Ctrl+Alt）
 *
 * @param kb 键盘对象指针
 * @param key_id 按键 ID（kb_key_id_t 枚举）
 * @param callback 回调函数
 *
 * @return 热键 ID
 */
#define M5TAB5_REGISTER_HOTKEY_ALT(kb, key_id, callback) \
    (kb)->registerHotkey(key_id, HOTKEY_EVENT_PRESSED, callback, nullptr, false, true, false, false, true)

/**
 * @brief Ctrl+Alt+键（同时需要 Ctrl 与 Alt）
 *
 * @param kb 键盘对象指针
 * @param key_id 按键 ID（kb_key_id_t 枚举）
 * @param callback 回调函数
 *
 * @return 热键 ID
 */
#define M5TAB5_REGISTER_HOTKEY_CTRL_ALT(kb, key_id, callback) \
    (kb)->registerHotkey(key_id, HOTKEY_EVENT_PRESSED, callback, nullptr, true, true, false, false, false)

/**
 * @brief ESC 键
 *
 * @param kb 键盘对象指针
 * @param callback 回调函数
 *
 * @return 热键 ID
 */
#define M5TAB5_REGISTER_HOTKEY_ESC(kb, callback) \
    (kb)->registerHotkey(KB_KEY_ESC, HOTKEY_EVENT_PRESSED, callback, nullptr, false, false, false, false, false)

/** @} */

/** @defgroup M5TAB5_KB_KEY_IDS 按键 ID 枚举
 * @{
 */

/**
 * @brief 按键 ID 枚举
 *
 * 定义键盘上所有按键 of ID，按行列顺序排列
 */
typedef enum {
    // Row 0
    KB_KEY_ESC = 0,
    KB_KEY_1,
    KB_KEY_2,
    KB_KEY_3,
    KB_KEY_4,
    KB_KEY_5,
    KB_KEY_6,
    KB_KEY_7,
    KB_KEY_8,
    KB_KEY_9,
    KB_KEY_0,
    KB_KEY_MINUS,
    KB_KEY_PLUS,
    KB_KEY_DEL,

    // Row 1
    KB_KEY_BACKTICK,   // `
    KB_KEY_EXCLAIM,    // !
    KB_KEY_AT,         // @
    KB_KEY_HASH,       // #
    KB_KEY_DOLLAR,     // $
    KB_KEY_PERCENT,    // %
    KB_KEY_CARET,      // ^
    KB_KEY_AMPERSAND,  // &
    KB_KEY_ASTERISK,   // *
    KB_KEY_LPAREN,     // (
    KB_KEY_RPAREN,     // )
    KB_KEY_LBRACKET,   // [
    KB_KEY_RBRACKET,   // ]
    KB_KEY_BACKSLASH,  // '\'

    // Row 2
    KB_KEY_TAB,
    KB_KEY_Q,
    KB_KEY_W,
    KB_KEY_E,
    KB_KEY_R,
    KB_KEY_T,
    KB_KEY_Y,
    KB_KEY_U,
    KB_KEY_I,
    KB_KEY_O,
    KB_KEY_P,
    KB_KEY_SEMICOLON,  // ;
    KB_KEY_QUOTE,      // '
    KB_KEY_BS,         // Backspace

    // Row 3
    KB_KEY_SYM,
    KB_KEY_AA,  // Shift/Caps
    KB_KEY_A,
    KB_KEY_S,
    KB_KEY_D,
    KB_KEY_F,
    KB_KEY_G,
    KB_KEY_H,
    KB_KEY_J,
    KB_KEY_K,
    KB_KEY_L,
    KB_KEY_UP,          // 方向键上
    KB_KEY_UNDERSCORE,  // _
    KB_KEY_ENTER,

    // Row 4
    KB_KEY_CTRL,
    KB_KEY_ALT,
    KB_KEY_Z,
    KB_KEY_X,
    KB_KEY_C,
    KB_KEY_V,
    KB_KEY_B,
    KB_KEY_N,
    KB_KEY_M,
    KB_KEY_DOT,    // .
    KB_KEY_LEFT,   // 方向键左
    KB_KEY_DOWN,   // 方向键下
    KB_KEY_RIGHT,  // 方向键右
    KB_KEY_SPACE,

    KB_KEY_COUNT  ///< 总数，应该等于 KEYBOARD_KEY_COUNT (70)
} kb_key_id_t;

/** @} */

/** @defgroup M5TAB5_KB_COLORS 颜色定义 (RGB888)
 * @{
 */

#define KB_COLOR_BG_NORM  0xF0F0F0  ///< 正常按键背景 (浅灰)
#define KB_COLOR_TXT_NORM 0x000000  ///< 正常按键文字 (黑)

#define KB_COLOR_BG_ESC    0xFFCC00  ///< ESC 键背景 (黄)
#define KB_COLOR_BG_DEL    0xFF0055  ///< DEL 键背景 (红)
#define KB_COLOR_TXT_WHITE 0xFFFFFF  ///< 白色文字

#define KB_COLOR_BG_BLUE   0x4080FF  ///< TAB, SYM 键背景 (蓝)
#define KB_COLOR_BG_ORANGE 0xE06030  ///< Aa 键背景 (橙)
#define KB_COLOR_BG_CYAN   0x00CCEE  ///< CTRL 键背景 (青)
#define KB_COLOR_BG_GRAY   0xC0C0C0  ///< ALT 键背景 (灰)
#define KB_COLOR_BG_BLACK  0x000000  ///< BS, ENT 键背景 (黑)

/** @} */

/** @defgroup M5TAB5_KB_MODES 键盘模式
 * @{
 */

/**
 * @brief 键盘模式枚举
 */
typedef enum {
    KB_MODE_0 = 0,  ///< 默认/小写模式
    KB_MODE_1,      ///< Shift/大写模式
    KB_MODE_2,      ///< Symbol/符号模式
    // KB_MODE_3,      ///< Fn 模式 (预留)
    KB_MODE_COUNT  ///< 模式总数
} kb_mode_t;

/** @} */

/** @defgroup M5TAB5_KB_KEY_DEF 按键定义结构体和宏
 * @{
 */

/**
 * @brief 按键定义结构体
 *
 * 定义每个按键的属性，包括位置、外观、文本和值
 */
typedef struct {
    uint16_t id;    ///< 唯一ID
    uint8_t row;    ///< 行索引
    uint8_t col;    ///< 列索引
    uint8_t width;  ///< 跨度

    /* 外观属性 */
    uint32_t bg_color;    ///< 背景颜色
    uint32_t text_color;  ///< 文字颜色

    /* 多层映射: 可定义任意层的文本(Label)与输出值(Value) */
    /* 即使某一层看起来一样，也请显式定义，以便获得最大自由度 */
    const char* labels[KB_MODE_COUNT];  ///< 显示文本（每个模式）
    const char* values[KB_MODE_COUNT];  ///< 输出值（每个模式）

    bool disabled;       ///< 禁用状态标志
    uint32_t user_data;  ///< 用户自定义数据
} tab5_key_t;

/**
 * @brief 万能映射宏
 *
 * 用于定义按键映射表中的每个按键
 *
 * @param id 按键 ID
 * @param r 行索引
 * @param c 列索引
 * @param w 宽度(跨度)
 * @param bg 背景颜色
 * @param fg 文字颜色
 * @param l0 模式0显示文本
 * @param v0 模式0输出值
 * @param l1 模式1显示文本
 * @param v1 模式1输出值
 * @param l2 模式2显示文本
 * @param v2 模式2输出值
 */
#define KEY_ITEM(id, r, c, w, bg, fg, l0, v0, l1, v1, l2, v2) \
    {id, r, c, w, bg, fg, {l0, l1, l2}, {v0, v1, v2}, false, 0}

/** @} */

/** @defgroup M5TAB5_KB_KEYMAP 键盘映射
 * @{
 */

/**
 * @brief M5Tab5 键盘映射表
 *
 * 定义所有 70 个按键的属性和多模式映射
 */
static const tab5_key_t tab5_key_map[] = {
    // ID, R, C, W, BG, FG,                              [Mode 0: Lower]      [Mode 1: Upper]      [Mode 2: Sym]

    // --- Row 0 ---
    KEY_ITEM(0, 0, 0, 1, KB_COLOR_BG_ESC, KB_COLOR_TXT_NORM, "ESC", "\x1B", "ESC", "\x1B", NULL, NULL),
    KEY_ITEM(1, 0, 1, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "1", "1", "1", "1", NULL, NULL),
    KEY_ITEM(2, 0, 2, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "2", "2", "2", "2", NULL, NULL),
    KEY_ITEM(3, 0, 3, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "3", "3", "3", "3", NULL, NULL),
    KEY_ITEM(4, 0, 4, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "4", "4", "4", "4", NULL, NULL),
    KEY_ITEM(5, 0, 5, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "5", "5", "5", "5", NULL, NULL),
    KEY_ITEM(6, 0, 6, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "6", "6", "6", "6", NULL, NULL),
    KEY_ITEM(7, 0, 7, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "7", "7", "7", "7", NULL, NULL),
    KEY_ITEM(8, 0, 8, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "8", "8", "8", "8", NULL, NULL),
    KEY_ITEM(9, 0, 9, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "9", "9", "9", "9", NULL, NULL),
    KEY_ITEM(10, 0, 10, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "0", "0", "0", "0", NULL, NULL),
    KEY_ITEM(11, 0, 11, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "-", "-", "-", "-", NULL, NULL),
    KEY_ITEM(12, 0, 12, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "+", "+", "+", "+", NULL, NULL),
    KEY_ITEM(13, 0, 13, 1, KB_COLOR_BG_DEL, KB_COLOR_TXT_WHITE, "DEL", "\x7F", "DEL", "\x7F", NULL, NULL),

    // --- Row 1 ---
    KEY_ITEM(14, 1, 0, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "`", "`", "`", "`", "~", "~"),
    KEY_ITEM(15, 1, 1, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "!", "!", "!", "!", "?", "?"),
    KEY_ITEM(16, 1, 2, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "@", "@", "@", "@", NULL, NULL),
    KEY_ITEM(17, 1, 3, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "#", "#", "#", "#", NULL, NULL),
    KEY_ITEM(18, 1, 4, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "$", "$", "$", "$", NULL, NULL),
    KEY_ITEM(19, 1, 5, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "%", "%", "%", "%", NULL, NULL),
    KEY_ITEM(20, 1, 6, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "^", "^", "^", "^", NULL, NULL),
    KEY_ITEM(21, 1, 7, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "&", "&", "&", "&", NULL, NULL),
    KEY_ITEM(22, 1, 8, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "*", "*", "*", "*", "/", "/"),
    KEY_ITEM(23, 1, 9, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "(", "(", "(", "(", "<", "<"),
    KEY_ITEM(24, 1, 10, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, ")", ")", ")", ")", ">", ">"),
    KEY_ITEM(25, 1, 11, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "[", "[", "[", "[", "{", "{"),
    KEY_ITEM(26, 1, 12, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "]", "]", "]", "]", "}", "}"),
    KEY_ITEM(27, 1, 13, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "\\", "\\", "\\", "\\", "|", "|"),

    // --- Row 2 ---
    KEY_ITEM(28, 2, 0, 1, KB_COLOR_BG_BLUE, KB_COLOR_TXT_WHITE, "TAB", "\t", "TAB", "\t", NULL, NULL),
    KEY_ITEM(29, 2, 1, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "q", "q", "Q", "Q", NULL, NULL),
    KEY_ITEM(30, 2, 2, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "w", "w", "W", "W", NULL, NULL),
    KEY_ITEM(31, 2, 3, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "e", "e", "E", "E", NULL, NULL),
    KEY_ITEM(32, 2, 4, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "r", "r", "R", "R", NULL, NULL),
    KEY_ITEM(33, 2, 5, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "t", "t", "T", "T", NULL, NULL),
    KEY_ITEM(34, 2, 6, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "y", "y", "Y", "Y", NULL, NULL),
    KEY_ITEM(35, 2, 7, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "u", "u", "U", "U", NULL, NULL),
    KEY_ITEM(36, 2, 8, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "i", "i", "I", "I", NULL, NULL),
    KEY_ITEM(37, 2, 9, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "o", "o", "O", "O", NULL, NULL),
    KEY_ITEM(38, 2, 10, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "p", "p", "P", "P", NULL, NULL),
    KEY_ITEM(39, 2, 11, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, ";", ";", ";", ";", ":", ":"),
    KEY_ITEM(40, 2, 12, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "'", "'", "'", "'", "\"", "\""),
    KEY_ITEM(41, 2, 13, 1, KB_COLOR_BG_BLACK, KB_COLOR_TXT_WHITE, "BS", "\b", "BS", "\b", NULL, NULL),

    // --- Row 3 ---
    KEY_ITEM(42, 3, 0, 1, KB_COLOR_BG_BLUE, KB_COLOR_TXT_WHITE, "SYM", NULL, "SYM", NULL, NULL, NULL),
    KEY_ITEM(43, 3, 1, 1, KB_COLOR_BG_ORANGE, KB_COLOR_TXT_WHITE, "Aa", NULL, "Aa", NULL, NULL, NULL),
    KEY_ITEM(44, 3, 2, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "a", "a", "A", "A", NULL, NULL),
    KEY_ITEM(45, 3, 3, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "s", "s", "S", "S", NULL, NULL),
    KEY_ITEM(46, 3, 4, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "d", "d", "D", "D", NULL, NULL),
    KEY_ITEM(47, 3, 5, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "f", "f", "F", "F", NULL, NULL),
    KEY_ITEM(48, 3, 6, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "g", "g", "G", "G", NULL, NULL),
    KEY_ITEM(49, 3, 7, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "h", "h", "H", "H", NULL, NULL),
    KEY_ITEM(50, 3, 8, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "j", "j", "J", "J", NULL, NULL),
    KEY_ITEM(51, 3, 9, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "k", "k", "K", "K", NULL, NULL),
    KEY_ITEM(52, 3, 10, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "l", "l", "L", "L", NULL, NULL),
    KEY_ITEM(53, 3, 11, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "UP", "\x11", "UP", "\x11", NULL, NULL),
    KEY_ITEM(54, 3, 12, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "_", "_", "_", "_", "=", "="),
    KEY_ITEM(55, 3, 13, 1, KB_COLOR_BG_BLACK, KB_COLOR_TXT_WHITE, "ENT", "\n", "ENT", "\n", NULL, NULL),

    // --- Row 4 ---
    KEY_ITEM(56, 4, 0, 1, KB_COLOR_BG_CYAN, KB_COLOR_TXT_NORM, "CTRL", NULL, "CTRL", NULL, NULL, NULL),
    KEY_ITEM(57, 4, 1, 1, KB_COLOR_BG_GRAY, KB_COLOR_TXT_NORM, "ALT", NULL, "ALT", NULL, NULL, NULL),
    KEY_ITEM(58, 4, 2, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "z", "z", "Z", "Z", NULL, NULL),
    KEY_ITEM(59, 4, 3, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "x", "x", "X", "X", NULL, NULL),
    KEY_ITEM(60, 4, 4, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "c", "c", "C", "C", NULL, NULL),
    KEY_ITEM(61, 4, 5, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "v", "v", "V", "V", NULL, NULL),
    KEY_ITEM(62, 4, 6, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "b", "b", "B", "B", NULL, NULL),
    KEY_ITEM(63, 4, 7, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "n", "n", "N", "N", NULL, NULL),
    KEY_ITEM(64, 4, 8, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "m", "m", "M", "M", NULL, NULL),
    KEY_ITEM(65, 4, 9, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, ".", ".", ".", ".", ",", ","),
    KEY_ITEM(66, 4, 10, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "LT", "\x14", "LT", "\x14", NULL, NULL),
    KEY_ITEM(67, 4, 11, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "DN", "\x12", "DN", "\x12", NULL, NULL),
    KEY_ITEM(68, 4, 12, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "RT", "\x13", "RT", "\x13", NULL, NULL),
    KEY_ITEM(69, 4, 13, 1, KB_COLOR_BG_NORM, KB_COLOR_TXT_NORM, "SPC", " ", "SPC", " ", NULL, NULL),
};

/** @} */

#endif  // M5TAB5_KEYBOARD_DEF_H
