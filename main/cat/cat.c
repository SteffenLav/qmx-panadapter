#include "cat.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "cat";

// QMX (and QMX+, same family) identifiers (STM32 with QRP Labs custom firmware)
#define QMX_VID  0x0483
#define QMX_PID  0xA34C

// Baud rate for Kenwood TS-480 CAT emulation on QMX
#define CAT_BAUD_RATE 38400

// Event bits for signaling between USB host events and our task
#define EVT_DEV_CONNECTED  BIT0
#define EVT_DEV_GONE       BIT1

// State
static TaskHandle_t s_cat_task_handle = NULL;
static EventGroupHandle_t s_evt_group = NULL;
static cdc_acm_dev_hdl_t s_cdc_dev = NULL;

// Forward decls
static void cat_task(void *arg);
static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg);
static void handle_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static esp_err_t try_open_qmx(void);

esp_err_t cat_init(void)
{
    ESP_LOGI(TAG, "CAT init (Phase 2.2 - CDC-ACM to QMX)");

    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Step 1: bring up USB host via BSP
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_usb_host_start failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "BSP USB host started, 5V enabled on USB-A port");

    // Step 2: install the CDC-ACM host class driver
    err = cdc_acm_host_install(NULL);  // NULL = default config (own task, default stack/priority)
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    // Step 3: spawn our task that watches for QMX and drives CAT communication
    BaseType_t ok = xTaskCreatePinnedToCore(
        cat_task, "cat_task", 8192, NULL, 5, &s_cat_task_handle, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cat_task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CAT task started, looking for QMX (VID=0x%04X PID=0x%04X)...",
             QMX_VID, QMX_PID);

    return ESP_OK;
}

// CDC-ACM event callback (connection lost, error, etc.)
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
        ESP_LOGI(TAG, "Serial state notification: 0x%04X",
                 event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
        // Not used for serial CDC
        break;
    default:
        ESP_LOGI(TAG, "Other CDC event: %d", event->type);
        break;
    }
}

// Inbound data handler — called when the QMX sends bytes to us
static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg)
{
    // CAT responses are ASCII text, semicolon-terminated.
    // Log as a string for easy reading. Cap at 128 chars per line.
    char buf[129];
    size_t copy_len = (data_len < sizeof(buf) - 1) ? data_len : sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';
    ESP_LOGI(TAG, "<<< QMX RX (%d bytes): %s", (int)data_len, buf);
    return true; // we've consumed the data
}

// Try to open the QMX as a CDC-ACM device
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

    // Interface 0 is the CDC Communications Class on the QMX.
    // (The CDC-ACM helper picks the data interface automatically based on descriptors.)
    ESP_LOGI(TAG, "Attempting to open QMX CDC interface...");
    esp_err_t err = cdc_acm_host_open(QMX_VID, QMX_PID, 0, &cfg, &s_cdc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_open failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "QMX CDC opened successfully");

    // Configure line coding: 38400 baud, 8N1
    const cdc_acm_line_coding_t line_coding = {
        .dwDTERate = CAT_BAUD_RATE,
        .bCharFormat = 0,    // 1 stop bit
        .bParityType = 0,    // no parity
        .bDataBits = 8,
    };
    err = cdc_acm_host_line_coding_set(s_cdc_dev, &line_coding);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_line_coding_set failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Line coding set: %d baud, 8N1", CAT_BAUD_RATE);

    // Assert DTR and RTS so the radio knows we want to talk
    err = cdc_acm_host_set_control_line_state(s_cdc_dev, true, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_set_control_line_state failed: 0x%x", err);
        // not fatal; some implementations don't care
    }

    // Send a test command: "ID;" asks the radio for its identity (TS-480 protocol)
    const char *test_cmd = "ID;";
    err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)test_cmd,
                                        strlen(test_cmd), 500);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send ID;: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, ">>> QMX TX: %s", test_cmd);

    return ESP_OK;
}

// Main task — handles connect attempts and orchestrates CAT lifecycle
static void cat_task(void *arg)
{
    while (1) {
        // Try to open the QMX. If it's not present, this returns quickly with
        // an error and we wait before retrying.
        esp_err_t err = try_open_qmx();
        if (err == ESP_OK) {
            // Successfully opened. Wait for it to disappear.
            EventBits_t bits = xEventGroupWaitBits(
                s_evt_group, EVT_DEV_GONE, pdTRUE, pdFALSE, portMAX_DELAY);
            if (bits & EVT_DEV_GONE) {
                ESP_LOGW(TAG, "Cleaning up after QMX disconnect");
                if (s_cdc_dev) {
                    cdc_acm_host_close(s_cdc_dev);
                    s_cdc_dev = NULL;
                }
            }
        } else {
            // Not present (or open failed). Wait a bit then retry.
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}
