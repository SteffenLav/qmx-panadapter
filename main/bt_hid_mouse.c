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
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "host/ble_gatt.h"
#include "store/config/ble_store_config.h"

#include <string.h>

static const char *TAG = "btmouse";

// HID over GATT. A mouse advertises this in its service-UUID list; it is what
// separates "a mouse" from every phone and earbud in a hotel in Taipei.
#define BLE_SVC_HID_UUID16 0x1812

static bool s_started;
static int  s_seen;          // devices reported this scan, for a one-line summary
static bool s_connecting;
static bool s_connected;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle;
static int64_t  s_enc_us;      // esp_timer at encryption, for measuring the idle drop
static int      s_cccd_writes; // CCCDs we asked to enable
static int      s_cccd_done;   // CCCDs that answered

// HID input reports arrive on notifications from the Report characteristic
// (0x2A4D). Discovering the full HID service, parsing its report map and
// picking the right report ID is what esp_hid does - but its NimBLE host path
// is a large dependency for one mouse, and the report we actually want is the
// boot-protocol mouse report, whose layout is fixed. So we subscribe to every
// notification and decode the ones that look like a mouse report.
#define UUID_HID_REPORT 0x2A4D
// Protocol Mode: 0 = Boot, 1 = Report. A HOGP host is expected to set this.
#define UUID_HID_PROTOCOL_MODE 0x2A4E
#define HID_PROTOCOL_MODE_REPORT 0x01

static void start_scan(void);
static int  conn_event_cb(struct ble_gap_event *event, void *arg);
static int  hid_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg);
static void handle_report(const uint8_t *d, int len);

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

// Have we bonded with this address before? NimBLE keeps the bond list in NVS
// (BT_NIMBLE_NVS_PERSIST), so this survives a reboot and a reflash - which is
// the point: the operator should pair the mouse ONCE, ever.
static bool addr_is_bonded(const ble_addr_t *addr)
{
    ble_addr_t peers[8];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, (int)(sizeof(peers) / sizeof(peers[0]))) != 0)
        return false;
    for (int i = 0; i < n; i++)
        if (memcmp(peers[i].val, addr->val, 6) == 0) return true;
    return false;
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
    // A device we have already bonded with is worth connecting to whatever its
    // advert says - and this is not an optimisation, it is the difference
    // between pairing once and pairing every time. A bonded peer reconnects
    // with DIRECTED advertising, which carries NO service UUID list, so
    // adv_has_hid() is false for the very mouse we just paired with. Matching
    // on the bond is the only thing that catches it.
    bool bonded = addr_is_bonded(&event->disc.addr);
    bool hid = bonded || adv_has_hid(&f);
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
    if (hid && !s_connecting && !s_connected) {
        ESP_LOGW(TAG, "HID device found - connecting");
        s_connecting = true;
        ble_gap_disc_cancel();              // cannot connect while scanning
        int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 10000,
                                 NULL, conn_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_connect failed: %d - resuming scan", rc);
            s_connecting = false;
            start_scan();
        }
    }
    return 0;
}

static int conn_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: status=%d - resuming scan", event->connect.status);
            start_scan();
            return 0;
        }
        s_connected   = true;
        s_conn_handle = event->connect.conn_handle;
        ESP_LOGW(TAG, "CONNECTED to HID device (handle %u)", s_conn_handle);
        hid_cursor_set_present(HID_CURSOR_SRC_BLE, true);
        // Ask for parameters a mouse can idle on. HYPOTHESIS for the ~30 s
        // drop, and labelled as one: the peripheral terminates the link
        // (reason 531 = HCI 0x13), so the lever is letting it SLEEP rather
        // than making it talk. latency=30 lets it skip 30 connection events
        // when it has nothing to send; the 6 s supervision timeout must stay
        // comfortably above latency x interval or the link drops the moment it
        // uses the latency we just granted.
        //   interval 15-30 ms, latency 30  -> up to ~0.9 s of silence allowed
        //   supervision 6 s                -> ~6x margin over that
        {
            struct ble_gap_upd_params p = {
                .itvl_min            = 12,    // x1.25 ms = 15 ms
                .itvl_max            = 24,    // x1.25 ms = 30 ms
                .latency             = 30,
                .supervision_timeout = 600,   // x10 ms = 6 s
                .min_ce_len          = 0,
                .max_ce_len          = 0,
            };
            int rc = ble_gap_update_params(s_conn_handle, &p);
            ESP_LOGI(TAG, "connection params requested (latency 30, 6 s timeout), rc=%d", rc);
        }
        // Pair/encrypt first: a HID peripheral will not send reports over an
        // unencrypted link, so skipping this gets a silent connection that
        // never moves the cursor.
        ble_gap_security_initiate(s_conn_handle);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s_enc_us = esp_timer_get_time();
        ESP_LOGW(TAG, "encryption change: status=%d - completing HOGP setup",
                 event->enc_change.status);
        // Subscribe to everything notifiable now that the link is encrypted.
        s_cccd_writes = 0;
        s_cccd_done   = 0;
        ble_gattc_disc_svc_by_uuid(s_conn_handle,
                                   BLE_UUID16_DECLARE(BLE_SVC_HID_UUID16),
                                   hid_svc_cb, NULL);
        // NO Protocol Mode write here. It was tried as a fix for the exactly-30 s
        // idle disconnect and FAILED on both counts, measured: the write itself
        // succeeded (handle 53, rc=0) yet the drop still came at 30335 ms and
        // 30386 ms after encryption, AND report notifications stopped arriving
        // entirely while it was in place. Switching the peripheral out of Boot
        // Protocol evidently moves reports somewhere our blanket CCCD
        // subscription does not catch. Do not re-add it without also handling
        // the Report Reference descriptors that Report mode implies.
        //
        // The 30 s disconnect therefore remains UNEXPLAINED. Evidence so far:
        // it is measured from the encryption-change event, not from the last
        // report, and it is far too repeatable to be a battery-saving timer.
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        // om is a chained mbuf; flatten the leading fragment, which for a HID
        // report is the whole thing.
        uint8_t buf[16];
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > (int)sizeof(buf)) len = sizeof(buf);
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL) == 0)
            handle_report(buf, len);
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE: {
        // What the peripheral actually AGREED to - a request is not a result,
        // and the whole 30 s question turns on whether it accepted latency.
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(s_conn_handle, &d) == 0)
            ESP_LOGW(TAG, "conn params now: itvl=%u (%u ms) latency=%u timeout=%u (%u ms) status=%d",
                     d.conn_itvl, (unsigned)(d.conn_itvl * 125 / 100),
                     d.conn_latency, d.supervision_timeout,
                     (unsigned)(d.supervision_timeout * 10), event->conn_update.status);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        // Seconds since encryption, because that is what the 30 s drop is
        // measured from - a bare "disconnected" line cost several rounds of
        // eyeballing timestamps by hand.
        ESP_LOGW(TAG, "HID device disconnected (reason %d) after %lld s connected - scanning again",
                 event->disconnect.reason,
                 s_enc_us ? (long long)((esp_timer_get_time() - s_enc_us) / 1000000) : -1);
        s_enc_us = 0;
        s_connected = false;
        hid_cursor_set_present(HID_CURSOR_SRC_BLE, false);
        start_scan();
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    s_own_addr_type = own_addr_type;
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

// Proper HOGP discovery: HID service -> its Report characteristics -> their
// CCCDs. NOT a blanket walk of the whole attribute table.
//
// The blanket version is what caused the infamous 30 s disconnect. It wrote to
// the first 0x2902 it found anywhere (handle 13, in a service that was not
// HID), the mouse never answered that write, and the BLUETOOTH SPEC'S ATT
// TRANSACTION TIMEOUT IS 30 SECONDS - on expiry the stack must tear the link
// down. Hence a disconnect that was always exactly 30 s after GATT setup
// began, on every connection, which looked for all the world like a
// peripheral power-saving timer and was nothing of the sort.
//   CCCD 1/1 answered at t+30133 ms, status=13   (13 = BLE_HS_ETIMEOUT)
// Never issue an ATT request to a handle you have not established is the
// right one.

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    s_cccd_done++;
    ESP_LOGI(TAG, "CCCD %d/%d answered at t+%lld ms, status=%d",
             s_cccd_done, s_cccd_writes,
             s_enc_us ? (long long)((esp_timer_get_time() - s_enc_us) / 1000) : -1,
             error ? error->status : 0);
    return 0;
}

// A Report characteristic's CCCD is the descriptor immediately following it,
// so search only the handle span belonging to THAT characteristic.
static int report_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    if (!error || error->status != 0 || !dsc) return 0;
    if (ble_uuid_u16(&dsc->uuid.u) != 0x2902) return 0;
    uint8_t val[2] = { 0x01, 0x00 };
    s_cccd_writes++;
    ESP_LOGI(TAG, "CCCD #%d at handle %u for report chr %u",
             s_cccd_writes, dsc->handle, chr_val_handle);
    int rc = ble_gattc_write_flat(conn_handle, dsc->handle, val, sizeof(val),
                                  cccd_write_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "CCCD write failed to start: %d", rc);
    return 0;
}

static int hid_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!error || error->status != 0 || !chr) return 0;
    if (!(chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) return 0;
    ESP_LOGI(TAG, "notifiable report chr at %u (props 0x%02x) - finding its CCCD",
             chr->val_handle, chr->properties);
    // Only this characteristic's own descriptor span.
    ble_gattc_disc_all_dscs(conn_handle, chr->val_handle,
                            chr->val_handle + 2, report_dsc_cb, NULL);
    return 0;
}

static int hid_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (!error || error->status != 0 || !svc) return 0;
    ESP_LOGI(TAG, "HID service at handles %u-%u", svc->start_handle, svc->end_handle);
    ble_gattc_disc_chrs_by_uuid(conn_handle, svc->start_handle, svc->end_handle,
                                BLE_UUID16_DECLARE(UUID_HID_REPORT),
                                hid_chr_cb, NULL);
    return 0;
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

// Decode one notification as a boot-protocol mouse report.
//
// Deliberately permissive about LENGTH and offset: BLE mice differ. Most send
// [buttons, dx, dy] with 8-bit deltas; some prefix a report ID; some use 12- or
// 16-bit deltas. Rather than guess, the RAW BYTES are logged at first sight so
// the real layout can be read off the hardware and this narrowed if needed.
static void handle_report(const uint8_t *d, int len)
{
    // Log the first few of ANY report, and ALWAYS log one whose wheel byte is
    // non-zero. The wheel is the thing under investigation and it is rare
    // compared to movement, so a plain first-N budget is spent on movement
    // before a single scroll is ever seen - which is exactly how the wheel
    // ended up undiagnosable on the previous build.
    static int logged = 0;
    bool wheelish = (len >= 6 && d[5] != 0);
    if (logged < 12 || wheelish) {
        logged++;
        char hex[64] = "";
        int n = 0;
        for (int i = 0; i < len && n < (int)sizeof(hex) - 4; i++)
            n += snprintf(hex + n, sizeof(hex) - n, "%02x ", d[i]);
        ESP_LOGI(TAG, "report[%d]: %s", len, hex);
    }
    if (len < 3) return;

    if (len >= 5) {
        // 12-BIT PACKED report - what a Logitech M240 actually sends, captured
        // on hardware 2026-08-09:
        //     00 00 d9 0f fd 00 00   ->  X=-39  Y=-48
        //     00 00 02 e0 ff 00 00   ->  X=+2   Y=-2
        // Layout: [buttons][.][X lo 8][X hi 4 | Y lo 4][Y hi 8][wheel][pan]
        // X and Y SHARE byte 3 - a nibble each - which is why reading this as
        // a 3-byte boot report gave a permanently-zero dx and fed the X value
        // into dy, so moving the mouse sideways moved the cursor vertically.
        int x = d[2] | ((d[3] & 0x0F) << 8);
        int y = ((d[3] >> 4) & 0x0F) | (d[4] << 4);
        if (x & 0x800) x -= 0x1000;      // 12-bit two's complement
        if (y & 0x800) y -= 0x1000;
        hid_cursor_apply(x, y, d[0]);
        // Byte 5 is the wheel: 00 in every movement-only report captured, ff
        // (= -1) in the one where the wheel was turned. Byte 6 is the
        // horizontal/pan wheel, which nothing in this UI scrolls sideways.
        if (len >= 6 && (int8_t)d[5]) hid_cursor_add_wheel((int8_t)d[5]);
        return;
    }

    // Boot-protocol layout (3-4 bytes): [buttons][dx int8][dy int8][wheel].
    hid_cursor_apply((int8_t)d[1], (int8_t)d[2], d[0]);
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
    ESP_LOGI(TAG, "BLE mouse up - scanning, will pair and connect automatically");
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
