// BLE HID mouse - Stage 1: bring the stack up and SCAN.
//
// Why a mouse over Bluetooth at all: the QMX owns the ESP32-P4's single USB
// host. A USB mouse therefore needs a hub, and a hub puts both devices behind
// a Transaction Translator that ESP-IDF 5.4.4 does not implement - hardware
// -proven, both ports get disabled (see CLAUDE.md, "USB mouse"). BLE touches
// none of that, so it is the only way to have a pointer AND the radio at once,
// which is the case that actually matters in the field.
//
// The controller is on the C6 and HCI rides the SAME SDIO link as WiFi. That
// link is this board's most fragile component by a wide margin (the whole
// esp_hosted wedge history in CLAUDE.md), so STAGE 1 DELIBERATELY STOPS AT
// SCANNING: bring up NimBLE, scan, log what is nearby, and let a soak answer
// "does BLE coexist with WiFi here?" BEFORE any pairing or report-handling
// code exists to muddy the question. Connecting is Stage 2.
//
// Static cost measured before writing a line of this: enabling BT moved .bss
// by +96 bytes (156,780 -> 156,876), because the controller is not ours and
// NimBLE takes its pools from the heap. The runtime heap cost is the real
// number and is logged at init below.

#include "bt_hid_mouse.h"
#include "hid_cursor.h"
#include "storage/settings.h"
#include "wifi/wifi.h"          // wifi_is_connected() - the C6 link must be up first
#include "util/psram_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include <string.h>

static const char *TAG = "btmouse";

// HID over GATT. A mouse advertises this in its service-UUID list; it is what
// separates "a mouse" from every phone and earbud in a hotel in Taipei.
#define BLE_SVC_HID_UUID16 0x1812

static bool s_started;
static int  s_seen;          // devices reported this scan, for a one-line summary

static void log_heap(const char *when)
{
    // O(1) queries only. A largest-free-block walk here would be a full heap
    // walk with interrupts off - the cyan-flash rule in CLAUDE.md.
    ESP_LOGI(TAG, "heap %s: internal free=%u B  dma free=%u B  psram free=%u KB",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// Does this advertisement claim HID? Checked against the 16-bit service UUID
// lists; a mouse may put it in either the complete or incomplete list.
static bool adv_has_hid(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++)
        if (ble_uuid_u16(&f->uuids16[i].u) == BLE_SVC_HID_UUID16) return true;
    return false;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0) return 0;

    char name[32] = "";
    if (f.name && f.name_len) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = '\0';
    }
    bool hid = adv_has_hid(&f);
    s_seen++;

    // Log HID devices always; everything else only in the first few, so a busy
    // hotel does not flood the diag ring with earbuds.
    if (hid || s_seen <= 8) {
        const uint8_t *a = event->disc.addr.val;
        ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x rssi=%d %s",
                 hid ? "HID DEVICE:" : "  seen:",
                 a[5], a[4], a[3], a[2], a[1], a[0], event->disc.rssi,
                 name[0] ? name : "(no name)");
    }
    if (hid) {
        ESP_LOGW(TAG, "^ that is a HID device - Stage 2 will pair with it. "
                      "Stage 1 only scans.");
    }
    return 0;
}

static void start_scan(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    struct ble_gap_disc_params p = {0};
    p.itvl          = 0;      // stack defaults
    p.window        = 0;
    p.filter_policy = 0;
    p.limited       = 0;
    p.passive       = 0;      // active: ask for the scan response, which carries the name
    p.filter_duplicates = 1;

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    else         ESP_LOGI(TAG, "scanning for BLE devices (turn the mouse on / put it in pairing mode)");
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "BLE host synced with the C6 controller");
    log_heap("after sync");
    start_scan();
}

static void on_reset(int reason)
{
    // Worth a warning rather than a silent retry: on this board a controller
    // reset usually means the SDIO link to the C6 hiccuped, which is exactly
    // what this stage exists to find out about.
    ESP_LOGW(TAG, "BLE controller reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// Bring NimBLE up only once the hosted transport to the C6 is actually alive.
//
// This is not belt-and-braces, it is the whole reason the first attempt failed.
// HCI rides the esp_hosted SDIO transport, and that transport is not ready
// until ~6 s into boot. Calling nimble_port_init() from app_main alongside the
// other network inits started the BLE host at ~4.8 s, a second BEFORE
// "transport: Base transport is set-up" - so the host began issuing HCI
// commands into a VHCI with no transport under it, and the link died with
// "Not able to connect with ESP-Hosted slave device" on a ~4 s timeout, taking
// WiFi down with it. Memory was never involved: 99 KB internal was free at the
// moment of the failed init.
//
// Waiting for an IP is a deliberately conservative proxy for "the C6 link is
// healthy and settled" rather than the earliest possible moment.
static void bt_start_task(void *arg)
{
    (void)arg;
    const int WAIT_S = 60;
    for (int i = 0; i < WAIT_S * 2 && !wifi_is_connected(); i++)
        vTaskDelay(pdMS_TO_TICKS(500));

    if (!wifi_is_connected()) {
        // No WiFi means either it is switched off or the link never came up.
        // Starting BLE anyway would be the same race that broke it before, so
        // wait a further settling period and try regardless - a WiFi-off
        // operator still deserves a mouse.
        ESP_LOGW(TAG, "no WiFi after %d s - starting BLE anyway after a settle", WAIT_S);
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
        vTaskDelay(pdMS_TO_TICKS(3000));   // let the link settle past DHCP/SNTP
    }

    log_heap("before NimBLE init");
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s - BLE unavailable this boot",
                 esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_NO_INPUT_OUTPUT;  // a mouse has no keypad
    ble_hs_cfg.sm_bonding      = 1;    // bond, so it reconnects by itself next time
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist  |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;

    // No ble_svc_gap_init() here on purpose: that registers a GATT SERVER
    // service and only links when the peripheral role is compiled in, which it
    // is not - we are central-only (we connect to a mouse, we never advertise).
    // Leaving the peripheral role out is most of why enabling Bluetooth cost
    // +96 bytes of .bss rather than tens of kilobytes.
    nimble_port_freertos_init(host_task);
    s_started = true;
    log_heap("after NimBLE init");
    ESP_LOGI(TAG, "BLE mouse Stage 1 up - scan only, no pairing yet");
    vTaskDelete(NULL);
}

void bt_hid_mouse_init(void)
{
    if (s_started) return;

    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.bt_mouse_en) {
        ESP_LOGI(TAG, "BLE mouse disabled in settings - stack not started");
        return;
    }
    // PSRAM stack, low priority: this task spends its life sleeping and must
    // not compete with the audio/FFT pipeline.
    psram_task_create(bt_start_task, "bt_start", 6144, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "BLE mouse enabled - waiting for the C6 link before starting NimBLE");
}

bool bt_hid_mouse_started(void) { return s_started; }
