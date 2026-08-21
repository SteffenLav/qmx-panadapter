#include "tab5_keyboard.h"

#include <string.h>
#include <stdio.h>

/* Set to 1 to log the raw payload of every String-mode event (modifier + hex
 * bytes). Used to discover what the non-letter keys emit. */
#define KB_DEBUG_BYTES 0

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "tab5_kbd";

/* ---- Bus / device geometry (from docs/tab5-keyboard-ref/NOTES.md) ---- */
#define KB_I2C_PORT      I2C_NUM_1   /* port 0 is the SYS bus; port 1 (EXT) is unused by us */
#define KB_SDA_GPIO      0
#define KB_SCL_GPIO      1
#define KB_I2C_HZ        100000
#define KB_ADDR          0x6D
#define KB_I2C_TIMEOUT_MS 50

/* ---- Register map (STM32 slave) ---- */
#define REG_INT_STA        0x01  /* bit0=Normal, bit1=HID, bit2=String event pending */
#define REG_EVENT_NUM      0x02  /* queued event count; write 0 to clear */
#define REG_KEYBOARD_MODE  0x10  /* 0=Normal 1=HID 2=String 3=BLE */
#define REG_CHAR_EVENT_LEN 0x40  /* String-mode pending payload length */
#define REG_CHAR_EVENT_BASE 0x50 /* [modifier, char0, char1, ...] */
#define REG_VERSION        0xFE
#define REG_INT_CFG        0x00  /* also used as the "is it alive?" probe register */

#define KB_MODE_STRING     2

#define KB_INT_STA_STRING  0x04  /* bit2 */

#define KB_POLL_MS         50

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_present = false;

static tab5_kbd_text_cb_t s_cb = NULL;
static void *s_cb_arg = NULL;

/* ---- Low-level register access (write reg, repeated-start, read N) ---- */
static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, KB_I2C_TIMEOUT_MS);
}

static esp_err_t reg_read8(uint8_t reg, uint8_t *val)
{
    return reg_read(reg, val, 1);
}

static esp_err_t reg_write8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, KB_I2C_TIMEOUT_MS);
}

void tab5_keyboard_set_text_cb(tab5_kbd_text_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
}

bool tab5_keyboard_present(void)
{
    return s_present;
}

/* Read and dispatch all pending String-mode events. Mirrors the loop in
 * M5Stack's M5Tab5Keyboard::_handleInterrupt() for MODE_STRING. */
static void drain_string_events(void)
{
    uint8_t count = 0;
    if (reg_read8(REG_EVENT_NUM, &count) != ESP_OK) return;

    int guard = 0;
    while (count > 0 && guard++ < 32) {
        uint8_t len = 0;
        if (reg_read8(REG_CHAR_EVENT_LEN, &len) == ESP_OK && len > 0 && len <= 15) {
            uint8_t buf[17];               /* [modifier][chars...] */
            if (reg_read(REG_CHAR_EVENT_BASE, buf, len + 1) == ESP_OK) {
                /* buf[0] is the modifier byte; buf[1..len] are the chars. */
                char text[16];
                memcpy(text, &buf[1], len);
                text[len] = '\0';
#if KB_DEBUG_BYTES
                char hex[64]; int o = 0;
                for (int i = 0; i <= len && o < (int)sizeof(hex) - 4; i++)
                    o += snprintf(hex + o, sizeof(hex) - o, "%02X ", buf[i]);
                ESP_LOGI(TAG, "evt mod=0x%02X len=%u bytes=[ %s] str=\"%s\"", buf[0], len, hex, text);
#endif
                if (s_cb) s_cb(text, buf[0], s_cb_arg);
            }
        }
        count--;
    }

    /* Clear the interrupt-status latch so the next event re-raises it. */
    reg_write8(REG_INT_STA, 0x00);
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t status = 0;
        if (reg_read8(REG_INT_STA, &status) == ESP_OK && (status & KB_INT_STA_STRING)) {
            drain_string_events();
        }
        vTaskDelay(pdMS_TO_TICKS(KB_POLL_MS));
    }
}

/* Scan the keyboard bus and log what answers — diagnostic for the case where
 * the keyboard is not at the expected address/pins on this hardware. */
static void scan_bus(void)
{
    ESP_LOGW(TAG, "no device at 0x%02X on GPIO%d/%d — scanning bus:", KB_ADDR, KB_SDA_GPIO, KB_SCL_GPIO);
    bool any = false;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 20) == ESP_OK) {
            ESP_LOGW(TAG, "  found device at 0x%02X", addr);
            any = true;
        }
    }
    if (!any) ESP_LOGW(TAG, "  (no devices found on GPIO%d/%d)", KB_SDA_GPIO, KB_SCL_GPIO);
}

esp_err_t tab5_keyboard_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = KB_I2C_PORT,
        .sda_io_num                   = KB_SDA_GPIO,
        .scl_io_num                   = KB_SCL_GPIO,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus(GPIO%d/%d) failed: 0x%x", KB_SDA_GPIO, KB_SCL_GPIO, err);
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = KB_ADDR,
        .scl_speed_hz    = KB_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device(0x%02X) failed: 0x%x", KB_ADDR, err);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    /* Probe: a successful read of INT_CFG means the keyboard is attached. */
    uint8_t probe = 0;
    if (reg_read8(REG_INT_CFG, &probe) != ESP_OK) {
        scan_bus();
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        s_dev = NULL;
        s_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t ver = 0;
    reg_read8(REG_VERSION, &ver);
    ESP_LOGI(TAG, "Tab5 keyboard detected at 0x%02X (fw 0x%02X) on GPIO%d/%d",
             KB_ADDR, ver, KB_SDA_GPIO, KB_SCL_GPIO);

    /* String mode: STM32 returns ready ASCII; clear any stale queue/latch. */
    reg_write8(REG_KEYBOARD_MODE, KB_MODE_STRING);
    reg_write8(REG_EVENT_NUM, 0x00);
    reg_write8(REG_INT_STA, 0x00);

    s_present = true;
    xTaskCreate(poll_task, "tab5_kb", 4096, NULL, 4, NULL);
    return ESP_OK;
}
