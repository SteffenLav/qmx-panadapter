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

/* Two earlier attempts at this comment claimed stock "binding" mode (0)
 * would just handle attach/active correctly on its own with zero code -
 * WRONG, contradicted on hardware: stock mode is a richer state machine than
 * "hot=one colour, boot=another" (it showed purple on a hot attach and green
 * on a boot attach, neither matching what was assumed here before), so
 * leaving RGB_MODE untouched does NOT give predictable behaviour. The actual
 * ask - "the keyboard is always active the moment it's claimed, so there is
 * nothing left for a light to usefully report, so show NOTHING" - needs
 * custom mode ANYWAY, just pointed at off instead of a colour. */
#define REG_RGB_MODE       0x11  /* 0=binding (stock, richer than a simple
                                     attach/active pair - do not rely on it
                                     meaning any one thing), 1=custom */
#define REG_RGB_COLOR_BASE 0x60  /* 7-byte window: [LED0 B,G,R] [reserved]
                                     [LED1 B,G,R]. LED1 is at +4, NOT +3 (see
                                     git history - the reference driver's
                                     index*3 formula is off by one). */
#define RGB_MODE_CUSTOM    1

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

/* Force both LEDs dark. Custom mode is required for this to stick - the
 * whole point is to override stock mode's own state machine, not read it. */
static void leds_off(void)
{
    reg_write8(REG_RGB_MODE, RGB_MODE_CUSTOM);
    uint8_t buf[8] = { REG_RGB_COLOR_BASE, 0,0,0, 0, 0,0,0 };  /* reg + 7-byte window, all zero */
    i2c_master_transmit(s_dev, buf, sizeof(buf), KB_I2C_TIMEOUT_MS);
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

#define KB_ATTACH_RETRY_MS      2000
#define KB_DETACH_FAIL_THRESHOLD  5   /* consecutive failed polls (~250 ms) before
                                          declaring the keyboard gone - long enough
                                          to ride out a transient bus glitch, short
                                          enough that a real detach is noticed fast */

/* Put an already-answering keyboard into String mode. Returns true once done. */
static bool claim_keyboard(void)
{
    uint8_t probe = 0;
    if (reg_read8(REG_INT_CFG, &probe) != ESP_OK) return false;

    uint8_t ver = 0;
    reg_read8(REG_VERSION, &ver);
    ESP_LOGI(TAG, "Tab5 keyboard detected at 0x%02X (fw 0x%02X) on GPIO%d/%d",
             KB_ADDR, ver, KB_SDA_GPIO, KB_SCL_GPIO);

    /* String mode: STM32 returns ready ASCII; clear any stale queue/latch. */
    reg_write8(REG_KEYBOARD_MODE, KB_MODE_STRING);
    reg_write8(REG_EVENT_NUM, 0x00);
    reg_write8(REG_INT_STA, 0x00);

    /* No light at all, on purpose: once we get here the keyboard is already
     * fully functional regardless of whether this is a boot-time or hot
     * attach, so there is nothing left for stock mode's own indicator to
     * usefully distinguish - and it does distinguish SOMETHING (purple vs
     * green, observed on hardware), just not "does it work", which is the
     * only question that ever mattered here. Reasserted on every claim -
     * including a reattach, via the retry loop below - so it can never come
     * back stuck on whatever colour stock mode was showing a moment before
     * we took over. */
    leds_off();

    s_present = true;
    return true;
}

/* One task, for the life of the firmware, in two states:
 *   - not present: re-probe every KB_ATTACH_RETRY_MS until claim_keyboard()
 *     lands. Covers both "not attached at boot" and "was detached, now
 *     reattached" - there is no separate one-shot watcher any more, because
 *     a one-shot watcher is exactly what left a reattached keyboard stuck
 *     unclaimed (found on hardware: detach a claimed keyboard, reattach it,
 *     and nothing ever re-ran claim_keyboard() a second time).
 *   - present: normal 50 ms String-mode poll. A run of
 *     KB_DETACH_FAIL_THRESHOLD failed reads in a row (the STM32 stops
 *     acking once physically unplugged) flips back to "not present" and the
 *     loop returns to re-probing - so a later reattach gets String mode
 *     re-applied and typing works again without a Tab5 reboot. */
static void kb_task(void *arg)
{
    (void)arg;
    int fails = 0;
    for (;;) {
        if (!s_present) {
            if (claim_keyboard()) { fails = 0; continue; }
            vTaskDelay(pdMS_TO_TICKS(KB_ATTACH_RETRY_MS));
            continue;
        }

        uint8_t status = 0;
        esp_err_t err = reg_read8(REG_INT_STA, &status);
        if (err == ESP_OK) {
            fails = 0;
            if (status & KB_INT_STA_STRING) drain_string_events();
        } else if (++fails >= KB_DETACH_FAIL_THRESHOLD) {
            ESP_LOGI(TAG, "Tab5 keyboard no longer answering - treating as detached");
            s_present = false;
            fails = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(KB_POLL_MS));
    }
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

    /* Try once now, purely so the boot log and this call's return value are
     * accurate immediately - kb_task (below) would find it on its own first
     * iteration a moment later regardless, since it checks s_present before
     * doing anything else. */
    bool found = claim_keyboard();
    if (!found) scan_bus();   /* diagnostic: what IS on this bus, if not the keyboard */

    /* One task for the whole session - keeps retrying while absent (covers
     * both "not attached at boot" and "was detached, now reattached") and
     * polls normally while present. See its own comment for why this
     * replaced a one-shot boot-time claim. */
    xTaskCreate(kb_task, "tab5_kb", 4096, NULL, 4, NULL);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
