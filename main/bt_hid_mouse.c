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
//
// WITH bt_mouse_en OFF THE COST IS ZERO, and this was checked rather than
// assumed. I spent an evening warning that CONFIG_BT_ENABLED changes the
// host<->C6 handshake for every user, on the strength of a boot line reading
// "vhci_drv: Host BT Support: Enabled". It does not:
//   * hci_drv_init() for VHCI is literally an empty function;
//   * hci_drv_show_configuration() is two ESP_LOGI calls;
//   * hci_rx_handler() only runs when HCI data arrives, and none does unless
//     NimBLE is started and sends a command first;
//   * the "capabilities: 0xd / HCI over SDIO / BLE only" report appears in
//     NON-BT builds too - verified against a v1.6.0-era boot log. That is the
//     C6 announcing its own features, unprompted.
// So the only difference for a user who never ticks the box is ~71 KB of
// dormant flash. Read the function before inferring behaviour from its log.

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

#include "hid_report_map.h"  // parse the device's own report descriptor
#include "hid_keycode.h"     // #273: HID usage -> the token ui.c already takes
#include "ui/ui.h"           // ui_kbd_feed

static const char *TAG = "btmouse";

// HID over GATT. A mouse advertises this in its service-UUID list; it is what
// separates "a mouse" from every phone and earbud in a hotel in Taipei.
#define BLE_SVC_HID_UUID16 0x1812

// The layout of this mouse's movement reports, parsed from its own HID Report Map
// rather than assumed. Cleared on every disconnect: the next mouse to connect may
// be a different model, and a stale layout is worse than no layout because it looks
// authoritative. See hid_report_map.h for the whole story.
static hid_mouse_layout_t s_layout;
static hid_kbd_layout_t   s_kbd_layout;   /* #273 - same descriptor, keyboard half */
/* The report map is assembled here across the chunks of a long read. 512 is
 * comfortably more than any HID descriptor this device will meet; a longer one
 * is truncated and will simply fail to parse, which is loud rather than silent. */
static uint8_t  s_rmap[512];
static uint16_t s_rmap_len;
static bool     s_rmap_reading;   /* true from the long read starting to it finishing */

/* Reports that arrive BEFORE the descriptor has been parsed (#273).
 *
 * Measured on a K380: CONNECTED at t=79.5 s, report map parsed at t=82.0 s -
 * two and a half seconds during which notifications are already flowing and
 * nothing knows what they mean. Keys pressed in that window were decoded as
 * mouse movement and lost, which is the "first two or three keystrokes go
 * missing" the operator hit every time the keyboard woke from sleep.
 *
 * Eight is generous for human typing in 2.5 s and costs 144 bytes. */
#define BT_PEND_MAX 8
#define BT_PEND_LEN 16
static uint8_t  s_pend[BT_PEND_MAX][BT_PEND_LEN];
static uint8_t  s_pend_len[BT_PEND_MAX];
static uint8_t  s_pend_n;
static void hid_report_map_ready(const uint8_t *desc, uint16_t n);
static void bt_replay_pending(void);
static void bt_kbd_handle(const uint8_t *p, int plen);

static bool s_started;
// The setting as it was at boot - see bt_hid_mouse_init(). Not the same fact
// as s_started, and the difference between the two is what "a restart is
// pending" means.
static bool s_boot_en;
static int  s_seen;          // devices reported this scan, for a one-line summary
// Per OPEN scan window, not per boot. High enough to see a whole room -
// the point of the open window is to find a device you are holding.
#define SCAN_LOG_MAX 40
static bool s_connecting;
static int64_t s_connect_us;      // when s_connecting was set; see the stuck guard
#define CONNECT_STUCK_US (20LL * 1000000)
static bool s_connected;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle;
static bool     s_hid_connected;   /* #273 - drives ui_osk_show() */
static int64_t  s_scan_burst_until; /* scan hard until this time after a drop */

/* ⚡ A BOUNDED BURST AFTER A DISCONNECT, and only then.
 *
 * A Logitech K380 hangs up after ~30 s idle (reason 531 = remote user
 * terminated) and comes back when a key is pressed. Measured on this bench:
 * disconnect at t=70.9 s, rediscovered at t=78.9 s - EIGHT SECONDS, nearly all
 * of it ours, because a 20% duty cycle listens for 20 ms in every 100 and a
 * keyboard advertising for a short window is easy to miss.
 *
 * ⛔ The 20% is NOT a number to raise globally. It was chosen after measurement:
 * continuous scanning starved the SDIO link shared with WiFi, and three cheaper
 * suspects were tested and cleared before landing on it. What is different here
 * is that we have just LOST a bonded device, so we know something is about to
 * advertise and we know roughly when. Listening hard for a short, bounded window
 * costs airtime once per disconnect rather than continuously.
 *
 * ⚠ WATCH THIS if WiFi or audio ever misbehaves right after a keyboard wakes:
 * this burst is the only place scanning got more aggressive. */
#define SCAN_BURST_ITVL_UNITS  64    // x0.625 ms = 40 ms between windows
#define SCAN_BURST_WIN_UNITS   48    // x0.625 ms = 30 ms listening -> 75% duty
#define SCAN_BURST_MS       20000    // how long the burst lasts after a disconnect

static int64_t  s_enc_us;      // esp_timer at encryption, for measuring the idle drop
static int      s_cccd_writes; // CCCDs we asked to enable
static int      s_cccd_done;   // CCCDs that answered
static uint16_t s_hid_start, s_hid_end;  // HID service handle span

// HID input reports arrive on notifications from the Report characteristic
// (0x2A4D). Discovering the full HID service, parsing its report map and
// picking the right report ID is what esp_hid does - but its NimBLE host path
// is a large dependency for one mouse, and the report we actually want is the
// boot-protocol mouse report, whose layout is fixed. So we subscribe to every
// notification and decode the ones that look like a mouse report.
#define UUID_HID_REPORT 0x2A4D
// The two characteristics a HOGP host is expected to READ during setup. We
// have never read either, and a peripheral that drops any host after exactly
// 30 s regardless of traffic behaves like one that does not consider the host
// set up. Untried candidate for that; see the header comment.
#define UUID_HID_REPORT_MAP  0x2A4B
#define UUID_HID_INFORMATION 0x2A4A
// Protocol Mode: 0 = Boot, 1 = Report. A HOGP host is expected to set this.
#define UUID_HID_PROTOCOL_MODE 0x2A4E
#define HID_PROTOCOL_MODE_REPORT 0x01

// Which half of the scan cycle we are in - filtered to the bonded mouse, or
// open so a new one can be found. Declared here because gap_event_cb flips it
// on DISC_COMPLETE, well before start_scan() is defined. See start_scan().
static bool s_scan_wl_phase = true;

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
    // The scan now runs in timed stretches instead of BLE_HS_FOREVER, so this
    // event is what keeps it alive - without it scanning simply stops after the
    // first stretch and the mouse never reconnects. Flip between the filtered
    // and open phases here.
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (!s_connecting && !s_connected) {
            s_scan_wl_phase = !s_scan_wl_phase;
            start_scan();
        }
        return 0;
    }
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
    if (hid || s_seen <= SCAN_LOG_MAX) {
        const uint8_t *a = event->disc.addr.val;
        ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x rssi=%d %s",
                 hid ? "HID DEVICE:" : "  seen:",
                 a[5], a[4], a[3], a[2], a[1], a[0], event->disc.rssi,
                 name[0] ? name : "(no name)");
    }
    // A connect attempt that neither succeeds nor reports failure would strand
    // s_connecting and make the mouse permanently undiscoverable. One such path
    // is fixed above (DISCONNECT), but the guard is the class of bug, not that
    // one instance: ble_gap_connect() is given a 10 s timeout, so anything
    // still "in progress" well past that is not coming back.
    if (s_connecting && s_connect_us &&
        (esp_timer_get_time() - s_connect_us) > CONNECT_STUCK_US) {
        ESP_LOGW(TAG, "connect attempt stuck for >%d s - clearing and retrying",
                 (int)(CONNECT_STUCK_US / 1000000));
        s_connecting = false;
    }
    if (hid && !s_connecting && !s_connected) {
        ESP_LOGW(TAG, "HID device found - connecting");
        s_connecting = true;
        s_connect_us = esp_timer_get_time();
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
        s_hid_connected = true;
        ESP_LOGW(TAG, "CONNECTED to HID device (handle %u)", s_conn_handle);
        /* ⛔ NOT here. Connecting is not the same as being usable, and the
         * bottom-bar Bluetooth glyph is driven by this flag: it went blue
         * within half a second of a keypress while the keyboard could not type
         * for another ~2.5 s, because the descriptor had not been read yet.
         * The operator spotted the discrepancy and asked what the icon meant.
         *
         * It is also wrong in its own right for a keyboard: a keyboard-only
         * device would put a MOUSE POINTER on screen just by connecting.
         *
         * Presence is now claimed where it is earned - in hid_report_map_ready()
         * once a mouse layout is known, or on the first report that actually
         * decodes as movement (the fallback path, for a descriptor we cannot
         * parse). */
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
        s_hid_connected = false;
        s_scan_burst_until = esp_timer_get_time() + (int64_t)SCAN_BURST_MS * 1000;
        s_pend_n = 0;
        s_rmap_reading = false;
        s_kbd_layout.valid = false;   /* the next device may not be a keyboard */
        // Seconds since encryption, because that is what the 30 s drop is
        // measured from - a bare "disconnected" line cost several rounds of
        // eyeballing timestamps by hand.
        ESP_LOGW(TAG, "HID device disconnected (reason %d) after %lld s connected - scanning again",
                 event->disconnect.reason,
                 s_enc_us ? (long long)((esp_timer_get_time() - s_enc_us) / 1000000) : -1);
        s_enc_us = 0;
        s_connected = false;
        // Forget the report layout: the next mouse to connect may be a different
        // model, and a stale layout is worse than none because it looks
        // authoritative. It is re-read from the descriptor on every connection.
        s_layout.valid = false;
        // Clear the IN-PROGRESS flag too. A connection attempt that fails to
        // establish (HCI 0x3E, reason 574 here) arrives as DISCONNECT, NOT as
        // CONNECT-with-error - so this path used to leave s_connecting set
        // forever, and every later sighting of the mouse hit the
        // "!s_connecting" guard in gap_event_cb and was silently ignored. The
        // mouse then never reconnected until the Tab5 was rebooted.
        //
        // Caught 2026-08-10 from the operator losing the cursor: the log shows
        // the scan finding it ("HID DEVICE: ... rssi=-62") with no "connecting"
        // line after it, again and again. The "-1 s connected" above is the
        // same event seen from the other side - s_enc_us was never set, so it
        // was never really connected.
        s_connecting = false;
        hid_cursor_set_present(HID_CURSOR_SRC_BLE, false);
        start_scan();
        return 0;

    default:
        return 0;
    }
}

// Scanning cost the WiFi link, and that is not a theory - it was measured.
//
// 2026-08-10, root cause of TODO #109 (web UI flapping Disconnected/Connected
// for days): WiFi and Bluetooth are multiplexed over the SAME esp_hosted SDIO
// transport, and this scan - FOREVER, ACTIVE, no whitelist, continuous - had the
// C6 forwarding every advertiser in an office to the host. NimBLE dropped
// ~3,000 ADV reports a MINUTE for OOM, and the pending-byte delta kept
// overrunning the 1536-byte SDIO RX buffer. Measured, same firmware, same
// network, everything else identical:
//
//     BT on : 5.0 SDIO recoveries/min, 4-8 rpc timeouts/min
//     BT off: 0 and 0 over 6.9 minutes (~34 recoveries expected)
//
// Three cheaper suspects were tested and cleared first (the spot feeds, the
// 10 fps spectrum WebSocket, and the hotel network it was originally blamed on).
//
// So: scan less, and let the CONTROLLER throw away what we do not want.
#define SCAN_ITVL_UNITS   160     // x0.625 ms = 100 ms between scan windows
#define SCAN_WIN_UNITS     32     // x0.625 ms =  20 ms of listening -> 20% duty
#define SCAN_WL_MS      60000     // stretch spent filtered to the bonded mouse
#define SCAN_OPEN_MS    15000     // stretch spent open, so a new mouse is findable


// Put the bonded peers on the controller whitelist. Returns how many, so the
// caller can fall back to an open scan when there is nothing to filter to.
static int whitelist_bonded(void)
{
    ble_addr_t peers[8];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, (int)(sizeof(peers) / sizeof(peers[0]))) != 0)
        return 0;
    if (n <= 0) return 0;
    if (ble_gap_wl_set(peers, (uint8_t)n) != 0) {
        ESP_LOGW(TAG, "whitelist set failed - scanning open");
        return 0;
    }
    return n;
}

static void start_scan(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    s_own_addr_type = own_addr_type;

    // ALTERNATE rather than commit to the whitelist. A mouse that advertises
    // with a resolvable private address the controller cannot match would be
    // filtered out permanently, and "the mouse silently never reconnects again"
    // is a far worse bug than the one being fixed. So we spend SCAN_WL_MS
    // filtered and SCAN_OPEN_MS open, which keeps ~80% of the airtime quiet
    // while guaranteeing the mouse is still discoverable either way.
    int wl = s_scan_wl_phase ? whitelist_bonded() : 0;

    bool burst = wl && esp_timer_get_time() < s_scan_burst_until;

    struct ble_gap_disc_params p = {0};
    p.itvl          = burst ? SCAN_BURST_ITVL_UNITS : SCAN_ITVL_UNITS;
    p.window        = burst ? SCAN_BURST_WIN_UNITS  : SCAN_WIN_UNITS;
    p.limited       = 0;
    p.filter_duplicates = 1;
    // Filtered stretch: the controller drops non-bonded adverts before they ever
    // cross SDIO, and passive scanning skips the scan REQUEST/RESPONSE exchange
    // entirely - we already know who this is, we do not need its name.
    // Open stretch: active, because pairing needs the scan response to show the
    // operator a name rather than a bare address.
    p.filter_policy = wl ? BLE_HCI_SCAN_FILT_USE_WL : BLE_HCI_SCAN_FILT_NO_WL;
    p.passive       = wl ? 1 : 0;

    int32_t dur = wl ? SCAN_WL_MS : SCAN_OPEN_MS;
    if (burst) dur = SCAN_BURST_MS;   /* then the normal cycle resumes by itself */
    int rc = ble_gap_disc(own_addr_type, dur, &p, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return;
    }
    if (wl) ESP_LOGI(TAG, "scanning (filtered to %d bonded device(s), passive, %d%% duty%s)",
                     wl, (p.window * 100) / p.itvl, burst ? " - BURST after a drop" : "");
    else {
        // Reset the per-scan log budget so EVERY open window reports what it
        // found, not only the first one after boot. The old counter was
        // cumulative, so from the ninth device onward the log showed HID
        // devices and nothing else - which makes "my keyboard does not appear"
        // indistinguishable from "my keyboard does not advertise HID", and
        // those need completely different answers.
        s_seen = 0;
        ESP_LOGI(TAG, "scanning open for %d s - turn the mouse on / put it in pairing mode",
                 (int)(SCAN_OPEN_MS / 1000));
    }
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

// Read-and-log the descriptive characteristics. A HOGP host reads these during
// setup; we never did. Purely a read - it changes nothing on the peripheral -
// so it is safe to try even though it is a hypothesis for the 30 s drop.
static int hid_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    const char *what = (const char *)arg;
    bool is_map = (what && strcmp(what, "Report Map") == 0);

    /* ⛔ THE REPORT MAP NEEDS A *LONG* READ, AND THIS IS WHY.
     *
     * A plain ble_gattc_read() returns one ATT_MTU-1 payload - 22 bytes on this
     * link - and a HID report map is far longer. A Logitech K380 arrived cut off
     * mid-descriptor, immediately before its first Input item:
     *
     *   05 01 09 06 a1 01 85 01 95 08 75 01 15 00 25 01 05 07 19 e0 29 e7
     *   Usage(Keyboard) ... Usage Min/Max(E0-E7)   <- ends exactly here, 22 bytes
     *
     * So the parser saw a keyboard collection with NO fields in it and correctly
     * concluded there was no keyboard. Every BLE keyboard would have failed the
     * same way. ble_gattc_read_long() issues Read Blob requests and calls this
     * back once per chunk, then a final time with BLE_HS_EDONE.
     *
     * ⚠ Safe against the failure that got the previous attempt at this reverted:
     * the subscription walk is chained off the *discovery* EDONE in
     * hid_info_chr_cb(), NOT off this read completing - so a read that now takes
     * several round trips cannot stall it. */
    if (is_map) {
        if (error && error->status == BLE_HS_EDONE) {
            ESP_LOGI(TAG, "Report Map read OK (%u bytes, long read)",
                     (unsigned)s_rmap_len);
            s_rmap_reading = false;
            hid_report_map_ready(s_rmap, s_rmap_len);
            bt_replay_pending();
            return 0;
        }
        if (!error || error->status != 0 || !attr || !attr->om) {
            ESP_LOGW(TAG, "Report Map read failed: status=%d",
                     error ? error->status : -1);
            s_rmap_reading = false;
            s_pend_n = 0;            /* nothing can be decoded without a layout */
            return 0;
        }
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (n > (uint16_t)(sizeof s_rmap - s_rmap_len))
            n = (uint16_t)(sizeof s_rmap - s_rmap_len);
        if (n && os_mbuf_copydata(attr->om, 0, n, s_rmap + s_rmap_len) == 0)
            s_rmap_len = (uint16_t)(s_rmap_len + n);
        return 0;
    }

    if (!error || error->status != 0 || !attr || !attr->om) {
        ESP_LOGW(TAG, "%s read failed: status=%d", what, error ? error->status : -1);
        return 0;
    }
    ESP_LOGI(TAG, "%s read OK (%u bytes)", what, OS_MBUF_PKTLEN(attr->om));

    // THE REPORT MAP IS THE ANSWER, AND WE USED TO THROW IT AWAY.
    //
    // This handler logged the length and dropped the bytes, while handle_report()
    // guessed the layout from one hardware capture. The descriptor states it: which
    // report ID carries movement, and at what bit offset and width X, Y and the
    return 0;
}

/* Is a Bluetooth KEYBOARD connected right now?
 *
 * Both halves matter: a device is connected AND its report map declared a
 * keyboard. A BLE mouse answers false, so the on-screen keyboard still appears
 * for someone using a mouse and no keyboard. Cleared on disconnect, so a
 * keyboard going flat or out of range brings the on-screen one straight back. */
bool bt_hid_keyboard_active(void)
{
    return s_hid_connected && s_kbd_layout.valid;
}

/* Parse a COMPLETE report map. Split out of hid_read_cb so the long read can
 * assemble the descriptor across several chunks and hand it over once. */
static void hid_report_map_ready(const uint8_t *desc, uint16_t n)
{
        {
            // Log it as hex regardless of whether the parse succeeds: on a mouse
            // this does not handle, that dump in a field diag log is the only way
            // to find out why - and it is read ONCE per connection, not per report.
            char hex[3 * 64 + 8];
            int  used = 0;
            for (uint16_t i = 0; i < n && used < (int)sizeof hex - 4; i++)
                used += snprintf(hex + used, sizeof hex - used, "%02x ", desc[i]);
            ESP_LOGI(TAG, "report map [%u]: %s%s", n, hex, n > 64 ? "..." : "");

            hid_mouse_layout_t L;
            if (hid_report_map_parse(desc, n, &L)) {
                s_layout = L;
                hid_cursor_set_present(HID_CURSOR_SRC_BLE, true);
                ESP_LOGI(TAG, "report layout: id=%u  X @bit%u/%ub  Y @bit%u/%ub  "
                              "wheel %s  payload %u bits",
                         (unsigned)L.report_id, (unsigned)L.x_bit, (unsigned)L.x_bits,
                         (unsigned)L.y_bit, (unsigned)L.y_bits,
                         L.have_wheel ? "yes" : "no", (unsigned)L.total_bits);
            } else {
                ESP_LOGW(TAG, "report map not understood - falling back to the "
                              "fixed layouts; send the hex above if the pointer "
                              "misbehaves");
            }

            /* The SAME descriptor, walked again for a keyboard (#273). A plain
             * keyboard has no mouse fields at all, so hid_report_map_parse()
             * above fails on it and, without this, its keystrokes would fall
             * through to the mouse FALLBACK and be decoded as pointer movement.
             * A combo keyboard/touchpad declares both and gets both. */
            hid_kbd_layout_t K;
            if (hid_report_map_parse_keyboard(desc, n, &K)) {
                s_kbd_layout = K;
                ESP_LOGI(TAG, "keyboard layout: id=%u  mods @bit%u/%ub  "
                              "%u key slot(s) @bit%u/%ub",
                         (unsigned)K.report_id, (unsigned)K.mod_bit,
                         (unsigned)K.mod_bits, (unsigned)K.key_count,
                         (unsigned)K.key_bit, (unsigned)K.key_bits);
            } else {
                s_kbd_layout.valid = false;
                ESP_LOGI(TAG, "no keyboard in this report map (mouse-only device)");
            }
        }
}

static int hid_info_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    const char *what = (const char *)arg;
    // status BLE_HS_EDONE marks the END of a discovery procedure - that is the
    // only safe moment to start the next one.
    if (error && error->status == BLE_HS_EDONE) {
        int rc;
        if (what && strcmp(what, "HID Information") == 0) {
            rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_hid_start, s_hid_end,
                                             BLE_UUID16_DECLARE(UUID_HID_REPORT_MAP),
                                             hid_info_chr_cb, (void *)"Report Map");
            if (rc != 0) ESP_LOGW(TAG, "Report Map discovery failed to start: %d", rc);
        } else {
            rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_hid_start, s_hid_end,
                                             BLE_UUID16_DECLARE(UUID_HID_REPORT),
                                             hid_chr_cb, NULL);
            if (rc != 0) ESP_LOGW(TAG, "Report discovery failed to start: %d", rc);
        }
        return 0;
    }
    if (!error || error->status != 0 || !chr) return 0;
    if (what && strcmp(what, "Report Map") == 0) {
        s_rmap_len = 0;                      /* fresh descriptor per connection */
        s_rmap_reading = true;
        s_pend_n = 0;
        int rc = ble_gattc_read_long(conn_handle, chr->val_handle, 0,
                                     hid_read_cb, arg);
        if (rc != 0) ESP_LOGW(TAG, "Report Map long read failed to start: %d", rc);
    } else {
        ble_gattc_read(conn_handle, chr->val_handle, hid_read_cb, arg);
    }
    return 0;
}

static int hid_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (!error || error->status != 0 || !svc) return 0;
    ESP_LOGI(TAG, "HID service at handles %u-%u", svc->start_handle, svc->end_handle);
    s_hid_start = svc->start_handle;
    s_hid_end   = svc->end_handle;
    // ONE GATT PROCEDURE AT A TIME. NimBLE rejects a second concurrent
    // procedure on the same connection with BLE_HS_EALREADY, and I stacked
    // three of these back-to-back - so the 2nd and 3rd were silently dropped
    // and that connection ended up with NO notifications enabled at all.
    // Chain them through the callbacks instead, and CHECK the return code.
    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_hid_start, s_hid_end,
                                         BLE_UUID16_DECLARE(UUID_HID_INFORMATION),
                                         hid_info_chr_cb, (void *)"HID Information");
    if (rc != 0) ESP_LOGW(TAG, "HID Information discovery failed to start: %d", rc);
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
/* ---- Bluetooth keyboard (#273) -------------------------------------------
 *
 * The two halves either side of this were already written and host-tested and
 * neither was ever called: hid_report_map_parse_keyboard() reads the layout out
 * of the device's own descriptor, and hid_keycode_translate() turns a usage into
 * exactly the token ui.c's keyboard bridge already takes. This is the glue.
 *
 * A HID keyboard reports the set of keys CURRENTLY HELD, not the key that just
 * changed - so typing is the difference between successive reports. Without the
 * edge detection below, holding a key for 300 ms would type it once per
 * notification, which on a BLE keyboard is a stream of repeats nobody asked for.
 * Auto-repeat, if it is ever wanted, belongs here as a deliberate timer rather
 * than as a side effect of the wire format. */
#define BT_KBD_MAX_SLOTS 8
static uint8_t s_kbd_prev[BT_KBD_MAX_SLOTS];
static uint8_t s_kbd_prev_n;

/* Does this report belong to the keyboard, and if so type it. Returns true when
 * it was consumed. ONE implementation, used for live reports and for replayed
 * ones, so a keystroke recovered from the connect window cannot be decoded
 * differently from one typed a second later. */
static bool bt_kbd_try(const void *d, int len)
{
    if (!s_kbd_layout.valid) return false;
    const uint8_t *p = (const uint8_t *)d;

    /* Payload size the DESCRIPTOR declares, in bytes. A K380 says mods @bit0/8b
     * + 6 slots @bit8/8b = 56 bits = 7, and it really does send 7 - one short of
     * the classic boot layout, because it has no reserved byte. Derived, never
     * assumed.
     *
     * ⛔ AND THE REPORT ID IS NOT ON THE WIRE ON BLE. The descriptor can declare
     * "Report ID 1" - this one does - while the payload begins 00 18 00 ...,
     * because each report has its own characteristic and the ID lives in its
     * Report Reference descriptor. Matching p[0] against the ID can never
     * succeed here, which is why the length is what identifies the report. The
     * ID form is still tried first, for a transport that does put it on the
     * wire. */
    int want = (s_kbd_layout.key_bit +
                (int)s_kbd_layout.key_count * s_kbd_layout.key_bits + 7) / 8;

    if (s_kbd_layout.report_id != 0 && len == want + 1 &&
        p[0] == s_kbd_layout.report_id) {
        bt_kbd_handle(p + 1, len - 1);
        return true;
    }
    if (len == want) {
        bt_kbd_handle(p, len);
        return true;
    }
    return false;   /* not the keyboard - on a combo device this is the mouse */
}

/* Type anything that arrived while the descriptor was still being read. */
static void bt_replay_pending(void)
{
    uint8_t n = s_pend_n;
    s_pend_n = 0;
    if (!n) return;
    int typed = 0;
    for (uint8_t i = 0; i < n; i++)
        if (bt_kbd_try(s_pend[i], s_pend_len[i])) typed++;
    ESP_LOGI(TAG, "replayed %u report(s) buffered during connect, %d were keys",
             (unsigned)n, typed);
}

static void bt_kbd_handle(const uint8_t *p, int plen)
{
    const hid_kbd_layout_t *K = &s_kbd_layout;

    uint8_t mods = 0;
    if (K->mod_bits)
        mods = (uint8_t)hid_field_signed(p, (size_t)plen, K->mod_bit,
                                         K->mod_bits > 8 ? 8 : K->mod_bits);

    uint8_t now[BT_KBD_MAX_SLOTS];
    int n = K->key_count > BT_KBD_MAX_SLOTS ? BT_KBD_MAX_SLOTS : K->key_count;
    for (int k = 0; k < n; k++) {
        uint16_t bit = (uint16_t)(K->key_bit + (uint16_t)k * K->key_bits);
        now[k] = (uint8_t)hid_field_signed(p, (size_t)plen, bit,
                                           K->key_bits > 8 ? 8 : K->key_bits);
    }

    for (int k = 0; k < n; k++) {
        uint8_t u = now[k];
        if (u == 0) continue;
        /* Held since the last report - not a new press. */
        bool was_down = false;
        for (int j = 0; j < s_kbd_prev_n; j++)
            if (s_kbd_prev[j] == u) { was_down = true; break; }
        if (was_down) continue;

        hid_key_event_t ev;
        /* Refuses rollover (0x01-0x03), the modifier usages and anything with no
         * sensible text. A false return must be DROPPED, never typed: a keyboard
         * reporting rollover would otherwise spray characters. */
        if (!hid_keycode_translate(u, mods, &ev)) continue;
        ESP_LOGI(TAG, "key: usage=0x%02x mods=0x%02x -> '%s'",
                 (unsigned)u, (unsigned)mods, ev.text);
        ui_kbd_feed(ev.text, ev.mods);
    }

    for (int k = 0; k < n; k++) s_kbd_prev[k] = now[k];
    s_kbd_prev_n = (uint8_t)n;
}

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
    /* KEYBOARD FIRST (#273). A keyboard-only device declares no mouse fields, so
     * the mouse parse above failed and the fallback at the bottom of this
     * function would happily decode its keystrokes as pointer movement. Checking
     * the keyboard layout first is what stops that, and it is why this sits
     * ahead of the `len < 3` guard's mouse assumptions. */
    /* Still reading the descriptor? Keep the report. Nothing can be decoded
     * correctly yet - this is the 2.5 s window between CONNECT and the report
     * map arriving, and it is exactly when a keyboard waking from sleep sends
     * its first keystrokes. They are replayed the moment the layout is known.
     *
     * Deliberately kept AND NOT processed: decoding it as mouse movement in the
     * meantime would jog the pointer for every key typed. A mouse loses at most
     * a couple of hundred ms of movement at connect, which is not noticeable;
     * losing the first characters you type is. */
    if (s_rmap_reading) {
        if (s_pend_n < BT_PEND_MAX && len > 0) {
            int n = len > BT_PEND_LEN ? BT_PEND_LEN : len;
            for (int i = 0; i < n; i++) s_pend[s_pend_n][i] = ((const uint8_t *)d)[i];
            s_pend_len[s_pend_n] = (uint8_t)n;
            s_pend_n++;
        }
        return;
    }

    if (bt_kbd_try(d, len)) return;

    if (len < 3) return;


    // PREFERRED PATH: the layout this mouse declared in its own Report Map.
    //
    // Everything below is fallback for a descriptor we could not read or parse. It
    // is kept because it is known to work on at least one real mouse, but it is a
    // guess and this is not.
    if (s_layout.valid) {
        const uint8_t *p = (const uint8_t *)d;
        int plen = len;
        // A descriptor that declares a report ID means the byte is on the wire.
        // Reports for OTHER IDs (a consumer-control page, a battery report) share
        // this notification and must be ignored rather than decoded as movement.
        if (s_layout.report_id != 0) {
            if (plen < 1 || p[0] != s_layout.report_id) return;
            p++; plen--;
        }
        int x = hid_field_signed(p, (size_t)plen, s_layout.x_bit, s_layout.x_bits);
        int y = hid_field_signed(p, (size_t)plen, s_layout.y_bit, s_layout.y_bits);
        // Buttons are the low bits of the first byte on every mouse I have seen,
        // and the descriptor's button range is not parsed, so this stays as it was.
        hid_cursor_apply(x, y, p[0]);
        if (s_layout.have_wheel) {
            int w = hid_field_signed(p, (size_t)plen, s_layout.wheel_bit, s_layout.wheel_bits);
            if (w) hid_cursor_add_wheel(w);
        }
        return;
    }

    // No usable descriptor. hid_fallback_decode() picks between layouts that have
    // each been CAPTURED off real hardware, using the report length.
    //
    // This used to inline the Logitech M240's 12-bit packed layout and apply it to
    // ANY report of five bytes or more. Kevin KW6E's Microsoft Surface Arc sends
    // nine bytes with 16-bit movement, and his own diagnostic log showed what that
    // cost: his report 00 06 00 0b 00 ff ff 00 00 is X=+6 Y=+11, and the M240
    // arithmetic turned it into X=-1280 Y=0 - a large jump the wrong way with no
    // vertical movement, which is the "erratic pointer" he reported while
    // scrolling worked perfectly.
    hid_mouse_move_t mv;
    if (!hid_fallback_decode(d, (size_t)len, &mv)) return;
    /* A descriptor we could not parse, but this really did decode as movement -
     * so there IS a pointer here, and the glyph may say so. */
    hid_cursor_set_present(HID_CURSOR_SRC_BLE, true);
    hid_cursor_apply(mv.dx, mv.dy, mv.buttons);
    if (mv.wheel) hid_cursor_add_wheel(mv.wheel);
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
    // What the setting said when THIS session started, recorded before the
    // early return so it is captured whichever way the decision went. It is
    // the honest answer to "will the radio be on for the rest of this boot" -
    // s_started cannot answer that, because it stays false for the several
    // seconds NimBLE waits on the C6 link, and the drawer can be opened inside
    // that window (#270, Don N2VGU).
    s_boot_en = s.bt_mouse_en;
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
bool bt_hid_mouse_enabled_at_boot(void) { return s_boot_en; }
