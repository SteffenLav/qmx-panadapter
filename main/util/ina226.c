#include "ina226.h"
#include <string.h>

#define INA226_REG_CONFIG   0x00
#define INA226_REG_SHUNT    0x01
#define INA226_REG_BUS      0x02

// 16 averages | 1100us bus conv | 1100us shunt conv | shunt+bus continuous
// = (0b010 << 9) | (0b100 << 6) | (0b100 << 3) | 0b111 = 0x527
#define INA226_CONFIG_VALUE 0x527

#define INA226_TIMEOUT_MS   100

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t write_reg16(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(s_dev, buf, 3, INA226_TIMEOUT_MS);
}

static esp_err_t read_reg16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, 2, INA226_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

esp_err_t ina226_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (s_dev) return ESP_OK;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    return write_reg16(INA226_REG_CONFIG, INA226_CONFIG_VALUE);
}

esp_err_t ina226_read_bus_mv(uint32_t *mv)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint16_t raw;
    esp_err_t err = read_reg16(INA226_REG_BUS, &raw);
    if (err != ESP_OK) return err;
    *mv = ((uint32_t)raw * 5) / 4;  // 1.25 mV/LSB, exact
    return ESP_OK;
}

esp_err_t ina226_read_shunt_ma(int32_t *ma)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint16_t raw_u;
    esp_err_t err = read_reg16(INA226_REG_SHUNT, &raw_u);
    if (err != ESP_OK) return err;
    int16_t raw = (int16_t)raw_u;
    *ma = (int32_t)raw / 2;  // 2.5uV/LSB / 0.005ohm = 0.5 mA/LSB
    return ESP_OK;
}