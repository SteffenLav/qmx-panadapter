#include "cat.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
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

static TaskHandle_t s_link_task = NULL;
static TaskHandle_t s_poll_task = NULL;
static EventGroupHandle_t s_evt_group = NULL;
static cdc_acm_dev_hdl_t s_cdc_dev = NULL;

static char s_rx_buf[CAT_RX_BUFFER_SIZE];
static size_t s_rx_len = 0;
static uint32_t s_last_freq_hz = 0;

static void link_task(void *arg);
static void poll_task(void *arg);
static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg);
static void handle_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static esp_err_t try_open_qmx(void);
static void process_cat_message(const char *msg, size_t len);

esp_err_t cat_init(void)
{
    ESP_LOGI(TAG, "CAT init (Phase 2.3 - polling frequency, updating UI)");

    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) return ESP_ERR_NO_MEM;

    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_usb_host_start failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "BSP USB host started");

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
    ESP_LOGI(TAG, "<<< CAT msg (%d): %s", (int)len, msg);
    if (len == 15 && msg[0] == 'F' && msg[1] == 'A') {
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

    if (len == 6 && msg[0] == 'I' && msg[1] == 'D') {
        ESP_LOGI(TAG, "Radio ID: %s", msg);
        return;
    }

    ESP_LOGD(TAG, "Unhandled CAT msg: %s", msg);
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
    return ESP_OK;
}

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "Poll task started (%d ms interval)", CAT_POLL_INTERVAL_MS);
    while (s_cdc_dev != NULL) {
        esp_err_t err = cdc_acm_host_data_tx_blocking(
            s_cdc_dev, (const uint8_t *)"FA;", 3, 200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "FA; send failed: 0x%x (radio likely disconnected)", err);
            break;
        }
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

