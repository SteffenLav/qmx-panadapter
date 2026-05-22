#include "cat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "usb/usb_host.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "cat";

// Task handle for our enumerator/observer task
static TaskHandle_t s_cat_task_handle = NULL;
// Client handle for talking to USB Host library
static usb_host_client_handle_t s_client_handle = NULL;

// Forward decls
static void cat_task(void *arg);
static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);
static void log_new_device(uint8_t dev_addr);

esp_err_t cat_init(void)
{
    ESP_LOGI(TAG, "CAT init (Phase 2.1 - USB Host enumeration only)");

    // Step 1: ask the BSP to start the USB host stack.
    // BSP handles: enabling 5V on the USB-A port via I/O expander,
    //              installing usb_host driver, and starting its event task.
    // Power mode USB5V means: provide 5V to the connected device.
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_usb_host_start failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "BSP USB host started, 5V enabled on USB-A port");

    // Step 2: register a client with the usb_host library so we get
    // notified when devices connect/disconnect.
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    err = usb_host_client_register(&client_config, &s_client_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "USB host client registered");

    // Step 3: spawn our observer task. It pumps client events.
    BaseType_t ok = xTaskCreatePinnedToCore(
        cat_task, "cat_task", 4096, NULL, 5, &s_cat_task_handle, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cat_task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CAT task started, waiting for USB devices...");

    return ESP_OK;
}

// Client event callback: invoked by usb_host_client_handle_events()
// in the context of our cat_task.
static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, ">>> NEW_DEV event: addr=%d", event_msg->new_dev.address);
        log_new_device(event_msg->new_dev.address);
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "<<< DEV_GONE event: device handle=%p",
                 event_msg->dev_gone.dev_hdl);
        break;

    default:
        ESP_LOGI(TAG, "Other client event: %d", event_msg->event);
        break;
    }
}

// Open a freshly-enumerated device and log its identifying info.
static void log_new_device(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl;
    esp_err_t err = usb_host_device_open(s_client_handle, dev_addr, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_device_open(addr=%d) failed: 0x%x", dev_addr, err);
        return;
    }

    // Get device descriptor (VID, PID, class info)
    const usb_device_desc_t *dev_desc;
    err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    if (err == ESP_OK && dev_desc != NULL) {
        ESP_LOGI(TAG, "    VID=0x%04X PID=0x%04X",
                 dev_desc->idVendor, dev_desc->idProduct);
        ESP_LOGI(TAG, "    Class=0x%02X SubClass=0x%02X Protocol=0x%02X",
                 dev_desc->bDeviceClass, dev_desc->bDeviceSubClass,
                 dev_desc->bDeviceProtocol);
        ESP_LOGI(TAG, "    USB version: %d.%02d",
                 dev_desc->bcdUSB >> 8, dev_desc->bcdUSB & 0xFF);
        ESP_LOGI(TAG, "    Max packet size EP0: %d", dev_desc->bMaxPacketSize0);
        ESP_LOGI(TAG, "    Number of configurations: %d",
                 dev_desc->bNumConfigurations);
    } else {
        ESP_LOGE(TAG, "usb_host_get_device_descriptor failed: 0x%x", err);
    }

    // Optionally fetch the configuration descriptor so we can see interface classes.
    // CDC-ACM devices typically expose two interfaces: control + data.
    const usb_config_desc_t *config_desc;
    err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (err == ESP_OK && config_desc != NULL) {
        ESP_LOGI(TAG, "    Active config has %d interfaces, total length %d",
                 config_desc->bNumInterfaces, config_desc->wTotalLength);
    }

    // Close the device handle - we just wanted to peek for now.
    // In Phase 2.2 we'll keep it open and claim the CDC interface.
    usb_host_device_close(s_client_handle, dev_hdl);
}

// Main event-processing loop for our client.
// usb_host_client_handle_events() will invoke client_event_cb() when needed.
static void cat_task(void *arg)
{
    while (1) {
        esp_err_t err = usb_host_client_handle_events(s_client_handle, portMAX_DELAY);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "usb_host_client_handle_events returned 0x%x", err);
        }
    }
}
