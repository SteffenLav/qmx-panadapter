#include "cat.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "bsp/m5stack_tab5.h"

#include "ui.h"

static const char *TAG = "cat";

#define QMX_VID  0x0483
#define QMX_PID  0xA34C
#define CAT_BAUD_RATE 38400
#define CAT_POLL_INTERVAL_MS 200
#define CAT_RX_BUFFER_SIZE 128

#define EVT_DEV_CONNECTED  BIT0
#define EVT_DEV_GONE       BIT1

// USB Audio Class descriptor sub-types we care about
#define USB_CLASS_AUDIO              0x01
#define USB_SUBCLASS_AUDIOCONTROL    0x01
#define USB_SUBCLASS_AUDIOSTREAMING  0x02
#define USB_DESC_TYPE_CS_INTERFACE   0x24
#define USB_DESC_TYPE_CS_ENDPOINT    0x25
#define UAC_AS_GENERAL               0x01
#define UAC_AS_FORMAT_TYPE           0x02
#define UAC_FORMAT_TYPE_I            0x01

static TaskHandle_t s_link_task = NULL;
static TaskHandle_t s_poll_task = NULL;
static EventGroupHandle_t s_evt_group = NULL;
static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static bool s_audio_dumped = false;

static char s_rx_buf[CAT_RX_BUFFER_SIZE];
static size_t s_rx_len = 0;
static uint32_t s_last_freq_hz = 0;
static char s_last_mode_digit = 0;  // Phase 5.10: cached Kenwood mode digit
static uint64_t s_last_tx_us = 0;   // for rate-limiting cat_set_frequency

static void link_task(void *arg);
static void poll_task(void *arg);
static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg);
static void handle_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static esp_err_t try_open_qmx(void);
static void process_cat_message(const char *msg, size_t len);
static void dump_audio_descriptors(void);

esp_err_t cat_init(void)
{
    ESP_LOGI(TAG, "CAT init (Phase 3.1 - descriptor dump on first connect)");

    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_OK;
err = cdc_acm_host_install(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    BaseType_t ok = xTaskCreatePinnedToCore(
        link_task, "cat_link", 8192, NULL, 5, &s_link_task, 1);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "CAT link task started, waiting for QMX (VID=0x%04X PID=0x%04X)",
             QMX_VID, QMX_PID);
    return ESP_OK;
}

static void handle_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "QMX disconnected");
        xEventGroupSetBits(s_evt_group, EVT_DEV_GONE);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        break;
    default:
        break;
    }
}

static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg)
{
    for (size_t i = 0; i < data_len; i++) {
        char c = (char)data[i];
        if (s_rx_len >= CAT_RX_BUFFER_SIZE - 1) {
            ESP_LOGW(TAG, "RX buffer overflow, dropping accumulated data");
            s_rx_len = 0;
        }
        s_rx_buf[s_rx_len++] = c;
        if (c == ';') {
            s_rx_buf[s_rx_len] = '\0';
            process_cat_message(s_rx_buf, s_rx_len);
            s_rx_len = 0;
        }
    }
    return true;
}

static void process_cat_message(const char *msg, size_t len)
{
    if (len == 14 && msg[0] == 'F' && msg[1] == 'A') {
        uint32_t freq_hz = 0;
        for (size_t i = 2; i < 13; i++) {
            char d = msg[i];
            if (d < '0' || d > '9') {
                ESP_LOGW(TAG, "Bad digit in FA response: '%c'", d);
                return;
            }
            freq_hz = freq_hz * 10 + (d - '0');
        }
        if (freq_hz != s_last_freq_hz) {
            s_last_freq_hz = freq_hz;
            ESP_LOGI(TAG, "Freq = %lu Hz (%lu.%03lu MHz)",
                     (unsigned long)freq_hz,
                     (unsigned long)(freq_hz / 1000000),
                     (unsigned long)((freq_hz / 1000) % 1000));
            ui_update_frequency(freq_hz);
        }
        return;
    }

    // MD response: "MDn;" — Kenwood mode digit. QMX uses:
    // 1=LSB, 2=USB, 3=CW, 4=FM, 5=AM, 6=FSK, 7=CW-R, 9=FSK-R
    if (len == 4 && msg[0] == 'M' && msg[1] == 'D') {
        char d = msg[2];
        if (d < '1' || d > '9') {
            ESP_LOGW(TAG, "Bad mode digit in MD response: '%c'", d);
            return;
        }
        static const char *kw_modes[] = {
            "?", "LSB", "USB", "CW", "FM", "AM", "FSK", "CW-R", "?", "FSK-R"
        };
        const char *mode_str = kw_modes[d - '0'];
        if (d != s_last_mode_digit) {
            s_last_mode_digit = d;
            ESP_LOGI(TAG, "Mode = %s (raw %c)", mode_str, d);
            ui_update_mode(mode_str);
        }
        return;
    }

    if (len == 6 && msg[0] == 'I' && msg[1] == 'D') {
        ESP_LOGI(TAG, "Radio ID: %s", msg);
        return;
    }
}

// Walk the QMX's active config descriptor and log audio class interfaces,
// alt settings, format types (sample rate, channels, bit depth), and endpoints.
static void dump_audio_descriptors(void)
{
    // Find the device handle from CDC-ACM. We need raw USB host access.
    // Easier: open the device by address. Address 1 is typical for a single device.
    usb_host_client_handle_t tmp_client = NULL;
    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = NULL, .callback_arg = NULL },
    };
    esp_err_t err = usb_host_client_register(&client_cfg, &tmp_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio dump: client register failed 0x%x", err);
        return;
    }

    usb_device_handle_t dev = NULL;
    err = usb_host_device_open(tmp_client, 1, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio dump: device_open(addr=1) failed 0x%x", err);
        usb_host_client_deregister(tmp_client);
        return;
    }

    const usb_config_desc_t *cfg_desc = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg_desc);
    if (err != ESP_OK || cfg_desc == NULL) {
        ESP_LOGW(TAG, "Audio dump: get_active_config_descriptor failed 0x%x", err);
        usb_host_device_close(tmp_client, dev);
        usb_host_client_deregister(tmp_client);
        return;
    }

    ESP_LOGI(TAG, "=== USB descriptor dump (total %d bytes) ===", cfg_desc->wTotalLength);

    const uint8_t *p = (const uint8_t *)cfg_desc;
    const uint8_t *end = p + cfg_desc->wTotalLength;
    int current_iface = -1;
    int current_alt = -1;

    while (p + 2 <= end) {
        uint8_t bLength = p[0];
        uint8_t bDescriptorType = p[1];
        if (bLength == 0 || p + bLength > end) break;

        switch (bDescriptorType) {
        case 0x04: { // INTERFACE
            if (bLength >= 9) {
                current_iface = p[2];     // bInterfaceNumber
                current_alt   = p[3];     // bAlternateSetting
                uint8_t nep   = p[4];     // bNumEndpoints
                uint8_t cls   = p[5];     // bInterfaceClass
                uint8_t sub   = p[6];     // bInterfaceSubClass
                uint8_t proto = p[7];     // bInterfaceProtocol
                const char *cls_name = "?";
                if (cls == 0x02) cls_name = "CDC-Comm";
                else if (cls == 0x0A) cls_name = "CDC-Data";
                else if (cls == USB_CLASS_AUDIO) {
                    if (sub == USB_SUBCLASS_AUDIOCONTROL) cls_name = "Audio-Control";
                    else if (sub == USB_SUBCLASS_AUDIOSTREAMING) cls_name = "Audio-Streaming";
                    else cls_name = "Audio-?";
                }
                ESP_LOGI(TAG, "IF %d alt %d: class=0x%02X/0x%02X/0x%02X (%s), %d EPs",
                         current_iface, current_alt, cls, sub, proto, cls_name, nep);
            }
            break;
        }

        case 0x05: { // ENDPOINT
            if (bLength >= 7) {
                uint8_t addr = p[2];
                uint8_t attr = p[3];
                uint16_t mps = p[4] | (p[5] << 8);
                uint8_t intvl = p[6];
                const char *dir = (addr & 0x80) ? "IN" : "OUT";
                const char *type =
                    (attr & 0x03) == 0 ? "control" :
                    (attr & 0x03) == 1 ? "isochronous" :
                    (attr & 0x03) == 2 ? "bulk" : "interrupt";
                ESP_LOGI(TAG, "  EP 0x%02X %s %s mps=%u intvl=%u",
                         addr, dir, type, mps, intvl);
            }
            break;
        }

        case USB_DESC_TYPE_CS_INTERFACE: { // 0x24, class-specific interface
            if (bLength < 3) break;
            uint8_t subtype = p[2];
            // Only interesting for Audio Streaming interfaces
            if (subtype == UAC_AS_GENERAL && bLength >= 7) {
                uint8_t terminal_link = p[3];
                ESP_LOGI(TAG, "  AS_GENERAL: terminal_link=%u", terminal_link);
            } else if (subtype == UAC_AS_FORMAT_TYPE && bLength >= 8) {
                uint8_t format_type = p[3];
                uint8_t channels    = p[4];
                uint8_t subframe    = p[5];   // bytes per sample
                uint8_t bit_resolution = p[6];
                uint8_t sample_freq_type = p[7];
                ESP_LOGI(TAG, "  FORMAT_TYPE_%u: channels=%u, %u bytes/sample, %u-bit",
                         format_type, channels, subframe, bit_resolution);
                if (sample_freq_type == 0 && bLength >= 14) {
                    // Continuous range: 3-byte min, 3-byte max
                    uint32_t fmin = p[8] | (p[9] << 8) | (p[10] << 16);
                    uint32_t fmax = p[11] | (p[12] << 8) | (p[13] << 16);
                    ESP_LOGI(TAG, "    sample rate: continuous %lu..%lu Hz",
                             (unsigned long)fmin, (unsigned long)fmax);
                } else {
                    // Discrete list: N entries of 3 bytes each
                    for (int i = 0; i < sample_freq_type; i++) {
                        int off = 8 + 3 * i;
                        if (off + 3 > bLength) break;
                        uint32_t f = p[off] | (p[off+1] << 8) | (p[off+2] << 16);
                        ESP_LOGI(TAG, "    sample rate: %lu Hz", (unsigned long)f);
                    }
                }
            }
            break;
        }

        case USB_DESC_TYPE_CS_ENDPOINT: { // 0x25, class-specific endpoint
            ESP_LOGI(TAG, "  CS_ENDPOINT (audio class), %u bytes", bLength);
            break;
        }

        default:
            break;
        }
        p += bLength;
    }

    ESP_LOGI(TAG, "=== End of descriptor dump ===");

    usb_host_device_close(tmp_client, dev);
    usb_host_client_deregister(tmp_client);
}

static esp_err_t try_open_qmx(void)
{
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 256,
        .in_buffer_size = 256,
        .event_cb = handle_cdc_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    esp_err_t err = cdc_acm_host_open(QMX_VID, QMX_PID, 0, &cfg, &s_cdc_dev);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "QMX CDC opened");

    const cdc_acm_line_coding_t lc = {
        .dwDTERate = CAT_BAUD_RATE,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };
    err = cdc_acm_host_line_coding_set(s_cdc_dev, &lc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "line_coding_set failed: 0x%x", err);
        return err;
    }

    cdc_acm_host_set_control_line_state(s_cdc_dev, true, true);
    ESP_LOGI(TAG, "QMX configured: %d baud, 8N1", CAT_BAUD_RATE);

    cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)"ID;", 3, 500);

    // Phase 3.1 — dump audio descriptors ONCE per session
    if (!s_audio_dumped) {
        ESP_LOGI(TAG, "=== Dumping QMX descriptors ===");
        cdc_acm_host_desc_print(s_cdc_dev);
        ESP_LOGI(TAG, "=== End descriptor dump ===");
        s_audio_dumped = true;
    }

    return ESP_OK;
}

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "Poll task started (%d ms interval, alternating FA/MD)", CAT_POLL_INTERVAL_MS);
    int phase = 0;
    while (s_cdc_dev != NULL) {
        const char *cmd = (phase == 0) ? "FA;" : "MD;";
        esp_err_t err = cdc_acm_host_data_tx_blocking(
            s_cdc_dev, (const uint8_t *)cmd, 3, 200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s send failed: 0x%x (radio likely disconnected)", cmd, err);
            break;
        }
        phase ^= 1;
        vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
    }
    ESP_LOGI(TAG, "Poll task exiting");
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

static void link_task(void *arg)
{
    while (1) {
        esp_err_t err = try_open_qmx();
        if (err == ESP_OK) {
            s_rx_len = 0;
            s_last_freq_hz = 0;
            s_last_mode_digit = 0;
            xTaskCreatePinnedToCore(
                poll_task, "cat_poll", 4096, NULL, 5, &s_poll_task, 1);

            xEventGroupWaitBits(s_evt_group, EVT_DEV_GONE,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            ESP_LOGW(TAG, "QMX gone, cleaning up");
            if (s_cdc_dev) {
                cdc_acm_host_close(s_cdc_dev);
                s_cdc_dev = NULL;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}




esp_err_t cat_set_frequency(uint32_t freq_hz)
{
    if (s_cdc_dev == NULL) {
        return ESP_ERR_INVALID_STATE;  // QMX not connected
    }
    // Rate-limit: drop calls that arrive within 200 ms of previous TX
    uint64_t now = esp_timer_get_time();
    if (now - s_last_tx_us < 200000) {
        return ESP_ERR_TIMEOUT;
    }
    s_last_tx_us = now;

    // Format: "FA" + 11 digits zero-padded + ";"
    char cmd[16];
    int n = snprintf(cmd, sizeof(cmd), "FA%011lu;", (unsigned long)freq_hz);
    if (n != 14) {
        ESP_LOGW(TAG, "cat_set_frequency: snprintf produced %d chars (expected 14)", n);
        return ESP_FAIL;
    }
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)cmd, 14, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FA TX failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Sent: %s (target %lu Hz)", cmd, (unsigned long)freq_hz);
    return ESP_OK;
}
