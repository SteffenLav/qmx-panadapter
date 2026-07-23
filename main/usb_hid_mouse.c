#include "usb_hid_mouse.h"

#include "usb/hid_host.h"
#include "usb/hid_usage_mouse.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "hidmouse";

// Landscape logical screen (matches the rotated LVGL coordinate space).
#define SCR_W 1280
#define SCR_H 720

static volatile int     s_x = SCR_W / 2;
static volatile int     s_y = SCR_H / 2;
static volatile uint8_t s_buttons = 0;
static volatile bool    s_present = false;

bool usb_hid_mouse_present(void) { return s_present; }

void usb_hid_mouse_get(int *x, int *y, uint8_t *buttons)
{
    if (x)       *x = s_x;
    if (y)       *y = s_y;
    if (buttons) *buttons = s_buttons;
}

// Runs on the HID host background task.
static void iface_cb(hid_host_device_handle_t hdl,
                     const hid_host_interface_event_t event, void *arg)
{
    uint8_t data[16];
    size_t len = 0;
    hid_host_dev_params_t p;
    hid_host_device_get_params(hdl, &p);

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(hdl, data, sizeof data, &len) != ESP_OK)
            return;
        if (p.proto == HID_PROTOCOL_MOUSE && len >= 3) {
            // Boot-protocol mouse: [0]=buttons, [1]=dx(int8), [2]=dy(int8), [3]=wheel.
            int8_t dx = (int8_t)data[1];
            int8_t dy = (int8_t)data[2];
            int nx = s_x + dx, ny = s_y + dy;
            if (nx < 0) nx = 0; else if (nx >= SCR_W) nx = SCR_W - 1;
            if (ny < 0) ny = 0; else if (ny >= SCR_H) ny = SCR_H - 1;
            s_x = nx; s_y = ny;

            uint8_t b = data[0];
            if (b != s_buttons) {
                s_buttons = b;
                ESP_LOGI(TAG, "buttons=0x%02x at (%d,%d)", b, nx, ny);
            }
            // Throttle movement logging so it doesn't flood the diag ring.
            static int cnt = 0;
            if (++cnt >= 25) { cnt = 0; ESP_LOGI(TAG, "cursor (%d,%d)", nx, ny); }
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID interface disconnected");
        s_present = false;
        hid_host_device_close(hdl);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    default:
        break;
    }
}

// Runs on the HID host background task when a HID device connects.
static void dev_cb(hid_host_device_handle_t hdl,
                   const hid_host_driver_event_t event, void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t p;
    hid_host_device_get_params(hdl, &p);
    ESP_LOGI(TAG, "HID connected: proto=%d sub_class=%d addr=%d iface=%d",
             (int)p.proto, (int)p.sub_class, (int)p.addr, (int)p.iface_num);

    const hid_host_device_config_t dc = { .callback = iface_cb, .callback_arg = NULL };
    esp_err_t e = hid_host_device_open(hdl, &dc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "device_open failed: %s", esp_err_to_name(e)); return; }

    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE)
        hid_class_request_set_protocol(hdl, HID_REPORT_PROTOCOL_BOOT);

    e = hid_host_device_start(hdl);
    if (e != ESP_OK) { ESP_LOGE(TAG, "device_start failed: %s", esp_err_to_name(e)); return; }

    if (p.proto == HID_PROTOCOL_MOUSE) {
        s_present = true;
        ESP_LOGI(TAG, "mouse ready");
    }
}

void usb_hid_mouse_init(void)
{
    const hid_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 4,   // modest; HID reports are low-rate
        .stack_size    = 4096,
        .core_id       = 1,   // keep off core 0 (audio/FFT pipeline)
        .callback      = dev_cb,
        .callback_arg  = NULL,
    };
    esp_err_t e = hid_host_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "USB HID host installed (mouse, Phase 1)");
}
