#include "usb_hid_mouse.h"
#include "hid_cursor.h"
#include "hid_report_map.h"   // mice do not agree on a byte layout - ask the descriptor

#include "usb/hid_host.h"
#include "usb/hid_usage_mouse.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "hidmouse";

// Landscape logical screen (matches the rotated LVGL coordinate space).
#define SCR_W 1280
#define SCR_H 720

// Cursor state now lives in hid_cursor.c so the BLE mouse can feed the same
// accumulator. These stay as thin wrappers rather than being deleted - they
// are the USB module's own public API and reading "is a USB mouse present"
// is a different question from "is any mouse present".
static volatile bool s_present = false;

// What THIS mouse's movement report looks like, read from its own Report
// Descriptor at connect. Invalid => fall back to the boot 3-byte layout.
static hid_mouse_layout_t s_layout;

bool usb_hid_mouse_present(void) { return s_present; }

void usb_hid_mouse_get(int *x, int *y, uint8_t *buttons)
{
    hid_cursor_get(x, y, buttons);
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
            int dx, dy;
            uint8_t btn;

            if (s_layout.valid) {
                // The mouse told us where its fields are; believe it.
                const uint8_t *rep = data;
                size_t rlen = len;
                if (s_layout.report_id) {          // leading ID byte is not payload
                    if (rlen < 1 || rep[0] != s_layout.report_id) break;
                    rep++; rlen--;
                }
                dx  = hid_field_signed(rep, rlen, s_layout.x_bit, s_layout.x_bits);
                dy  = hid_field_signed(rep, rlen, s_layout.y_bit, s_layout.y_bits);
                btn = rlen ? rep[0] : 0;           // buttons are bit 0.. of byte 0
            } else {
                // Boot-protocol mouse: [0]=buttons, [1]=dx(int8), [2]=dy(int8), [3]=wheel.
                dx  = (int8_t)data[1];
                dy  = (int8_t)data[2];
                btn = data[0];
            }
            hid_cursor_apply(dx, dy, btn);
            // Throttle movement logging so it doesn't flood the diag ring.
            static int cnt = 0;
            if (++cnt >= 25) {
                cnt = 0;
                int nx, ny; uint8_t b;
                hid_cursor_get(&nx, &ny, &b);
                ESP_LOGI(TAG, "cursor (%d,%d) buttons=0x%02x", nx, ny, b);
            }
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID interface disconnected");
        s_present = false;
        hid_cursor_set_present(HID_CURSOR_SRC_USB, false);
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

    // ⛔ DO NOT ASSUME THE BOOT LAYOUT. This used to request boot protocol only
    // when the device declared HID_SUBCLASS_BOOT_INTERFACE, and then parse EVERY
    // mouse report as boot's [buttons][dx:int8][dy:int8] regardless. Plenty of
    // modern mice do not declare that subclass - they are report-protocol only -
    // so they were never switched, sent their native report, and had it read
    // with the wrong field positions.
    //
    // Kevin KW6E's Microsoft Surface Arc Mouse: "the pointer moves oddly -
    // rapidly moving between multiple points... speed and acceleration VERY HIGH
    // in the horizontal direction and VERY LOW in the vertical". That is the
    // signature of a 16-bit X read as 8-bit: our dx becomes X's LOW byte, which
    // wraps every 256 counts, and our dy becomes X's HIGH byte, which is nearly
    // always 0. It is not a sensitivity problem, and a speed setting would only
    // have hidden it - a scale factor cannot make one axis wrap and the other
    // stand still.
    //
    // So: still ask for boot protocol when the device offers it (it is the
    // simplest correct thing), but ALWAYS read the Report Descriptor and let it
    // say where the fields are. Same fix, and the same parser, as the BLE mouse
    // needed for the same reason.
    memset(&s_layout, 0, sizeof(s_layout));
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE)
        hid_class_request_set_protocol(hdl, HID_REPORT_PROTOCOL_BOOT);

    if (p.proto == HID_PROTOCOL_MOUSE) {
        size_t dlen = 0;
        uint8_t *desc = hid_host_get_report_descriptor(hdl, &dlen);
        if (desc && dlen && hid_report_map_parse(desc, dlen, &s_layout) && s_layout.valid) {
            ESP_LOGI(TAG, "report map: id=%u  X @bit%u/%ub  Y @bit%u/%ub  wheel=%s  payload=%ub",
                     s_layout.report_id, s_layout.x_bit, s_layout.x_bits,
                     s_layout.y_bit, s_layout.y_bits,
                     s_layout.have_wheel ? "yes" : "no", s_layout.total_bits);
        } else {
            ESP_LOGW(TAG, "could not parse the report map (%u bytes) - assuming the "
                          "boot 3-byte layout", (unsigned)dlen);
        }
    }

    e = hid_host_device_start(hdl);
    if (e != ESP_OK) { ESP_LOGE(TAG, "device_start failed: %s", esp_err_to_name(e)); return; }

    if (p.proto == HID_PROTOCOL_MOUSE) {
        s_present = true;
        hid_cursor_set_present(HID_CURSOR_SRC_USB, true);
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
