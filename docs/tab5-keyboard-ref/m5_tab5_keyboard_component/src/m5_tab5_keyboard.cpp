/*
 * SPDX-FileCopyrightText: 2025-2026 M5Stack
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_tab5_keyboard.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
#include "Arduino.h"
#else
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#endif

namespace {

m5_tab5_kb_log_level_t s_log_level = M5_TAB5_KB_LOG_LEVEL_INFO;

#ifndef ARDUINO
static const char* kTag              = "m5_tab5_keyboard";
static bool s_gpio_isr_service_ready = false;
#endif

static void log_message(m5_tab5_kb_log_level_t level, const char* fmt, ...)
{
    if (level == M5_TAB5_KB_LOG_LEVEL_NONE || level > s_log_level) {
        return;
    }

    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

#ifdef ARDUINO
    if (Serial) {
        Serial.println(buffer);
    }
#else
    switch (level) {
        case M5_TAB5_KB_LOG_LEVEL_ERROR:
            ESP_LOGE(kTag, "%s", buffer);
            break;
        case M5_TAB5_KB_LOG_LEVEL_WARN:
            ESP_LOGW(kTag, "%s", buffer);
            break;
        case M5_TAB5_KB_LOG_LEVEL_INFO:
            ESP_LOGI(kTag, "%s", buffer);
            break;
        case M5_TAB5_KB_LOG_LEVEL_DEBUG:
            ESP_LOGD(kTag, "%s", buffer);
            break;
        case M5_TAB5_KB_LOG_LEVEL_VERBOSE:
            ESP_LOGV(kTag, "%s", buffer);
            break;
        default:
            break;
    }
#endif
}

static const char* mode_to_string(m5_tab5_kb_mode_t mode)
{
    switch (mode) {
        case M5_TAB5_KB_MODE_NORMAL:
            return "Normal";
        case M5_TAB5_KB_MODE_HID:
            return "HID";
        case M5_TAB5_KB_MODE_STRING:
            return "String";
        case M5_TAB5_KB_MODE_BLE:
            return "BLE";
        default:
            return "Unknown";
    }
}

static const char* trigger_to_string(m5_tab5_kb_int_mode_t intMode)
{
    switch (intMode) {
        case M5_TAB5_KB_INT_MODE_HARDWARE:
            return "irq";
        case M5_TAB5_KB_INT_MODE_POLLING:
            return "poll";
        case M5_TAB5_KB_INT_MODE_DISABLED:
            return "off";
        default:
            return "unknown";
    }
}

static void log_key_event(const m5_tab5_key_event_t& event, m5_tab5_kb_int_mode_t intMode, uint8_t int_status,
                          uint8_t queue_len)
{
    const char* mode_str    = mode_to_string(event.type);
    const char* trigger_str = trigger_to_string(intMode);

    switch (event.type) {
        case M5_TAB5_KB_MODE_NORMAL:
            log_message(
                M5_TAB5_KB_LOG_LEVEL_INFO,
                "KB_EVT mode=%s(%d) trigger=%s int_sta=0x%02X queue=%u normal pressed=%u row=%u col=%u raw=0x%02X",
                mode_str, event.type, trigger_str, int_status, queue_len, event.pressed ? 1u : 0u, event.row, event.col,
                event.raw_data);
            break;
        case M5_TAB5_KB_MODE_HID:
            log_message(M5_TAB5_KB_LOG_LEVEL_INFO,
                        "KB_EVT mode=%s(%d) trigger=%s int_sta=0x%02X queue=%u hid mod=0x%02X key=0x%02X", mode_str,
                        event.type, trigger_str, int_status, queue_len, event.hid_modifier, event.hid_key_code);
            break;
        case M5_TAB5_KB_MODE_STRING:
            log_message(M5_TAB5_KB_LOG_LEVEL_INFO,
                        "KB_EVT mode=%s(%d) trigger=%s int_sta=0x%02X queue=%u str mod=0x%02X len=%u data=%s", mode_str,
                        event.type, trigger_str, int_status, queue_len, event.str_modifier, event.str_len,
                        event.str_data);
            break;
        case M5_TAB5_KB_MODE_BLE:
            log_message(M5_TAB5_KB_LOG_LEVEL_INFO,
                        "KB_EVT mode=%s(%d) trigger=%s int_sta=0x%02X queue=%u ble mod=0x%02X key=0x%02X", mode_str,
                        event.type, trigger_str, int_status, queue_len, event.hid_modifier, event.hid_key_code);
            break;
        default:
            log_message(M5_TAB5_KB_LOG_LEVEL_WARN, "KB_EVT mode=%s(%d) trigger=%s int_sta=0x%02X queue=%u unknown",
                        mode_str, event.type, trigger_str, int_status, queue_len);
            break;
    }
}

static void delay_ms(uint32_t ms)
{
#ifdef ARDUINO
    delay(ms);
#else
    vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

#ifndef ARDUINO
class KeyboardMutexGuard {
public:
    explicit KeyboardMutexGuard(SemaphoreHandle_t mutex) : _mutex(mutex), _locked(mutex == nullptr)
    {
        if (_mutex != nullptr) {
            _locked = (xSemaphoreTakeRecursive(_mutex, portMAX_DELAY) == pdTRUE);
        }
    }

    ~KeyboardMutexGuard()
    {
        if (_mutex != nullptr && _locked) {
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    bool locked() const
    {
        return _locked;
    }

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};
#endif

}  // namespace

m5::M5Tab5Keyboard::M5Tab5Keyboard()
    : _addr(M5_TAB5_KB_DEFAULT_ADDR),
      _initialized(false),
      _currentMode(M5_TAB5_KB_MODE_NORMAL),
      _requestedSpeed(M5_TAB5_KB_I2C_FREQ_DEFAULT),
      _intMode(M5_TAB5_KB_INT_MODE_POLLING),
      _intPin(-1),
      _pollingInterval(100),
      _callback(nullptr),
      _callbackArg(nullptr),
      _callbackArgData(nullptr),
      _keyCallback(nullptr),
      _keyCallbackArg(nullptr)
#ifdef ARDUINO
      ,
      _wire(nullptr),
      _sda(0xFF),
      _scl(0xFF)
#else
      ,
      _i2cDriverType(M5_TAB5_KB_I2C_DRIVER_NONE),
      _i2c_master_bus(nullptr),
      _i2c_master_dev(nullptr),
      _i2c_bus(nullptr),
      _i2c_device(nullptr),
      _busExternal(false),
      _sda(-1),
      _scl(-1),
      _port(I2C_NUM_0),
      _pollTask(nullptr),
      _intrQueue(nullptr),
      _opMutex(nullptr)
#endif
      ,
      _reg_access_cb(nullptr),
      _reg_access_cb_data(nullptr)
{
#ifndef ARDUINO
    _opMutex = xSemaphoreCreateRecursiveMutex();
#endif
}

m5::M5Tab5Keyboard::~M5Tab5Keyboard()
{
    end();
#ifndef ARDUINO
    if (_opMutex != nullptr) {
        vSemaphoreDelete(_opMutex);
        _opMutex = nullptr;
    }
#endif
}

void m5::M5Tab5Keyboard::setLogLevel(m5_tab5_kb_log_level_t level)
{
    s_log_level = level;
}

m5_tab5_kb_log_level_t m5::M5Tab5Keyboard::getLogLevel()
{
    return s_log_level;
}

bool m5::M5Tab5Keyboard::isInitialized() const
{
    return _initialized;
}

#ifdef ARDUINO

m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(TwoWire* wire, uint8_t addr, uint8_t sda, uint8_t scl, uint32_t speed,
                                           int8_t intPin, m5_tab5_kb_int_mode_t intMode)
{
    if (wire == nullptr) {
        return M5_TAB5_KB_ERR_INVALID_ARG;
    }

    _wire   = wire;
    _addr   = addr;
    _sda    = sda;
    _scl    = scl;
    _intPin = intPin;

    if (!_isValidI2cFrequency(speed)) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN, "Invalid I2C frequency: %lu Hz. Falling back to default.",
                    static_cast<unsigned long>(speed));
        _requestedSpeed = M5_TAB5_KB_I2C_FREQ_DEFAULT;
    } else {
        _requestedSpeed = speed;
    }

    if (_sda == static_cast<uint8_t>(-1) || _scl == static_cast<uint8_t>(-1)) {
        _wire->begin();
#ifdef ESP32
        _wire->setClock(_requestedSpeed);
#endif
    } else {
#ifdef ESP32
        _wire->begin(_sda, _scl, _requestedSpeed);
#else
        _wire->begin(_sda, _scl);
#endif
    }

    delay_ms(10);

    if (!_initDevice()) {
        return M5_TAB5_KB_ERR_I2C_COMM;
    }

    _initialized = true;

    if (intPin >= 0) {
        if (intMode == M5_TAB5_KB_INT_MODE_POLLING) {
            log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                        "Interrupt pin provided but Polling mode selected. Using Polling mode as requested.");
        }
    } else {
        if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
            log_message(
                M5_TAB5_KB_LOG_LEVEL_WARN,
                "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
            intMode = M5_TAB5_KB_INT_MODE_POLLING;
        }
    }

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        m5_tab5_kb_err_t err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    // Default to Normal Mode & Default Callback
    enableNormalMode();

    return M5_TAB5_KB_OK;
}

#else  // ESP-IDF

// Type 1A: Self-created I2C bus, no hardware interrupt
m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(i2c_port_t port, uint8_t addr, int sda, int scl, uint32_t speed,
                                           m5_tab5_kb_int_mode_t intMode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    end();

    _addr          = addr;
    _busExternal   = false;
    _i2cDriverType = M5_TAB5_KB_I2C_DRIVER_SELF_CREATED;
    _intPin        = -1;
    _port          = port;
    _sda           = sda;
    _scl           = scl;

    if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                    "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
        intMode = M5_TAB5_KB_INT_MODE_POLLING;
    }

    if (!_isValidI2cFrequency(speed)) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN, "Invalid I2C frequency: %lu Hz. Falling back to default.",
                    static_cast<unsigned long>(speed));
        _requestedSpeed = M5_TAB5_KB_I2C_FREQ_DEFAULT;
    } else {
        _requestedSpeed = speed;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port          = port,
        .sda_io_num        = static_cast<gpio_num_t>(sda),
        .scl_io_num        = static_cast<gpio_num_t>(scl),
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        .flags =
            {
                .enable_internal_pullup = true,
                .allow_pd               = false,
            },
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &_i2c_master_bus);
    if (ret != ESP_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_ERROR, "Failed to create I2C master bus.");
        return M5_TAB5_KB_ERR_I2C_CONFIG;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = _addr,
        .scl_speed_hz    = _requestedSpeed,
        .scl_wait_us     = 0,
        .flags =
            {
                .disable_ack_check = false,
            },
    };

    ret = i2c_master_bus_add_device(_i2c_master_bus, &dev_config, &_i2c_master_dev);
    if (ret != ESP_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_ERROR, "Failed to add I2C device.");
        i2c_del_master_bus(_i2c_master_bus);
        _i2c_master_bus = nullptr;
        return M5_TAB5_KB_ERR_I2C_CONFIG;
    }

    if (!_initDevice()) {
        end();
        return M5_TAB5_KB_ERR_I2C_COMM;
    }

    _initialized = true;

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        m5_tab5_kb_err_t err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    // Default to Normal Mode & Default Callback
    enableNormalMode();

    return M5_TAB5_KB_OK;
}

// Type 1B: Self-created I2C bus, with hardware interrupt
m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(i2c_port_t port, uint8_t addr, int sda, int scl, uint32_t speed, int intPin,
                                           m5_tab5_kb_int_mode_t intMode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    m5_tab5_kb_err_t err = begin(port, addr, sda, scl, speed, M5_TAB5_KB_INT_MODE_DISABLED);
    if (err != M5_TAB5_KB_OK) {
        return err;
    }

    _intPin = intPin;
    if (_intPin >= 0) {
        if (intMode == M5_TAB5_KB_INT_MODE_POLLING) {
            log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                        "Interrupt pin provided but Polling mode selected. Using Polling mode as requested.");
        }
    } else {
        if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
            log_message(
                M5_TAB5_KB_LOG_LEVEL_WARN,
                "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
            intMode = M5_TAB5_KB_INT_MODE_POLLING;
        }
    }

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    // Default to Normal Mode & Default Callback
    // Removed to avoid duplication (Called by Type 1A begin already)
    // enableNormalMode();

    return M5_TAB5_KB_OK;
}

// Type 2A: Existing i2c_master_bus_handle_t, no hardware interrupt
m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t speed,
                                           m5_tab5_kb_int_mode_t intMode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    end();

    _addr           = addr;
    _busExternal    = true;
    _i2cDriverType  = M5_TAB5_KB_I2C_DRIVER_MASTER;
    _intPin         = -1;
    _i2c_master_bus = bus;
    _sda            = -1;
    _scl            = -1;

    if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                    "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
        intMode = M5_TAB5_KB_INT_MODE_POLLING;
    }

    if (!_isValidI2cFrequency(speed)) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN, "Invalid I2C frequency: %lu Hz. Falling back to default.",
                    static_cast<unsigned long>(speed));
        _requestedSpeed = M5_TAB5_KB_I2C_FREQ_DEFAULT;
    } else {
        _requestedSpeed = speed;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = _addr,
        .scl_speed_hz    = _requestedSpeed,
        .scl_wait_us     = 0,
        .flags =
            {
                .disable_ack_check = false,
            },
    };

    esp_err_t ret = i2c_master_bus_add_device(_i2c_master_bus, &dev_config, &_i2c_master_dev);
    if (ret != ESP_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_ERROR, "Failed to add I2C device.");
        return M5_TAB5_KB_ERR_I2C_CONFIG;
    }

    if (!_initDevice()) {
        end();
        return M5_TAB5_KB_ERR_I2C_COMM;
    }

    _initialized = true;

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        m5_tab5_kb_err_t err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    // Default to Normal Mode & Default Callback
    enableNormalMode();

    return M5_TAB5_KB_OK;
}

// Type 2B: Existing i2c_master_bus_handle_t, with hardware interrupt
m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t speed, int intPin,
                                           m5_tab5_kb_int_mode_t intMode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    m5_tab5_kb_err_t err = begin(bus, addr, speed, M5_TAB5_KB_INT_MODE_DISABLED);
    if (err != M5_TAB5_KB_OK) {
        return err;
    }

    _intPin = intPin;
    if (_intPin >= 0) {
        if (intMode == M5_TAB5_KB_INT_MODE_POLLING) {
            log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                        "Interrupt pin provided but Polling mode selected. Using Polling mode as requested.");
        }
    } else {
        if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
            log_message(
                M5_TAB5_KB_LOG_LEVEL_WARN,
                "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
            intMode = M5_TAB5_KB_INT_MODE_POLLING;
        }
    }

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    return M5_TAB5_KB_OK;
}

// Type 3A: Existing i2c_bus_handle_t, no hardware interrupt
m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(i2c_bus_handle_t bus, uint8_t addr, uint32_t speed,
                                           m5_tab5_kb_int_mode_t intMode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    end();

    _addr          = addr;
    _busExternal   = true;
    _i2cDriverType = M5_TAB5_KB_I2C_DRIVER_BUS;
    _intPin        = -1;
    _i2c_bus       = bus;
    _sda           = -1;
    _scl           = -1;

    if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                    "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
        intMode = M5_TAB5_KB_INT_MODE_POLLING;
    }

    if (!_isValidI2cFrequency(speed)) {
        log_message(M5_TAB5_KB_LOG_LEVEL_WARN, "Invalid I2C frequency: %lu Hz. Falling back to default.",
                    static_cast<unsigned long>(speed));
        _requestedSpeed = M5_TAB5_KB_I2C_FREQ_DEFAULT;
    } else {
        _requestedSpeed = speed;
    }

    _i2c_device = i2c_bus_device_create(_i2c_bus, _addr, _requestedSpeed);
    if (_i2c_device == nullptr) {
        log_message(M5_TAB5_KB_LOG_LEVEL_ERROR, "Failed to create I2C device.");
        return M5_TAB5_KB_ERR_I2C_CONFIG;
    }

    if (!_initDevice()) {
        end();
        return M5_TAB5_KB_ERR_I2C_COMM;
    }

    _initialized = true;

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        m5_tab5_kb_err_t err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    // Default to Normal Mode & Default Callback
    enableNormalMode();

    return M5_TAB5_KB_OK;
}

// Type 3B: Existing i2c_bus_handle_t, with hardware interrupt
m5_tab5_kb_err_t m5::M5Tab5Keyboard::begin(i2c_bus_handle_t bus, uint8_t addr, uint32_t speed, int intPin,
                                           m5_tab5_kb_int_mode_t intMode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    m5_tab5_kb_err_t err = begin(bus, addr, speed, M5_TAB5_KB_INT_MODE_DISABLED);
    if (err != M5_TAB5_KB_OK) {
        return err;
    }

    _intPin = intPin;
    if (_intPin >= 0) {
        if (intMode == M5_TAB5_KB_INT_MODE_POLLING) {
            log_message(M5_TAB5_KB_LOG_LEVEL_WARN,
                        "Interrupt pin provided but Polling mode selected. Using Polling mode as requested.");
        }
    } else {
        if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
            log_message(
                M5_TAB5_KB_LOG_LEVEL_WARN,
                "Hardware interrupt mode selected but no interrupt pin provided. Falling back to Polling mode.");
            intMode = M5_TAB5_KB_INT_MODE_POLLING;
        }
    }

    if (intMode != M5_TAB5_KB_INT_MODE_DISABLED) {
        err = setInterruptMode(intMode, _pollingInterval);
        if (err != M5_TAB5_KB_OK) {
            end();
            return err;
        }
    }

    // Default to Normal Mode & Default Callback
    // enableNormalMode();

    return M5_TAB5_KB_OK;
}

#endif  // ARDUINO

void m5::M5Tab5Keyboard::end()
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return;
    }
#endif
    // 清除寄存器访问拦截回?
    clearRegAccessCallback();

#ifdef ARDUINO
    if (_intMode == M5_TAB5_KB_INT_MODE_POLLING) {
        _cleanupPollingArduino();
    } else if (_intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        _cleanupHardwareInterruptArduino();
    }

    if (_wire != nullptr) {
        _wire->end();
    }
#else
    if (_intMode == M5_TAB5_KB_INT_MODE_POLLING) {
        _cleanupPolling();
    } else if (_intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        _cleanupHardwareInterrupt();
    }

    switch (_i2cDriverType) {
        case M5_TAB5_KB_I2C_DRIVER_SELF_CREATED:
        case M5_TAB5_KB_I2C_DRIVER_MASTER:
            if (_i2c_master_dev != nullptr) {
                i2c_master_bus_rm_device(_i2c_master_dev);
                _i2c_master_dev = nullptr;
            }
            if (!_busExternal && _i2c_master_bus != nullptr) {
                i2c_del_master_bus(_i2c_master_bus);
                _i2c_master_bus = nullptr;
            }
            break;
        case M5_TAB5_KB_I2C_DRIVER_BUS:
            if (_i2c_device != nullptr) {
                i2c_bus_device_delete(&_i2c_device);
                _i2c_device = nullptr;
            }
            if (!_busExternal && _i2c_bus != nullptr) {
                i2c_bus_delete(&_i2c_bus);
                _i2c_bus = nullptr;
            }
            break;
        default:
            break;
    }

        // 注意：_i2cDriverType 故意不重置。它代表配置意图（由 begin() 设置），
        // ?begin() ?_initDevice() 失败而调?end() 时仍需保留?
        // 以便正确重建临时总线
        // Note: _i2cDriverType is intentionally NOT reset here. It represents the
        // configuration intent set by begin(), and must survive an end() call triggered
        // by _initDevice() failure so recovery can reconstruct the bus.
#endif

    _initialized = false;
    _intMode     = M5_TAB5_KB_INT_MODE_DISABLED;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getVersion(uint8_t* version)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    if (version == nullptr) {
        return M5_TAB5_KB_ERR_INVALID_ARG;
    }
    if (!_initialized) {
        return M5_TAB5_KB_ERR_NOT_INIT;
    }

    return _readReg(M5_TAB5_KB_REG_VERSION, version) ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getRGB(uint8_t index, uint8_t* r, uint8_t* g, uint8_t* b)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (index > 1) return M5_TAB5_KB_ERR_INVALID_ARG;

    uint8_t buf[3];
    uint8_t reg_addr = M5_TAB5_KB_REG_RGB_COLOR_BASE + (index * 4);
    if (_readBytes(reg_addr, buf, 3)) {
        if (b) *b = buf[0];
        if (g) *g = buf[1];
        if (r) *r = buf[2];
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getBothRGB(uint8_t* r1, uint8_t* g1, uint8_t* b1, uint8_t* r2, uint8_t* g2,
                                                uint8_t* b2)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;

    uint8_t buf[7];
    if (_readBytes(M5_TAB5_KB_REG_RGB_COLOR_BASE, buf, 7)) {
        if (b1) *b1 = buf[0];
        if (g1) *g1 = buf[1];
        if (r1) *r1 = buf[2];

        if (b2) *b2 = buf[4];
        if (g2) *g2 = buf[5];
        if (r2) *r2 = buf[6];

        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setRGB(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (index > 1) return M5_TAB5_KB_ERR_INVALID_ARG;

    // RGB data format: B, G, R
    uint8_t buf[3]   = {b, g, r};
    uint8_t reg_addr = M5_TAB5_KB_REG_RGB_COLOR_BASE + (index * 3);

    if (_writeBytes(reg_addr, buf, 3)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setBothRGB(uint8_t r, uint8_t g, uint8_t b)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;

    // RGB 7-byte window with reserved gap at offset 3:
    // [RGB1_B, RGB1_G, RGB1_R, Reserved(0), RGB2_B, RGB2_G, RGB2_R]
    uint8_t buf[7] = {b, g, r, 0x00, b, g, r};
    if (_writeBytes(M5_TAB5_KB_REG_RGB_COLOR_BASE, buf, 7)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getRGBMode(m5_tab5_kb_rgb_mode_t* mode)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    uint8_t val;
    if (_readReg(M5_TAB5_KB_REG_RGB_MODE, &val)) {
        *mode = (m5_tab5_kb_rgb_mode_t)val;
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setRGBMode(uint8_t mode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (mode > 1) return M5_TAB5_KB_ERR_INVALID_ARG;

    if (_writeReg(M5_TAB5_KB_REG_RGB_MODE, mode)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setBrightness(uint8_t brightness)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (brightness > 100) return M5_TAB5_KB_ERR_INVALID_ARG;

    if (_writeReg(M5_TAB5_KB_REG_BRIGHTNESS, brightness)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getBrightness(uint8_t* brightness)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (brightness == nullptr) return M5_TAB5_KB_ERR_INVALID_ARG;

    if (_readReg(M5_TAB5_KB_REG_BRIGHTNESS, brightness)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setI2CAddress(uint8_t newAddr)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (newAddr < 0x08 || newAddr > 0x77) return M5_TAB5_KB_ERR_INVALID_ARG;

    if (_writeReg(M5_TAB5_KB_REG_I2C_ADDR, newAddr)) {
        _addr = newAddr;  // Update internal address
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getI2CAddress(uint8_t* addr)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (addr == nullptr) return M5_TAB5_KB_ERR_INVALID_ARG;

    if (_readReg(M5_TAB5_KB_REG_I2C_ADDR, addr)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setInterruptMode(m5_tab5_kb_int_mode_t intMode, uint32_t pollingIntervalMs)
{
    if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE && _intPin < 0) {
        return M5_TAB5_KB_ERR_INVALID_ARG;
    }

    if (pollingIntervalMs == 0) {
        pollingIntervalMs = 1;
    }

    if (_intMode == M5_TAB5_KB_INT_MODE_POLLING) {
#ifdef ARDUINO
        _cleanupPollingArduino();
#else
        _cleanupPolling();
#endif
    } else if (_intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
#ifdef ARDUINO
        _cleanupHardwareInterruptArduino();
#else
        _cleanupHardwareInterrupt();
#endif
    }

    _intMode         = intMode;
    _pollingInterval = pollingIntervalMs;

    if (intMode == M5_TAB5_KB_INT_MODE_DISABLED) {
        return M5_TAB5_KB_OK;
    }

#ifdef ARDUINO
    if (intMode == M5_TAB5_KB_INT_MODE_POLLING) {
        return _setupPollingArduino() ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_INTERNAL;
    }
    if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        return _setupHardwareInterruptArduino() ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_INTERNAL;
    }
#else
    if (intMode == M5_TAB5_KB_INT_MODE_POLLING) {
        return _setupPolling() ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_INTERNAL;
    }
    if (intMode == M5_TAB5_KB_INT_MODE_HARDWARE) {
        return _setupHardwareInterrupt() ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_INTERNAL;
    }
#endif

    return M5_TAB5_KB_ERR_NOT_SUPPORTED;
}

void m5::M5Tab5Keyboard::attachInterrupt(m5_tab5_kb_callback_t callback)
{
    _callback        = callback;
    _callbackArg     = nullptr;
    _callbackArgData = nullptr;
}

void m5::M5Tab5Keyboard::attachInterruptArg(m5_tab5_kb_callback_arg_t callback, void* arg)
{
    _callbackArg     = callback;
    _callbackArgData = arg;
    _callback        = nullptr;
}

// ========================
// 公开的原始寄存器访问
// Public raw register access
// ========================
m5_tab5_kb_err_t m5::M5Tab5Keyboard::readRegByte(uint8_t reg, uint8_t* value)
{
    if (!value) return M5_TAB5_KB_ERR_INVALID_ARG;
    return _readReg(reg, value) ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::writeRegByte(uint8_t reg, uint8_t value)
{
    return _writeReg(reg, value) ? M5_TAB5_KB_OK : M5_TAB5_KB_ERR_I2C_COMM;
}

// ============================================================================
// 寄存器访问拦截回?(Register Access Intercept Callback)
// ============================================================================

void m5::M5Tab5Keyboard::setRegAccessCallback(m5_tab5_reg_access_cb_t cb, void* user_data)
{
    _reg_access_cb      = cb;
    _reg_access_cb_data = user_data;
}

void m5::M5Tab5Keyboard::clearRegAccessCallback()
{
    _reg_access_cb      = nullptr;
    _reg_access_cb_data = nullptr;
}

bool m5::M5Tab5Keyboard::_writeReg(uint8_t reg, uint8_t value)
{
#ifdef ARDUINO
    if (_wire == nullptr) {
        return false;
    }
    for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
        if (TAB5_KB_I2C_WRITE_BYTE(_wire, _addr, reg, value)) {
            if (_reg_access_cb) _reg_access_cb(reg, value, M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
            return true;
        }
        delay_ms(2);
    }
    return false;
#else
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_MASTER || _i2cDriverType == M5_TAB5_KB_I2C_DRIVER_SELF_CREATED) {
        if (_i2c_master_dev == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_MASTER_WRITE_BYTE(_i2c_master_dev, reg, value) == ESP_OK) {
                if (_reg_access_cb) _reg_access_cb(reg, value, M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                return true;
            }
            delay_ms(2);
        }
        return false;
    }
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_BUS) {
        if (_i2c_device == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_WRITE_BYTE(_i2c_device, reg, value) == ESP_OK) {
                if (_reg_access_cb) _reg_access_cb(reg, value, M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                return true;
            }
            delay_ms(2);
        }
    }
    return false;
#endif
}

bool m5::M5Tab5Keyboard::_writeReg16(uint8_t reg, uint16_t value)
{
#ifdef ARDUINO
    if (_wire == nullptr) {
        return false;
    }
    for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
        if (TAB5_KB_I2C_WRITE_REG16(_wire, _addr, reg, value)) {
            if (_reg_access_cb) {
                _reg_access_cb(reg, (uint8_t)(value & 0xFF), M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                _reg_access_cb(reg + 1, (uint8_t)((value >> 8) & 0xFF), M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
            }
            return true;
        }
        delay_ms(2);
    }
    return false;
#else
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_MASTER || _i2cDriverType == M5_TAB5_KB_I2C_DRIVER_SELF_CREATED) {
        if (_i2c_master_dev == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_MASTER_WRITE_REG16(_i2c_master_dev, reg, value) == ESP_OK) {
                if (_reg_access_cb) {
                    _reg_access_cb(reg, (uint8_t)(value & 0xFF), M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                    _reg_access_cb(reg + 1, (uint8_t)((value >> 8) & 0xFF), M5_TAB5_REG_ACCESS_WRITE,
                                   _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
        return false;
    }
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_BUS) {
        if (_i2c_device == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_WRITE_REG16(_i2c_device, reg, value) == ESP_OK) {
                if (_reg_access_cb) {
                    _reg_access_cb(reg, (uint8_t)(value & 0xFF), M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                    _reg_access_cb(reg + 1, (uint8_t)((value >> 8) & 0xFF), M5_TAB5_REG_ACCESS_WRITE,
                                   _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
    }
    return false;
#endif
}

bool m5::M5Tab5Keyboard::_readReg(uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return false;
    }
#ifdef ARDUINO
    if (_wire == nullptr) {
        return false;
    }
    for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
        if (TAB5_KB_I2C_READ_BYTE(_wire, _addr, reg, value)) {
            if (_reg_access_cb) _reg_access_cb(reg, *value, M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
            return true;
        }
        delay_ms(2);
    }
    return false;
#else
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_MASTER || _i2cDriverType == M5_TAB5_KB_I2C_DRIVER_SELF_CREATED) {
        if (_i2c_master_dev == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_MASTER_READ_BYTE(_i2c_master_dev, reg, value) == ESP_OK) {
                if (_reg_access_cb) _reg_access_cb(reg, *value, M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                return true;
            }
            delay_ms(2);
        }
        return false;
    }
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_BUS) {
        if (_i2c_device == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_READ_BYTE(_i2c_device, reg, value) == ESP_OK) {
                if (_reg_access_cb) _reg_access_cb(reg, *value, M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                return true;
            }
            delay_ms(2);
        }
    }
    return false;
#endif
}

bool m5::M5Tab5Keyboard::_readReg16(uint8_t reg, uint16_t* value)
{
    if (value == nullptr) {
        return false;
    }
#ifdef ARDUINO
    if (_wire == nullptr) {
        return false;
    }
    for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
        if (TAB5_KB_I2C_READ_REG16(_wire, _addr, reg, value)) {
            if (_reg_access_cb) {
                _reg_access_cb(reg, (uint8_t)(*value & 0xFF), M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                _reg_access_cb(reg + 1, (uint8_t)((*value >> 8) & 0xFF), M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
            }
            return true;
        }
        delay_ms(2);
    }
    return false;
#else
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_MASTER || _i2cDriverType == M5_TAB5_KB_I2C_DRIVER_SELF_CREATED) {
        if (_i2c_master_dev == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_MASTER_READ_REG16(_i2c_master_dev, reg, value) == ESP_OK) {
                if (_reg_access_cb) {
                    _reg_access_cb(reg, (uint8_t)(*value & 0xFF), M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                    _reg_access_cb(reg + 1, (uint8_t)((*value >> 8) & 0xFF), M5_TAB5_REG_ACCESS_READ,
                                   _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
        return false;
    }
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_BUS) {
        if (_i2c_device == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_READ_REG16(_i2c_device, reg, value) == ESP_OK) {
                if (_reg_access_cb) {
                    _reg_access_cb(reg, (uint8_t)(*value & 0xFF), M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                    _reg_access_cb(reg + 1, (uint8_t)((*value >> 8) & 0xFF), M5_TAB5_REG_ACCESS_READ,
                                   _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
    }
    return false;
#endif
}

bool m5::M5Tab5Keyboard::_writeBytes(uint8_t reg, const uint8_t* data, uint8_t len)
{
    if (data == nullptr || len == 0) {
        return false;
    }
#ifdef ARDUINO
    if (_wire == nullptr) {
        return false;
    }
    for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
        if (TAB5_KB_I2C_WRITE_BYTES(_wire, _addr, reg, len, data)) {
            if (_reg_access_cb) {
                for (uint8_t _i = 0; _i < len; _i++)
                    _reg_access_cb(reg + _i, data[_i], M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
            }
            return true;
        }
        delay_ms(2);
    }
    return false;
#else
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_MASTER || _i2cDriverType == M5_TAB5_KB_I2C_DRIVER_SELF_CREATED) {
        if (_i2c_master_dev == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_MASTER_WRITE_BYTES(_i2c_master_dev, reg, len, data) == ESP_OK) {
                if (_reg_access_cb) {
                    for (uint8_t _i = 0; _i < len; _i++)
                        _reg_access_cb(reg + _i, data[_i], M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
        return false;
    }
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_BUS) {
        if (_i2c_device == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_WRITE_BYTES(_i2c_device, reg, len, data) == ESP_OK) {
                if (_reg_access_cb) {
                    for (uint8_t _i = 0; _i < len; _i++)
                        _reg_access_cb(reg + _i, data[_i], M5_TAB5_REG_ACCESS_WRITE, _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
    }
    return false;
#endif
}

bool m5::M5Tab5Keyboard::_readBytes(uint8_t reg, uint8_t* data, uint8_t len)
{
    if (data == nullptr || len == 0) {
        return false;
    }
#ifdef ARDUINO
    if (_wire == nullptr) {
        return false;
    }
    for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
        if (TAB5_KB_I2C_READ_BYTES(_wire, _addr, reg, len, data)) {
            if (_reg_access_cb) {
                for (uint8_t _i = 0; _i < len; _i++)
                    _reg_access_cb(reg + _i, data[_i], M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
            }
            return true;
        }
        delay_ms(2);
    }
    return false;
#else
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_MASTER || _i2cDriverType == M5_TAB5_KB_I2C_DRIVER_SELF_CREATED) {
        if (_i2c_master_dev == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_MASTER_READ_BYTES(_i2c_master_dev, reg, len, data) == ESP_OK) {
                if (_reg_access_cb) {
                    for (uint8_t _i = 0; _i < len; _i++)
                        _reg_access_cb(reg + _i, data[_i], M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
        return false;
    }
    if (_i2cDriverType == M5_TAB5_KB_I2C_DRIVER_BUS) {
        if (_i2c_device == nullptr) {
            return false;
        }
        for (int i = 0; i < M5_TAB5_KB_I2C_RETRY_COUNT; ++i) {
            if (TAB5_KB_I2C_READ_BYTES(_i2c_device, reg, len, data) == ESP_OK) {
                if (_reg_access_cb) {
                    for (uint8_t _i = 0; _i < len; _i++)
                        _reg_access_cb(reg + _i, data[_i], M5_TAB5_REG_ACCESS_READ, _reg_access_cb_data);
                }
                return true;
            }
            delay_ms(2);
        }
    }
    return false;
#endif
}

bool m5::M5Tab5Keyboard::_initDevice()
{
    uint8_t val = 0;
    // Try to read INT_CFG register to verify connection
    if (!_readReg(M5_TAB5_KB_REG_INT_CFG, &val)) {
        log_message(M5_TAB5_KB_LOG_LEVEL_ERROR, "Failed to read device INT_CFG.");
        return false;
    }
    log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "Device INT_CFG: 0x%02X", val);
    return true;
}

void m5::M5Tab5Keyboard::detachInterrupt()
{
    _cleanupHardwareInterrupt();
    _cleanupPolling();  // Also stop polling if active
    _intMode     = M5_TAB5_KB_INT_MODE_DISABLED;
    _callback    = nullptr;
    _callbackArg = nullptr;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getInterruptStatus(uint8_t* status)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_readReg(M5_TAB5_KB_REG_INT_STA, status)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::clearInterruptStatus()
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    uint8_t data = 0;
    if (_writeBytes(M5_TAB5_KB_REG_INT_STA, &data, 1)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setInterruptConfig(uint8_t config)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_writeBytes(M5_TAB5_KB_REG_INT_CFG, &config, 1)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getInterruptConfig(uint8_t* config)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_readReg(M5_TAB5_KB_REG_INT_CFG, config)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setKeyboardMode(uint8_t mode)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_writeBytes(M5_TAB5_KB_REG_KEYBOARD_MODE, &mode, 1)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getKeyboardMode(uint8_t* mode)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_readReg(M5_TAB5_KB_REG_KEYBOARD_MODE, mode)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getKeyEvent(uint8_t* event)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_readReg(M5_TAB5_KB_REG_KEY_EVENT, event)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::getEventCount(uint8_t* count)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    if (_readReg(M5_TAB5_KB_REG_EVENT_NUM, count)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::clearEventQueue()
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;
    uint8_t val = 0;
    if (_writeBytes(M5_TAB5_KB_REG_EVENT_NUM, &val, 1)) {
        return M5_TAB5_KB_OK;
    }
    return M5_TAB5_KB_ERR_I2C_COMM;
}

void m5::M5Tab5Keyboard::_handleInterrupt()
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return;
    }
#endif
    if (_callbackArg != nullptr) {
        _callbackArg(_callbackArgData);
    } else if (_callback != nullptr) {
        _callback();
    } else if (_keyCallback != nullptr) {
        // Internal handling loop based on Mode
        uint8_t status = 0;
        if (getInterruptStatus(&status) == M5_TAB5_KB_OK) {
            bool hasEvent = false;
            if (_currentMode == M5_TAB5_KB_MODE_NORMAL && (status & 0x01))
                hasEvent = true;
            else if (_currentMode == M5_TAB5_KB_MODE_HID && (status & 0x02))
                hasEvent = true;
            else if (_currentMode == M5_TAB5_KB_MODE_STRING && (status & 0x04))
                hasEvent = true;

            if (hasEvent) {
                uint8_t count = 0;
                if (getEventCount(&count) == M5_TAB5_KB_OK) {
                    while (count > 0) {
                        m5_tab5_key_event_t keyEvt;
                        memset(&keyEvt, 0, sizeof(keyEvt));
                        keyEvt.type = _currentMode;
                        bool valid  = false;

                        if (_currentMode == M5_TAB5_KB_MODE_NORMAL) {
                            uint8_t event = 0;
                            if (getKeyEvent(&event) == M5_TAB5_KB_OK && event != 0xFF) {
                                keyEvt.pressed  = (event & 0x80) != 0;
                                keyEvt.row      = (event >> 4) & 0x07;
                                keyEvt.col      = event & 0x0F;
                                keyEvt.raw_data = event;
                                valid           = true;
                            }
                        } else if (_currentMode == M5_TAB5_KB_MODE_HID) {
                            uint8_t buf[2];
                            if (_readBytes(M5_TAB5_KB_REG_HID_EVENT, buf, 2)) {
                                if (buf[0] != 0xFF || buf[1] != 0xFF) {
                                    keyEvt.hid_modifier = buf[0];
                                    keyEvt.hid_key_code = buf[1];
                                    valid               = true;
                                }
                            }
                        } else if (_currentMode == M5_TAB5_KB_MODE_STRING) {
                            uint8_t len = 0;
                            if (_readBytes(M5_TAB5_KB_REG_CHAR_EVENT_LEN, &len, 1) && len > 0) {
                                if (len > 15) {
                                    log_message(M5_TAB5_KB_LOG_LEVEL_WARN, "String event length out of range: %u", len);
                                } else {
                                    uint8_t buf[16];
                                    if (_readBytes(M5_TAB5_KB_REG_CHAR_EVENT_BASE, buf, len + 1)) {
                                        keyEvt.str_modifier = buf[0];
                                        keyEvt.str_len      = len;
                                        int copyLen         = len;
                                        if (copyLen > 15) copyLen = 15;
                                        memcpy(keyEvt.str_data, &buf[1], copyLen);
                                        keyEvt.str_data[copyLen] = 0;
                                        valid                    = true;
                                    }
                                }
                            }
                        }

                        if (valid) {
                            uint8_t queue_len = count;
                            log_key_event(keyEvt, _intMode, status, queue_len);
                            _keyCallback(keyEvt, _keyCallbackArg);
                        }
                        count--;
                        if (count > 32) break;  // Safety break
                    }
                }
            }
            clearInterruptStatus();
        }
    }
}

// Default Key Event Callback
// 默认按键事件回调
static void _default_key_callback(m5_tab5_key_event_t event, void* arg)
{
    (void)event;
    (void)arg;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::setMode(m5_tab5_kb_mode_t mode)
{
#ifndef ARDUINO
    KeyboardMutexGuard guard(_opMutex);
    if (!guard.locked()) {
        return M5_TAB5_KB_ERR_INTERNAL;
    }
#endif
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;

    // Set hardware mode
    m5_tab5_kb_err_t err = setKeyboardMode((uint8_t)mode);
    if (err != M5_TAB5_KB_OK) return err;

    // Update internal state
    _currentMode = mode;

    // Clear queues as mode switch resets device state
    uint8_t dummy;
    getInterruptStatus(&dummy);
    clearEventQueue();
    clearInterruptStatus();

    log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "Keyboard Mode Set to: %d", mode);
    return M5_TAB5_KB_OK;
}

void m5::M5Tab5Keyboard::setKeyCallback(m5_tab5_key_callback_t callback, void* arg)
{
    if (callback == nullptr) {
        _keyCallback    = _default_key_callback;
        _keyCallbackArg = nullptr;
    } else {
        _keyCallback    = callback;
        _keyCallbackArg = arg;
    }
}

m5_tab5_kb_mode_t m5::M5Tab5Keyboard::getMode() const
{
    return _currentMode;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::enableNormalMode(m5_tab5_key_callback_t callback, void* arg)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;

    if (callback == nullptr) {
        _keyCallback    = _default_key_callback;
        _keyCallbackArg = nullptr;
    } else {
        _keyCallback    = callback;
        _keyCallbackArg = arg;
    }

    // 1. Set Normal Mode using new API
    m5_tab5_kb_err_t err = setMode(M5_TAB5_KB_MODE_NORMAL);
    if (err != M5_TAB5_KB_OK) return err;

    log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "Normal Mode Enabled. Handlers Registered.");

    // Print Device Info
    uint8_t ver = 0;
    if (getVersion(&ver) == M5_TAB5_KB_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "Firmware Version: 0x%02X", ver);
    }

    // Print Mode
    const char* modeStr = "Unknown";
    switch (_currentMode) {
        case M5_TAB5_KB_MODE_NORMAL:
            modeStr = "Normal";
            break;
        case M5_TAB5_KB_MODE_HID:
            modeStr = "HID";
            break;
        case M5_TAB5_KB_MODE_STRING:
            modeStr = "String";
            break;
        case M5_TAB5_KB_MODE_BLE:
            modeStr = "BLE";
            break;
    }
    log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "Current Mode: %s (%d)", modeStr, _currentMode);

    m5_tab5_kb_rgb_mode_t rgbMode;
    if (getRGBMode(&rgbMode) == M5_TAB5_KB_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "RGB Mode: %s (%d)",
                    rgbMode == M5_TAB5_KB_RGB_MODE_BINDING ? "Binding" : "Custom", rgbMode);
    }

    // Print RGB Info
    uint8_t r, g, b;
    if (getRGB(0, &r, &g, &b) == M5_TAB5_KB_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "RGB1 Color: R=%d, G=%d, B=%d", r, g, b);
    }
    if (getRGB(1, &r, &g, &b) == M5_TAB5_KB_OK) {
        log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "RGB2 Color: R=%d, G=%d, B=%d", r, g, b);
    }

    return M5_TAB5_KB_OK;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::enableHIDMode(m5_tab5_key_callback_t callback, void* arg)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;

    if (callback == nullptr) {
        _keyCallback    = _default_key_callback;
        _keyCallbackArg = nullptr;
    } else {
        _keyCallback    = callback;
        _keyCallbackArg = arg;
    }

    // 1. Set HID Mode
    m5_tab5_kb_err_t err = setMode(M5_TAB5_KB_MODE_HID);
    if (err != M5_TAB5_KB_OK) return err;

    log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "HID Mode Enabled. Handlers Registered.");
    return M5_TAB5_KB_OK;
}

m5_tab5_kb_err_t m5::M5Tab5Keyboard::enableStringMode(m5_tab5_key_callback_t callback, void* arg)
{
    if (!_initialized) return M5_TAB5_KB_ERR_NOT_INIT;

    if (callback == nullptr) {
        _keyCallback    = _default_key_callback;
        _keyCallbackArg = nullptr;
    } else {
        _keyCallback    = callback;
        _keyCallbackArg = arg;
    }

    // 1. Set String Mode
    m5_tab5_kb_err_t err = setMode(M5_TAB5_KB_MODE_STRING);
    if (err != M5_TAB5_KB_OK) return err;

    log_message(M5_TAB5_KB_LOG_LEVEL_INFO, "String Mode Enabled. Handlers Registered.");
    return M5_TAB5_KB_OK;
}

bool m5::M5Tab5Keyboard::_isValidI2cFrequency(uint32_t speed)
{
    return (speed == M5_TAB5_KB_I2C_FREQ_100K || speed == M5_TAB5_KB_I2C_FREQ_400K);
}

#ifdef ARDUINO

bool m5::M5Tab5Keyboard::_setupPollingArduino()
{
    // TODO: Provide a timer-based polling implementation if needed.
    return true;
}

void m5::M5Tab5Keyboard::_cleanupPollingArduino()
{
}

void m5::M5Tab5Keyboard::_pollTaskArduino(void* arg)
{
    (void)arg;
}

bool m5::M5Tab5Keyboard::_setupHardwareInterruptArduino()
{
    // TODO: Provide Arduino interrupt binding if needed.
    return true;
}

void m5::M5Tab5Keyboard::_cleanupHardwareInterruptArduino()
{
}

#else  // ESP-IDF

void m5::M5Tab5Keyboard::_pollTaskFunc(void* arg)
{
    auto* self = static_cast<M5Tab5Keyboard*>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        if (self->_intMode == M5_TAB5_KB_INT_MODE_HARDWARE && self->_intrQueue != nullptr) {
            uint32_t gpio_num = 0;
            if (xQueueReceive(self->_intrQueue, &gpio_num, portMAX_DELAY) == pdTRUE) {
                // Coalesce IRQ bursts. The device event queue already stores the actual key events,
                // so multiple GPIO edges only need one pass through the handler.
                while (xQueueReceive(self->_intrQueue, &gpio_num, 0) == pdTRUE) {
                }
                self->_handleInterrupt();
                // Always yield after one IRQ pass so a burst of plug/unplug edges cannot starve IDLE0.
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else if (self->_intMode == M5_TAB5_KB_INT_MODE_POLLING) {
            uint8_t status = 0;
            // Check status register (0x01) for any active flags (bits 0, 1, 2)
            // 检查状态寄存器是否有标志位被置?
            if (self->_readReg(M5_TAB5_KB_REG_INT_STA, &status)) {
                if (status & 0x07) {
                    self->_handleInterrupt();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(self->_pollingInterval));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void IRAM_ATTR m5::M5Tab5Keyboard::_isrHandler(void* arg)
{
    auto* self = static_cast<M5Tab5Keyboard*>(arg);
    if (self == nullptr || self->_intrQueue == nullptr) {
        return;
    }

    uint32_t gpio_num                   = static_cast<uint32_t>(self->_intPin);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(self->_intrQueue, &gpio_num, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

bool m5::M5Tab5Keyboard::_setupHardwareInterrupt()
{
    if (_intPin < 0) {
        return false;
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type     = GPIO_INTR_NEGEDGE;
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask  = 1ULL << _intPin;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;

    if (gpio_config(&io_conf) != ESP_OK) {
        return false;
    }

    if (_intrQueue == nullptr) {
        _intrQueue = xQueueCreate(4, sizeof(uint32_t));
        if (_intrQueue == nullptr) {
            return false;
        }
    }

    if (!s_gpio_isr_service_ready) {
        esp_err_t ret = gpio_install_isr_service(0);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return false;
        }
        s_gpio_isr_service_ready = true;
    }

    if (gpio_isr_handler_add(static_cast<gpio_num_t>(_intPin), _isrHandler, this) != ESP_OK) {
        return false;
    }

    if (_pollTask == nullptr) {
        if (!_setupPolling()) {
            return false;
        }
    }

    return true;
}

bool m5::M5Tab5Keyboard::_setupPolling()
{
    if (_pollTask != nullptr) {
        return true;
    }

    BaseType_t ok = xTaskCreate(_pollTaskFunc, "tab5_kb_poll", 4096, this, 5, &_pollTask);
    return ok == pdPASS;
}

void m5::M5Tab5Keyboard::_cleanupPolling()
{
    if (_pollTask != nullptr) {
        vTaskDelete(_pollTask);
        _pollTask = nullptr;
    }
}

void m5::M5Tab5Keyboard::_cleanupHardwareInterrupt()
{
    if (_intPin >= 0) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(_intPin));
    }

    if (_intrQueue != nullptr) {
        vQueueDelete(_intrQueue);
        _intrQueue = nullptr;
    }

    if (_pollTask != nullptr) {
        vTaskDelete(_pollTask);
        _pollTask = nullptr;
    }
}

#endif  // ARDUINO
