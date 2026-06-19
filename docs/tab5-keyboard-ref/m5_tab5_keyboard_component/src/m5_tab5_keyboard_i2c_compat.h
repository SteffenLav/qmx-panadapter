/*
 * SPDX-FileCopyrightText: 2025-2026 M5Stack
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __M5_TAB5_KEYBOARD_I2C_COMPAT_H__
#define __M5_TAB5_KEYBOARD_I2C_COMPAT_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#ifdef ARDUINO

#include "Wire.h"

// ============================
// Arduino I2C 功能
// Arduino I2C Functions
// ============================

/**
 * @brief 读取单个字节 (Arduino)
 *        Read single byte (Arduino)
 *
 * @param wire TwoWire 实例指针 / TwoWire instance pointer
 * @param addr I2C 设备地址 / I2C device address
 * @param reg 寄存器地址 / Register address
 * @param[out] data 输出：读取的数据 / Output: read data
 * @return true 成功 / Success
 * @return false 失败 / Failed
 */
#ifndef TAB5_KB_I2C_READ_BYTE
static inline bool TAB5_KB_I2C_READ_BYTE(TwoWire *wire, uint8_t addr, uint8_t reg, uint8_t *data)
{
    wire->beginTransmission(addr);
    wire->write(reg);
    if (wire->endTransmission(false) != 0) {
        return false;
    }
    if (wire->requestFrom(addr, (uint8_t)1) != 1) {
        return false;
    }
    *data = wire->read();
    return true;
}
#endif

/**
 * @brief 读取多个字节 (Arduino)
 *        Read multiple bytes (Arduino)
 *
 * @param wire TwoWire 实例指针 / TwoWire instance pointer
 * @param addr I2C 设备地址 / I2C device address
 * @param start_reg 起始寄存器地址 / Start register address
 * @param len 读取长度 / Read length
 * @param[out] data 输出：读取的数据缓冲区 / Output: read data buffer
 * @return true 成功 / Success
 * @return false 失败 / Failed
 */
#ifndef TAB5_KB_I2C_READ_BYTES
static inline bool TAB5_KB_I2C_READ_BYTES(TwoWire *wire, uint8_t addr, uint8_t start_reg, size_t len, uint8_t *data)
{
    wire->beginTransmission(addr);
    wire->write(start_reg);
    if (wire->endTransmission(false) != 0) {
        return false;
    }
    if (wire->requestFrom(addr, (uint8_t)len) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = wire->read();
    }
    return true;
}
#endif

/**
 * @brief 读取 16 位寄存器 (Arduino)
 *        Read 16-bit register (Arduino)
 *
 * @param wire TwoWire 实例指针 / TwoWire instance pointer
 * @param addr I2C 设备地址 / I2C device address
 * @param reg 寄存器地址 / Register address
 * @param[out] data 输出：16 位数据（小端序） / Output: 16-bit data (little-endian)
 * @return true 成功 / Success
 * @return false 失败 / Failed
 */
#ifndef TAB5_KB_I2C_READ_REG16
static inline bool TAB5_KB_I2C_READ_REG16(TwoWire *wire, uint8_t addr, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    if (!TAB5_KB_I2C_READ_BYTES(wire, addr, reg, 2, buf)) {
        return false;
    }
    // 小端模式：低字节在前
    // Little-endian: low byte first
    *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}
#endif

/**
 * @brief 写入单个字节 (Arduino)
 *        Write single byte (Arduino)
 *
 * @param wire TwoWire 实例指针 / TwoWire instance pointer
 * @param addr I2C 设备地址 / I2C device address
 * @param reg 寄存器地址 / Register address
 * @param data 要写入的数据 / Data to write
 * @return true 成功 / Success
 * @return false 失败 / Failed
 */
#ifndef TAB5_KB_I2C_WRITE_BYTE
static inline bool TAB5_KB_I2C_WRITE_BYTE(TwoWire *wire, uint8_t addr, uint8_t reg, uint8_t data)
{
    wire->beginTransmission(addr);
    wire->write(reg);
    wire->write(data);
    if (wire->endTransmission() != 0) {
        return false;
    }
    return true;
}
#endif

/**
 * @brief 写入多个字节 (Arduino)
 *        Write multiple bytes (Arduino)
 *
 * @param wire TwoWire 实例指针 / TwoWire instance pointer
 * @param addr I2C 设备地址 / I2C device address
 * @param start_reg 起始寄存器地址 / Start register address
 * @param len 写入长度 / Write length
 * @param data 要写入的数据缓冲区 / Data buffer to write
 * @return true 成功 / Success
 * @return false 失败 / Failed
 */
#ifndef TAB5_KB_I2C_WRITE_BYTES
static inline bool TAB5_KB_I2C_WRITE_BYTES(TwoWire *wire, uint8_t addr, uint8_t start_reg, size_t len,
                                           const uint8_t *data)
{
    wire->beginTransmission(addr);
    wire->write(start_reg);
    for (size_t i = 0; i < len; i++) {
        wire->write(data[i]);
    }
    if (wire->endTransmission() != 0) {
        return false;
    }
    return true;
}
#endif

/**
 * @brief 写入 16 位寄存器 (Arduino)
 *        Write 16-bit register (Arduino)
 *
 * @param wire TwoWire 实例指针 / TwoWire instance pointer
 * @param addr I2C 设备地址 / I2C device address
 * @param reg 寄存器地址 / Register address
 * @param data 16 位数据（小端序） / 16-bit data (little-endian)
 * @return true 成功 / Success
 * @return false 失败 / Failed
 */
#ifndef TAB5_KB_I2C_WRITE_REG16
static inline bool TAB5_KB_I2C_WRITE_REG16(TwoWire *wire, uint8_t addr, uint8_t reg, uint16_t data)
{
    uint8_t buf[2];
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[0] = (uint8_t)(data & 0xFF);
    buf[1] = (uint8_t)((data >> 8) & 0xFF);
    return TAB5_KB_I2C_WRITE_BYTES(wire, addr, reg, 2, buf);
}
#endif

#else  // ESP-IDF

#include <esp_err.h>
#include <driver/i2c_master.h>  // ESP-IDF native i2c_master driver
#include <i2c_bus.h>            // esp-idf-lib i2c_bus component

#ifdef __cplusplus
extern "C" {
#endif

// ============================
// I2C 驱动类型选择
// I2C Driver Type Selection
// ============================
/**
 * @brief I2C 驱动类型枚举
 *        I2C Driver Type Enumeration
 */
typedef enum {
    M5_TAB5_KB_I2C_DRIVER_NONE = 0,      ///< 未初始化 / Not initialized
    M5_TAB5_KB_I2C_DRIVER_SELF_CREATED,  ///< 使用 i2c_port_t 自创建 / Self-created using i2c_port_t
    M5_TAB5_KB_I2C_DRIVER_MASTER,        ///< ESP-IDF 原生 i2c_master 驱动 / ESP-IDF native i2c_master driver
    M5_TAB5_KB_I2C_DRIVER_BUS            ///< esp-idf-lib i2c_bus 组件 / esp-idf-lib i2c_bus component
} m5_tab5_kb_i2c_driver_t;

// ============================
// ESP-IDF I2C 函数 (i2c_bus)
// ESP-IDF I2C Functions (i2c_bus)
// ============================

/**
 * @brief 读取单个字节 (ESP-IDF i2c_bus)
 *        Read single byte (ESP-IDF i2c_bus)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param[out] data 输出：读取的数据 / Output: read data
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_READ_BYTE
static inline esp_err_t TAB5_KB_I2C_READ_BYTE(i2c_bus_device_handle_t dev, uint8_t reg, uint8_t *data)
{
    return i2c_bus_read_byte(dev, reg, data);
}
#endif

/**
 * @brief 读取多个字节 (ESP-IDF i2c_bus)
 *        Read multiple bytes (ESP-IDF i2c_bus)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param start_reg 起始寄存器地址 / Start register address
 * @param len 读取长度 / Read length
 * @param[out] data 输出：读取的数据缓冲区 / Output: read data buffer
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_READ_BYTES
static inline esp_err_t TAB5_KB_I2C_READ_BYTES(i2c_bus_device_handle_t dev, uint8_t start_reg, size_t len,
                                               uint8_t *data)
{
    return i2c_bus_read_bytes(dev, start_reg, len, data);
}
#endif

/**
 * @brief 读取 16 位寄存器 (ESP-IDF i2c_bus)
 *        Read 16-bit register (ESP-IDF i2c_bus)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param[out] data 输出：16 位数据（小端序） / Output: 16-bit data (little-endian)
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_READ_REG16
static inline esp_err_t TAB5_KB_I2C_READ_REG16(i2c_bus_device_handle_t dev, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_bus_read_bytes(dev, reg, 2, buf);
    if (ret == ESP_OK) {
        // 小端模式：低字节在前
        // Little-endian: low byte first
        *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}
#endif

/**
 * @brief 写入单个字节 (ESP-IDF i2c_bus)
 *        Write single byte (ESP-IDF i2c_bus)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param data 要写入的数据 / Data to write
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_WRITE_BYTE
static inline esp_err_t TAB5_KB_I2C_WRITE_BYTE(i2c_bus_device_handle_t dev, uint8_t reg, uint8_t data)
{
    return i2c_bus_write_byte(dev, reg, data);
}
#endif

/**
 * @brief 写入多个字节 (ESP-IDF i2c_bus)
 *        Write multiple bytes (ESP-IDF i2c_bus)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param start_reg 起始寄存器地址 / Start register address
 * @param len 写入长度 / Write length
 * @param data 要写入的数据缓冲区 / Data buffer to write
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_WRITE_BYTES
static inline esp_err_t TAB5_KB_I2C_WRITE_BYTES(i2c_bus_device_handle_t dev, uint8_t start_reg, size_t len,
                                                const uint8_t *data)
{
    return i2c_bus_write_bytes(dev, start_reg, len, (uint8_t *)data);
}
#endif

/**
 * @brief 写入 16 位寄存器 (ESP-IDF i2c_bus)
 *        Write 16-bit register (ESP-IDF i2c_bus)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param data 16 位数据（小端序） / 16-bit data (little-endian)
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_WRITE_REG16
static inline esp_err_t TAB5_KB_I2C_WRITE_REG16(i2c_bus_device_handle_t dev, uint8_t reg, uint16_t data)
{
    uint8_t buf[2];
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[0] = (uint8_t)(data & 0xFF);
    buf[1] = (uint8_t)((data >> 8) & 0xFF);
    return i2c_bus_write_bytes(dev, reg, 2, buf);
}
#endif

// ============================
// ESP-IDF I2C 函数 (i2c_master - 原生驱动)
// ESP-IDF I2C Functions (i2c_master - native driver)
// ============================

/**
 * @brief 读取单个字节 (ESP-IDF i2c_master)
 *        Read single byte (ESP-IDF i2c_master)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param[out] data 输出：读取的数据 / Output: read data
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_MASTER_READ_BYTE
static inline esp_err_t TAB5_KB_I2C_MASTER_READ_BYTE(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, 1, -1);
}
#endif

/**
 * @brief 读取多个字节 (ESP-IDF i2c_master)
 *        Read multiple bytes (ESP-IDF i2c_master)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param start_reg 起始寄存器地址 / Start register address
 * @param len 读取长度 / Read length
 * @param[out] data 输出：读取的数据缓冲区 / Output: read data buffer
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_MASTER_READ_BYTES
static inline esp_err_t TAB5_KB_I2C_MASTER_READ_BYTES(i2c_master_dev_handle_t dev, uint8_t start_reg, size_t len,
                                                      uint8_t *data)
{
    return i2c_master_transmit_receive(dev, &start_reg, 1, data, len, -1);
}
#endif

/**
 * @brief 读取 16 位寄存器 (ESP-IDF i2c_master)
 *        Read 16-bit register (ESP-IDF i2c_master)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param[out] data 输出：16 位数据（小端序） / Output: 16-bit data (little-endian)
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_MASTER_READ_REG16
static inline esp_err_t TAB5_KB_I2C_MASTER_READ_REG16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, buf, 2, -1);
    if (ret == ESP_OK) {
        // 小端模式：低字节在前
        // Little-endian: low byte first
        *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}
#endif

/**
 * @brief 写入单个字节 (ESP-IDF i2c_master)
 *        Write single byte (ESP-IDF i2c_master)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param data 要写入的数据 / Data to write
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_MASTER_WRITE_BYTE
static inline esp_err_t TAB5_KB_I2C_MASTER_WRITE_BYTE(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(dev, buf, 2, -1);
}
#endif

/**
 * @brief 写入多个字节 (ESP-IDF i2c_master)
 *        Write multiple bytes (ESP-IDF i2c_master)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param start_reg 起始寄存器地址 / Start register address
 * @param len 写入长度 / Write length
 * @param data 要写入的数据缓冲区 / Data buffer to write
 * @return ESP_OK 成功 / Success
 * @return ESP_ERR_NO_MEM 内存不足 / Out of memory
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_MASTER_WRITE_BYTES
static inline esp_err_t TAB5_KB_I2C_MASTER_WRITE_BYTES(i2c_master_dev_handle_t dev, uint8_t start_reg, size_t len,
                                                       const uint8_t *data)
{
    // 需要在数据前添加寄存器地址
    // Need to prepend register address
    uint8_t *buf = (uint8_t *)malloc(len + 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    buf[0] = start_reg;
    memcpy(buf + 1, data, len);
    esp_err_t ret = i2c_master_transmit(dev, buf, len + 1, -1);
    free(buf);
    return ret;
}
#endif

/**
 * @brief 写入 16 位寄存器 (ESP-IDF i2c_master)
 *        Write 16-bit register (ESP-IDF i2c_master)
 *
 * @param dev I2C 设备句柄 / I2C device handle
 * @param reg 寄存器地址 / Register address
 * @param data 16 位数据（小端序） / 16-bit data (little-endian)
 * @return ESP_OK 成功 / Success
 * @return 其他错误码 / Other error codes
 */
#ifndef TAB5_KB_I2C_MASTER_WRITE_REG16
static inline esp_err_t TAB5_KB_I2C_MASTER_WRITE_REG16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t data)
{
    uint8_t buf[3];
    buf[0] = reg;
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[1] = (uint8_t)(data & 0xFF);
    buf[2] = (uint8_t)((data >> 8) & 0xFF);
    return i2c_master_transmit(dev, buf, 3, -1);
}
#endif

#ifdef __cplusplus
}
#endif

#endif  // ARDUINO

#endif  // __M5_TAB5_KEYBOARD_I2C_COMPAT_H__
