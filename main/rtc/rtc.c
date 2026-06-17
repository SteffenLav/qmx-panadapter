#include "rtc.h"
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "rtc";

// RX8130CE register addresses
#define REG_SEC      0x10  // BCD, mask 0x7F
#define REG_MIN      0x11  // BCD, mask 0x7F
#define REG_HOUR     0x12  // BCD, mask 0x3F
#define REG_WEEKDAY  0x13  // bit-encoded: bit N = weekday N (0=Sun)
#define REG_DAY      0x14  // BCD, mask 0x3F
#define REG_MONTH    0x15  // BCD, mask 0x1F
#define REG_YEAR     0x16  // BCD 00-99 => 2000-2099
#define REG_IRQ_EN   0x1E  // interrupt enable
#define REG_EXT      0x1F  // extension register
#define REG_FLAG     0x1D  // flag register; bit 0x80 = VBLF (power failure)
#define REG_TIMER0   0x30  // timer counter low (cleared on init)

#define I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_rtc_valid = false;

static uint8_t bcd_to_byte(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static uint8_t byte_to_bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len > sizeof(buf) - 1) return ESP_ERR_INVALID_ARG;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write8(uint8_t reg, uint8_t val)
{
    return reg_write(reg, &val, 1);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_read8(uint8_t reg, uint8_t *val)
{
    return reg_read(reg, val, 1);
}

esp_err_t rtc_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x32,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add RX8130CE device: 0x%x", err);
        return err;
    }

    // Init sequence from M5Unified RX8130_Class::begin():
    //   bitOn(0x1F, 0x30)      — set bits [5:4] in extension register
    //   writeRegister8(0x30, 0x00) — clear timer counter register
    //   writeRegister8(0x1E, 0x00) — disable all IRQs
    uint8_t ext = 0;
    if (reg_read8(REG_EXT, &ext) == ESP_OK) {
        ext |= 0x30;
        reg_write8(REG_EXT, ext);
    }
    reg_write8(REG_TIMER0, 0x00);
    reg_write8(REG_IRQ_EN, 0x00);

    // Check power-failure flag
    uint8_t flags = 0x80;  // assume invalid on read error
    reg_read8(REG_FLAG, &flags);
    s_rtc_valid = !(flags & 0x80);

    if (s_rtc_valid) {
        struct tm tm_rtc = {0};
        if (rtc_get_time(&tm_rtc)) {
            ESP_LOGI(TAG, "RX8130CE valid: %04d-%02d-%02d %02d:%02d:%02d UTC",
                     tm_rtc.tm_year + 1900, tm_rtc.tm_mon + 1, tm_rtc.tm_mday,
                     tm_rtc.tm_hour, tm_rtc.tm_min, tm_rtc.tm_sec);
        } else {
            // VBLF was clear but registers contain out-of-range BCD values —
            // registers were never initialised (factory state). Mark invalid.
            s_rtc_valid = false;
            ESP_LOGW(TAG, "RX8130CE registers invalid despite VBLF=0 (uninitialised chip)");
        }
    } else {
        ESP_LOGW(TAG, "RX8130CE power-failure flag set — time invalid (supercap discharged?)");
    }

    return ESP_OK;
}

bool rtc_is_valid(void)
{
    return s_rtc_valid;
}

static bool tm_is_sane(const struct tm *tm)
{
    if (tm->tm_sec  < 0  || tm->tm_sec  > 59) return false;
    if (tm->tm_min  < 0  || tm->tm_min  > 59) return false;
    if (tm->tm_hour < 0  || tm->tm_hour > 23) return false;
    if (tm->tm_mday < 1  || tm->tm_mday > 31) return false;
    if (tm->tm_mon  < 0  || tm->tm_mon  > 11) return false;  // 0-indexed
    if (tm->tm_year < 100 || tm->tm_year > 200) return false; // 2000–2100
    return true;
}

bool rtc_get_time(struct tm *out)
{
    if (!s_dev) return false;
    uint8_t buf[7];
    if (reg_read(REG_SEC, buf, 7) != ESP_OK) return false;

    out->tm_sec  = bcd_to_byte(buf[0] & 0x7F);
    out->tm_min  = bcd_to_byte(buf[1] & 0x7F);
    out->tm_hour = bcd_to_byte(buf[2] & 0x3F);
    out->tm_wday = __builtin_ctz(buf[3]);            // bit-encoded weekday
    out->tm_mday = bcd_to_byte(buf[4] & 0x3F);
    out->tm_mon  = bcd_to_byte(buf[5] & 0x1F) - 1;  // RTC 1-indexed → tm 0-indexed
    out->tm_year = bcd_to_byte(buf[6] & 0xFF) + 100; // 00-99 → years since 1900 (2000-2099)
    out->tm_isdst = 0;

    if (!tm_is_sane(out)) {
        ESP_LOGW(TAG, "RTC registers out of range (%04d-%02d-%02d %02d:%02d:%02d) — treating as invalid",
                 out->tm_year + 1900, out->tm_mon + 1, out->tm_mday,
                 out->tm_hour, out->tm_min, out->tm_sec);
        return false;
    }
    return true;
}

bool rtc_set_time(const struct tm *tm)
{
    if (!s_dev) return false;
    uint8_t buf[7];
    buf[0] = byte_to_bcd(tm->tm_sec);
    buf[1] = byte_to_bcd(tm->tm_min);
    buf[2] = byte_to_bcd(tm->tm_hour);
    buf[3] = (uint8_t)(1u << (tm->tm_wday & 0x07));  // bit-encoded weekday
    buf[4] = byte_to_bcd(tm->tm_mday);
    buf[5] = byte_to_bcd(tm->tm_mon + 1);            // tm 0-indexed → RTC 1-indexed
    buf[6] = byte_to_bcd(tm->tm_year % 100);          // years since 1900 → 00-99

    if (reg_write(REG_SEC, buf, 7) != ESP_OK) return false;

    // Clear the VBLF power-failure flag now that we have a valid time
    uint8_t flags = 0;
    if (reg_read8(REG_FLAG, &flags) == ESP_OK && (flags & 0x80)) {
        flags &= ~0x80;
        reg_write8(REG_FLAG, flags);
    }
    s_rtc_valid = true;
    return true;
}

bool rtc_apply_to_system(void)
{
    if (!s_dev || !s_rtc_valid) return false;
    struct tm tm_rtc = {0};
    if (!rtc_get_time(&tm_rtc)) return false;

    // mktime() is safe here: ESP-IDF newlib runs with UTC as the default
    // timezone (no TZ env var), so mktime() == timegm() on this platform.
    time_t utc = mktime(&tm_rtc);
    if (utc < 0) return false;

    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "System clock set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm_rtc.tm_year + 1900, tm_rtc.tm_mon + 1, tm_rtc.tm_mday,
             tm_rtc.tm_hour, tm_rtc.tm_min, tm_rtc.tm_sec);
    return true;
}
