/*
 * SPDX-FileCopyrightText: 2025-2026 M5Stack
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _M5_TAB5_KEYBOARD_H_
#define _M5_TAB5_KEYBOARD_H_

#include "m5_tab5_keyboard_i2c_compat.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef ARDUINO
// Arduino specific FreeRTOS
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#endif

// ============================
// Error Codes
// ============================
typedef enum {
    M5_TAB5_KB_OK                = 0,
    M5_TAB5_KB_FAIL              = -1,
    M5_TAB5_KB_ERR_I2C_CONFIG    = -2,
    M5_TAB5_KB_ERR_INVALID_ARG   = -3,
    M5_TAB5_KB_ERR_TIMEOUT       = -4,
    M5_TAB5_KB_ERR_NOT_SUPPORTED = -5,
    M5_TAB5_KB_ERR_I2C_COMM      = -6,
    M5_TAB5_KB_ERR_NOT_INIT      = -7,
    M5_TAB5_KB_ERR_INTERNAL      = -8,
} m5_tab5_kb_err_t;

// ============================
// Device Constants
// ============================
#define M5_TAB5_KB_DEFAULT_ADDR    0x6D
#define M5_TAB5_KB_I2C_RETRY_COUNT 2

#define M5_TAB5_KB_DEFAULT_SDA 0
#define M5_TAB5_KB_DEFAULT_SCL 1
#define M5_TAB5_KB_DEFAULT_INT 50

// ============================
// I2C Frequency Constants
// ============================
#define M5_TAB5_KB_I2C_FREQ_100K    100000
#define M5_TAB5_KB_I2C_FREQ_400K    400000
#define M5_TAB5_KB_I2C_FREQ_DEFAULT M5_TAB5_KB_I2C_FREQ_100K

// ============================
// Register Addresses
// ============================
#define M5_TAB5_KB_REG_INT_CFG    0x00
#define M5_TAB5_KB_REG_INT_STA    0x01
#define M5_TAB5_KB_REG_EVENT_NUM  0x02
#define M5_TAB5_KB_REG_BRIGHTNESS 0x03

#define M5_TAB5_KB_REG_KEYBOARD_MODE 0x10
#define M5_TAB5_KB_REG_RGB_MODE      0x11

#define M5_TAB5_KB_REG_KEY_EVENT 0x20
#define M5_TAB5_KB_REG_HID_EVENT 0x30

#define M5_TAB5_KB_REG_CHAR_EVENT_LEN  0x40
#define M5_TAB5_KB_REG_CHAR_EVENT_BASE 0x50

#define M5_TAB5_KB_REG_RGB_COLOR_BASE 0x60
#define M5_TAB5_KB_REG_VERSION        0xFE
#define M5_TAB5_KB_REG_I2C_ADDR       0xFF

// ============================
// Log Level Definitions
// ============================
typedef enum {
    M5_TAB5_KB_LOG_LEVEL_NONE = 0,
    M5_TAB5_KB_LOG_LEVEL_ERROR,
    M5_TAB5_KB_LOG_LEVEL_WARN,
    M5_TAB5_KB_LOG_LEVEL_INFO,
    M5_TAB5_KB_LOG_LEVEL_DEBUG,
    M5_TAB5_KB_LOG_LEVEL_VERBOSE
} m5_tab5_kb_log_level_t;

// ============================
// Interrupt Handling Mode
// ============================
typedef enum {
    M5_TAB5_KB_INT_MODE_DISABLED = 0,
    M5_TAB5_KB_INT_MODE_POLLING,
    M5_TAB5_KB_INT_MODE_HARDWARE
} m5_tab5_kb_int_mode_t;

// ============================
// Callback Types
// ============================
typedef void (*m5_tab5_kb_callback_t)(void);
typedef void (*m5_tab5_kb_callback_arg_t)(void*);

// ============================
// Keyboard Working Mode
// ============================
typedef enum {
    M5_TAB5_KB_MODE_NORMAL = 0,
    M5_TAB5_KB_MODE_HID    = 1,
    M5_TAB5_KB_MODE_STRING = 2,
    M5_TAB5_KB_MODE_BLE    = 3
} m5_tab5_kb_mode_t;

// ============================
// RGB Working Mode
// ============================
typedef enum { M5_TAB5_KB_RGB_MODE_BINDING = 0, M5_TAB5_KB_RGB_MODE_CUSTOM = 1 } m5_tab5_kb_rgb_mode_t;

// ============================
// Key Event Structure
// ============================
typedef struct {
    m5_tab5_kb_mode_t type;

    // Normal Mode
    bool pressed;
    uint8_t row;
    uint8_t col;
    uint8_t raw_data;

    // HID Mode
    uint8_t hid_modifier;
    uint8_t hid_key_code;

    // String Mode
    uint8_t str_modifier;
    uint8_t str_len;
    char str_data[16];
} m5_tab5_key_event_t;

typedef void (*m5_tab5_key_callback_t)(m5_tab5_key_event_t event, void* arg);

// ============================
// Register Access Intercept Callback
// ============================
typedef enum {
    M5_TAB5_REG_ACCESS_READ  = 0,
    M5_TAB5_REG_ACCESS_WRITE = 1,
} m5_tab5_reg_access_type_t;

typedef void (*m5_tab5_reg_access_cb_t)(uint8_t reg, uint8_t value, m5_tab5_reg_access_type_t type, void* user_data);

// ============================
// M5Tab5Keyboard Driver Class
// ============================
namespace m5 {

class M5Tab5Keyboard {
public:
    M5Tab5Keyboard();
    ~M5Tab5Keyboard();

    // ========================
    // Initialization
    // ========================
#ifdef ARDUINO
    m5_tab5_kb_err_t begin(TwoWire* wire, uint8_t addr = M5_TAB5_KB_DEFAULT_ADDR, uint8_t sda = -1, uint8_t scl = -1,
                           uint32_t speed = 100000, int8_t intPin = -1,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_POLLING);
#else  // ESP-IDF
    m5_tab5_kb_err_t begin(i2c_port_t port = I2C_NUM_0, uint8_t addr = M5_TAB5_KB_DEFAULT_ADDR,
                           int sda = M5_TAB5_KB_DEFAULT_SDA, int scl = M5_TAB5_KB_DEFAULT_SCL,
                           uint32_t speed                = M5_TAB5_KB_I2C_FREQ_100K,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_POLLING);

    m5_tab5_kb_err_t begin(i2c_port_t port, uint8_t addr, int sda, int scl, uint32_t speed, int intPin,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_HARDWARE);

    m5_tab5_kb_err_t begin(i2c_master_bus_handle_t bus, uint8_t addr = M5_TAB5_KB_DEFAULT_ADDR,
                           uint32_t speed                = M5_TAB5_KB_I2C_FREQ_100K,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_POLLING);

    m5_tab5_kb_err_t begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t speed, int intPin,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_HARDWARE);

    m5_tab5_kb_err_t begin(i2c_bus_handle_t bus, uint8_t addr = M5_TAB5_KB_DEFAULT_ADDR,
                           uint32_t speed                = M5_TAB5_KB_I2C_FREQ_100K,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_POLLING);

    m5_tab5_kb_err_t begin(i2c_bus_handle_t bus, uint8_t addr, uint32_t speed, int intPin,
                           m5_tab5_kb_int_mode_t intMode = M5_TAB5_KB_INT_MODE_HARDWARE);
#endif

    void end();
    bool isInitialized() const;

    // ========================
    // Log Settings
    // ========================
    static void setLogLevel(m5_tab5_kb_log_level_t level);
    static m5_tab5_kb_log_level_t getLogLevel();

    // ========================
    // Device Information
    // ========================
    m5_tab5_kb_err_t getVersion(uint8_t* version);

    m5_tab5_kb_err_t getRGBMode(m5_tab5_kb_rgb_mode_t* mode);
    m5_tab5_kb_err_t setRGBMode(uint8_t mode);

    m5_tab5_kb_err_t getRGB(uint8_t index, uint8_t* r, uint8_t* g, uint8_t* b);
    m5_tab5_kb_err_t getBothRGB(uint8_t* r1, uint8_t* g1, uint8_t* b1, uint8_t* r2, uint8_t* g2, uint8_t* b2);

    m5_tab5_kb_err_t setRGB(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    m5_tab5_kb_err_t setBothRGB(uint8_t r, uint8_t g, uint8_t b);

    m5_tab5_kb_err_t setBrightness(uint8_t brightness);
    m5_tab5_kb_err_t getBrightness(uint8_t* brightness);

    m5_tab5_kb_err_t setI2CAddress(uint8_t newAddr);
    m5_tab5_kb_err_t getI2CAddress(uint8_t* addr);

    // ========================
    // Interrupt Functions
    // ========================
    m5_tab5_kb_err_t setInterruptMode(m5_tab5_kb_int_mode_t intMode, uint32_t pollingIntervalMs = 100);

    void attachInterrupt(m5_tab5_kb_callback_t callback);
    void attachInterruptArg(m5_tab5_kb_callback_arg_t callback, void* arg);
    void detachInterrupt();

    m5_tab5_kb_err_t getInterruptStatus(uint8_t* status);
    m5_tab5_kb_err_t clearInterruptStatus();
    m5_tab5_kb_err_t setInterruptConfig(uint8_t config);
    m5_tab5_kb_err_t getInterruptConfig(uint8_t* config);

    m5_tab5_kb_err_t setKeyboardMode(uint8_t mode);
    m5_tab5_kb_err_t getKeyboardMode(uint8_t* mode);

    m5_tab5_kb_err_t getKeyEvent(uint8_t* event);
    m5_tab5_kb_err_t getEventCount(uint8_t* count);
    m5_tab5_kb_err_t clearEventQueue();

    m5_tab5_kb_err_t enableNormalMode(m5_tab5_key_callback_t callback = nullptr, void* arg = nullptr);
    m5_tab5_kb_err_t enableHIDMode(m5_tab5_key_callback_t callback = nullptr, void* arg = nullptr);
    m5_tab5_kb_err_t enableStringMode(m5_tab5_key_callback_t callback = nullptr, void* arg = nullptr);

    m5_tab5_kb_err_t setMode(m5_tab5_kb_mode_t mode);
    void setKeyCallback(m5_tab5_key_callback_t callback, void* arg = nullptr);
    m5_tab5_kb_mode_t getMode() const;

    // ========================
    // Raw Register Access
    // ========================
    m5_tab5_kb_err_t readRegByte(uint8_t reg, uint8_t* value);
    m5_tab5_kb_err_t writeRegByte(uint8_t reg, uint8_t value);

    // ========================
    // Register Access Intercept Callback
    // ========================
    void setRegAccessCallback(m5_tab5_reg_access_cb_t cb, void* user_data = nullptr);
    void clearRegAccessCallback();

private:
    uint8_t _addr;
    bool _initialized;
    m5_tab5_kb_mode_t _currentMode;
    uint32_t _requestedSpeed;

    m5_tab5_kb_int_mode_t _intMode;
    int8_t _intPin;
    uint32_t _pollingInterval;

    m5_tab5_kb_callback_t _callback;
    m5_tab5_kb_callback_arg_t _callbackArg;
    void* _callbackArgData;

    m5_tab5_key_callback_t _keyCallback;
    void* _keyCallbackArg;

#ifdef ARDUINO
    TwoWire* _wire;
    uint8_t _sda;
    uint8_t _scl;
#else
    m5_tab5_kb_i2c_driver_t _i2cDriverType;

    i2c_master_bus_handle_t _i2c_master_bus;
    i2c_master_dev_handle_t _i2c_master_dev;
    i2c_bus_handle_t _i2c_bus;
    i2c_bus_device_handle_t _i2c_device;

    bool _busExternal;

    int _sda;
    int _scl;
    i2c_port_t _port;

    TaskHandle_t _pollTask;
    QueueHandle_t _intrQueue;
    SemaphoreHandle_t _opMutex;
#endif

    // ========================
    // Internal Helper Functions
    // ========================
    bool _writeReg(uint8_t reg, uint8_t value);
    bool _writeReg16(uint8_t reg, uint16_t value);
    bool _readReg(uint8_t reg, uint8_t* value);
    bool _readReg16(uint8_t reg, uint16_t* value);
    bool _writeBytes(uint8_t reg, const uint8_t* data, uint8_t len);
    bool _readBytes(uint8_t reg, uint8_t* data, uint8_t len);

    bool _initDevice();
    void _handleInterrupt();

    bool _isValidI2cFrequency(uint32_t speed);

#ifdef ARDUINO
    bool _setupPollingArduino();
    void _cleanupPollingArduino();
    static void _pollTaskArduino(void* arg);
    bool _setupHardwareInterruptArduino();
    void _cleanupHardwareInterruptArduino();
#else
    static void _pollTaskFunc(void* arg);
    static void IRAM_ATTR _isrHandler(void* arg);
    bool _setupHardwareInterrupt();
    bool _setupPolling();
    void _cleanupPolling();
    void _cleanupHardwareInterrupt();
#endif

    m5_tab5_reg_access_cb_t _reg_access_cb;
    void* _reg_access_cb_data;
};

}  // namespace m5

#endif  // _M5_TAB5_KEYBOARD_H_
