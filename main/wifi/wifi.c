#include "wifi.h"
#include "settings.h"
#include <stdbool.h>

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_netif.h"        // wifi_netif_driver_t, esp_wifi_register_if_rxcb, ...
#include "esp_wifi_default.h"      // ESP_NETIF_INHERENT_DEFAULT_WIFI_STA, attach_wifi_station
#include "esp_private/wifi.h"      // esp_wifi_internal_reg_netstack_buf_cb, set_sta_ip
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "webserver.h"
#include "rigctld_server.h"
#include "time_sync.h"
#include "esp_timer.h"   // scan-hold timeout

static const char *TAG = "wifi";

// State -----------------------------------------------------------------
static EventGroupHandle_t s_events = NULL;
#define BIT_CONNECTED  (1 << 0)
#define BIT_TIME_OK    (1 << 1)

static int s_retry_count = 0;
#define MAX_FAST_RETRIES 5

// STA netif handle + whether esp_wifi_start() has been called this boot.
static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_started = false;

// Set true when the user turns WiFi OFF via the live toggle
// (panadapter_wifi_set_enabled(false)). While set, the STA_DISCONNECTED
// handler must NOT auto-reconnect — otherwise a user-requested stop would be
// fought by the retry loop and the radio would come straight back up.
static volatile bool s_wifi_user_disabled = false;

// A user SSID scan competes with the reconnect loop for the C6's radio: with
// an unreachable stored network the connect retries run back-to-back (the
// backoff even sleeps ON the event-loop task), and every scan returns 0 APs
// (hardware-captured 2026-08-02: stored hotel SSID gone, "scan done: 0 AP(s)"
// ten seconds after the request, plenty of networks around). While a scan is
// in flight AND we are not connected, the retry chain is held off and
// re-kicked when the scan completes; time-bounded so a lost SCAN_DONE can't
// kill WiFi. Scans while CONNECTED are untouched - they have always worked,
// and dropping a live link to scan would be worse than the disease.
static volatile bool    s_scan_hold = false;
static volatile int64_t s_scan_hold_us = 0;
#define SCAN_HOLD_TIMEOUT_US (15LL * 1000000)
// One free automatic retry per user scan: a 0-AP result while we were
// holding the retry chain is almost always interference (some timing window
// trampling the scan), not an actually-empty band - retry once before
// reporting "no networks", which also covers windows not yet imagined.
static volatile bool    s_scan_auto_retried = false;

// Roaming to a remembered network (Roy KI0ER: "remember a few SSID setups ...
// auto connect if that SSID is present"). When the configured network will not
// come up, we scan once and, if a DIFFERENT remembered network is on the air,
// switch to it. s_roam_scan marks a scan we started for that purpose - the user
// Scan button uses the same machinery and must not trigger a credential change.
// s_roam_last_us rate-limits the attempts instead of allowing only one per
// episode: a one-shot would stop looking entirely if the first scan happened
// before the other network was up, which is precisely the "arrived somewhere new"
// case this feature is for. Cleared on a successful IP so the next episode can
// roam immediately.
static volatile bool    s_roam_scan    = false;
static volatile int64_t s_roam_last_us = 0;
#define ROAM_RESCAN_INTERVAL_US (30LL * 1000000)
// How many failed connects before looking elsewhere. Deliberately small: waiting
// for the fast-retry budget AND the 10 s backoff took the best part of a minute
// to notice a hotspot had gone (operator, 2026-08-05). Two failures is already a
// clear signal, and a scan cannot pick a different network unless the configured
// one is genuinely absent from the air - so being eager here is safe.
#define ROAM_AFTER_RETRIES 2

// True when WE created the STA netif (without IDF's default, un-guarded event
// handlers) and therefore drive its start/connect/disconnect lifecycle from
// on_wifi_event()/on_ip_event(). False when we reused an ESP-Hosted
// auto-created WIFI_STA_DEF (whose own default handlers do that).
static bool s_manual_netif = false;
// Set once esp_netif_action_start() has run. Guards against the DUPLICATE
// WIFI_EVENT_STA_START that newer ESP-Hosted/C6 firmware delivers: IDF's default
// esp_netif_action_start has no "already started" guard (esp_netif_lwip.c calls
// netif_add() unconditionally), so a second STA_START would netif_add() twice →
// "netif already added" assert → reboot. Seen in the field on the SSID-scan path
// (Roy's log: two "STA started" lines then the assert).
static bool s_netif_started = false;

// Live credentials. Filled at boot from NVS, or overwritten via
// panadapter_wifi_reconnect(). 0-length ssid means "not configured".
static char s_ssid[33] = {0};
static char s_pass[65] = {0};

// WiFi scan state (for the SSID picker). Results are written in the
// WIFI_EVENT_SCAN_DONE handler (event-loop task) and read by the UI via
// panadapter_wifi_scan_get(); s_scan_state is set last so a reader that sees
// DONE also sees a complete s_scan[] / s_scan_n.
#define WIFI_SCAN_MAX 24
static wifi_scan_ap_t s_scan[WIFI_SCAN_MAX];
static volatile int s_scan_n = 0;
static volatile wifi_scan_state_t s_scan_state = WIFI_SCAN_IDLE;

// Tab5 SDIO pin drive-strength quirk -----------------------------------
// Matches N6HAN's qrp_companion and M5Stack factory demo: SDIO between P4
// and the C6 co-processor needs the LOWEST drive capability or the link
// becomes unreliable.
static void set_sdio_gpio_drive(void)
{
    static const gpio_num_t sdio_gpios[] = {
        GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
        GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_15,
    };
    for (size_t i = 0; i < sizeof(sdio_gpios) / sizeof(sdio_gpios[0]); i++) {
        gpio_set_drive_capability(sdio_gpios[i], GPIO_DRIVE_CAP_0);
    }
}

// SNTP callback --------------------------------------------------------
static void sntp_sync_cb(struct timeval *tv)
{
    struct tm tm_utc;
    gmtime_r(&tv->tv_sec, &tm_utc);
    ESP_LOGI(TAG, "SNTP sync: UTC %04d-%02d-%02d %02d:%02d:%02d",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    xEventGroupSetBits(s_events, BIT_TIME_OK);

    // Write to the Tab5 supercap RTC, persist NVS anchor, update system clock,
    // and push to QMX (unless QMX GPS flag is set).
    time_sync_notify_sntp(tv->tv_sec);
}

// Replicates the work IDF's default WIFI_EVENT_STA_START handler (wifi_start in
// wifi_default.c) does for our manually-created netif: register the wifi→netif
// RX path and the netstack buffer callbacks, copy the MAC, then start the netif.
// Called exactly once (guarded by s_netif_started) so a duplicate STA_START is a
// no-op instead of a second netif_add() crash.
static void manual_netif_start(esp_event_base_t base, int32_t id, void *data)
{
    wifi_netif_driver_t drv = esp_netif_get_io_driver(s_sta_netif);
    uint8_t mac[6];
    if (esp_wifi_is_if_ready_when_started(drv)) {
        // Older path (e.g. native esp_wifi): RX cb can register at start time.
        esp_wifi_register_if_rxcb(drv, esp_netif_receive, s_sta_netif);
    }
    esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref,
                                          esp_netif_netstack_buf_free);
    if (esp_wifi_get_if_mac(drv, mac) == ESP_OK) {
        esp_netif_set_mac(s_sta_netif, mac);
    }
    esp_netif_action_start(s_sta_netif, base, id, data);
}

// Apply a remembered network's credentials directly to the driver.
//
// Deliberately NOT panadapter_wifi_update_credentials(): that persists the SSID
// as the configured one. Roaming is a convenience, not a decision - the network
// the operator typed in stays the configured one, so returning home behaves
// exactly as before. A successful connect is what promotes a network in the
// remembered list, via settings_wifi_known_remember() on GOT_IP.
static void apply_creds_live(const char *ssid, const char *pass)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");

    wifi_config_t sta_cfg = { 0 };
    memcpy(sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
    memcpy(sta_cfg.sta.password, s_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

// Pick the strongest remembered network that is actually on the air and switch
// to it. `recs`/`num` are the freshly harvested scan records - see the call site
// for why this must run before any connect is issued.
static void roam_to_known_if_present(const wifi_ap_record_t *recs, uint16_t num)
{
    // STATIC, not on the stack: every caller of this runs on the system event
    // task, whose stack is under 3 KB, and this array is ~590 bytes. The scan
    // records buffer in the SCAN_DONE handler is static for the same reason.
    // Safe to share because that task is single-threaded.
    static wifi_known_t known[WIFI_KNOWN_MAX];
    int kn = settings_wifi_known_get(known, WIFI_KNOWN_MAX);
    if (kn <= 1) return;                 // nothing to roam between

    int best_k = -1, best_rssi = -127;
    for (uint16_t i = 0; i < num; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        for (int k = 0; k < kn; k++) {
            if (strcmp((const char *)recs[i].ssid, known[k].ssid) != 0) continue;
            if (recs[i].rssi > best_rssi) { best_rssi = recs[i].rssi; best_k = k; }
            break;
        }
    }
    if (best_k < 0) {
        ESP_LOGI(TAG, "roam: none of the %d remembered network(s) are on the air", kn);
        return;
    }
    if (strcmp(known[best_k].ssid, s_ssid) == 0) {
        // The one we are already failing on is the best remembered one present -
        // nothing to gain by "switching" to it.
        ESP_LOGI(TAG, "roam: '%s' (%d dBm) is already the network we are trying",
                 s_ssid, best_rssi);
        return;
    }
    ESP_LOGW(TAG, "roam: '%s' not reachable - switching to remembered '%s' (%d dBm)",
             s_ssid, known[best_k].ssid, best_rssi);
    apply_creds_live(known[best_k].ssid, known[best_k].pass);
    s_retry_count = 0;                   // fresh fast-retry budget for the new one
}

// Start a roam scan if it is worth doing. Returns true when a scan was started,
// in which case the caller must return: SCAN_DONE picks the network and re-kicks
// the connect.
static bool try_start_roam_scan(void)
{
    if (s_wifi_user_disabled) return false;
    if (settings_wifi_known_count() <= 1) return false;   // nothing to roam between
    int64_t now = esp_timer_get_time();
    if (s_roam_last_us && (now - s_roam_last_us) < ROAM_RESCAN_INTERVAL_US) return false;
    if (s_scan_hold) return false;                        // a scan is already in flight

    wifi_scan_config_t cfg = { 0 };
    s_roam_last_us      = now;
    s_roam_scan         = true;
    s_scan_hold         = true;
    s_scan_hold_us      = now;
    s_scan_auto_retried = false;
    if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
        s_roam_scan = false;
        s_scan_hold = false;
        return false;
    }
    ESP_LOGI(TAG, "'%s' not answering - scanning for a remembered network", s_ssid);
    return true;
}

// Event handlers -------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        // Drive the netif start ourselves, exactly once. The s_netif_started
        // guard makes the duplicate STA_START that newer ESP-Hosted firmware
        // delivers harmless (see s_netif_started declaration).
        if (s_manual_netif && !s_netif_started) {
            s_netif_started = true;
            manual_netif_start(base, id, data);
        }
        if (s_ssid[0] == '\0') {
            ESP_LOGI(TAG, "STA started but no SSID configured; not connecting");
            return;
        }
        ESP_LOGI(TAG, "STA started, connecting to '%s'", s_ssid);
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_STOP) {
        if (s_manual_netif) {
            esp_netif_action_stop(s_sta_netif, base, id, data);
            s_netif_started = false;
        }
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        if (s_manual_netif) {
            // Hosted path (esp_wifi_remote): the interface isn't ready at start,
            // so the RX cb is registered here on connect — this is the glue a
            // bare esp_netif_new()/attach left out (data path was dead without it).
            wifi_netif_driver_t drv = esp_netif_get_io_driver(s_sta_netif);
            if (!esp_wifi_is_if_ready_when_started(drv)) {
                esp_wifi_register_if_rxcb(drv, esp_netif_receive, s_sta_netif);
            }
            esp_netif_action_connected(s_sta_netif, base, id, data);
        }
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        // Harvest FIRST: esp_wifi_connect() flushes the scan results on the
        // radio, so the retry-chain re-kick below must wait until the records
        // are copied out. Hardware-caught 2026-08-02: the first version of
        // the scan-hold fix re-kicked first and read back 0 APs every time -
        // the hold had given the scan its airtime and the harvest then threw
        // the results away.
        static wifi_ap_record_t recs[WIFI_SCAN_MAX];
        uint16_t num = WIFI_SCAN_MAX;
        esp_err_t get_err = esp_wifi_scan_get_ap_records(&num, recs);
        // Empty result while holding: take the one free retry BEFORE releasing
        // the hold (releasing reconnects, which flushes scan state). Extend the
        // hold clock so the second scan gets its full window.
        if (get_err == ESP_OK && num == 0 && s_scan_hold && !s_scan_auto_retried) {
            s_scan_auto_retried = true;
            s_scan_hold_us = esp_timer_get_time();
            wifi_scan_config_t retry_cfg = { 0 };
            if (esp_wifi_scan_start(&retry_cfg, false) == ESP_OK) {
                ESP_LOGW(TAG, "scan returned 0 APs while held - auto-retrying once");
                return;   // stay RUNNING; the retry's own SCAN_DONE lands here
            }
        }
        // Roam BEFORE the re-kick below. This is the only safe window: the
        // records are already out of the radio (so the connect's scan-flush
        // cannot lose them), and no connect has been issued yet - so the new
        // credentials are the ones the re-kick actually uses. Doing it after the
        // re-kick would waste an attempt on the old network first.
        if (s_roam_scan && get_err == ESP_OK) {
            s_roam_scan = false;
            roam_to_known_if_present(recs, num);
        }

        // Scan finished: if the retry chain was held for it, resume connecting
        // (the chain is event-driven, so skipping a retry ends it - it must be
        // re-kicked here or WiFi stays down until reboot).
        if (s_scan_hold) {
            s_scan_hold = false;
            if (!s_wifi_user_disabled) esp_wifi_connect();
        }
        if (get_err != ESP_OK) {
            s_scan_state = WIFI_SCAN_FAILED;
            return;
        }
        int n = 0;
        for (uint16_t i = 0; i < num && n < WIFI_SCAN_MAX; i++) {
            if (recs[i].ssid[0] == '\0') continue;  // hidden SSID
            bool dup = false;
            for (int j = 0; j < n; j++) {
                if (strcmp(s_scan[j].ssid, (char *)recs[i].ssid) == 0) { dup = true; break; }
            }
            if (dup) continue;
            strncpy(s_scan[n].ssid, (char *)recs[i].ssid, sizeof(s_scan[n].ssid) - 1);
            s_scan[n].ssid[sizeof(s_scan[n].ssid) - 1] = '\0';
            s_scan[n].rssi   = recs[i].rssi;
            s_scan[n].locked = (recs[i].authmode != WIFI_AUTH_OPEN);
            n++;
        }
        s_scan_n = n;
        s_scan_state = WIFI_SCAN_DONE;  // set last
        ESP_LOGI(TAG, "scan done: %d AP(s)", n);

    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_manual_netif) esp_netif_action_disconnected(s_sta_netif, base, id, data);
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        webserver_stop();
        if (s_wifi_user_disabled) {
            ESP_LOGI(TAG, "Disconnected (WiFi turned off by user); not reconnecting");
            return;
        }
        if (s_scan_hold &&
            (esp_timer_get_time() - s_scan_hold_us) < SCAN_HOLD_TIMEOUT_US) {
            // A user SSID scan is in flight: don't immediately re-grab the
            // radio (and don't sleep the event loop in the backoff, which
            // would also delay SCAN_DONE). The scan-done path resumes us.
            ESP_LOGI(TAG, "Disconnected during SSID scan - retry held until scan completes");
            return;
        }
        s_retry_count++;

        // Look for a remembered network as soon as two connects have failed,
        // rather than after the whole fast-retry budget plus a 10 s sleep.
        if (s_retry_count >= ROAM_AFTER_RETRIES && try_start_roam_scan()) return;

        if (s_retry_count <= MAX_FAST_RETRIES) {
            ESP_LOGW(TAG, "Disconnected (reason=%d) retry %d/%d",
                     e->reason, s_retry_count, MAX_FAST_RETRIES);
            esp_wifi_connect();
        } else {
            // Back off — try again every 10 s.
            ESP_LOGW(TAG, "Disconnected (reason=%d) backing off", e->reason);
            vTaskDelay(pdMS_TO_TICKS(10000));
            // Re-check the scan hold AFTER the sleep: a scan started during
            // the backoff would otherwise be flushed by this wake-up connect
            // (hardware-caught 2026-08-02: the first Scan press after a
            // backoff always read 0 APs; a press during a connect attempt
            // worked - the entry check above only covers that case). The
            // SCAN_DONE handler re-kicks the chain, so returning here is safe.
            if (s_scan_hold &&
                (esp_timer_get_time() - s_scan_hold_us) < SCAN_HOLD_TIMEOUT_US) {
                ESP_LOGI(TAG, "backoff wake during SSID scan - retry held");
                return;
            }

            // Still nowhere? Keep looking for a remembered network on the way
            // round the backoff loop too (rate-limited inside).
            if (try_start_roam_scan()) return;
            esp_wifi_connect();
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        if (s_manual_netif) {
            // Mirrors IDF's default got-ip handler: tell the wifi driver the new
            // IP, then run the netif got-ip action (sets default route etc.).
            esp_wifi_internal_set_sta_ip();
            esp_netif_action_got_ip(s_sta_netif, base, id, data);
        }
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        s_roam_last_us = 0;         // this network works; allow an immediate roam next time
        xEventGroupSetBits(s_events, BIT_CONNECTED);

        // Remember the network that actually worked, most-recently-used first.
        // Building the list from successes rather than from a management screen
        // means there is nothing for the operator to maintain - which is the
        // whole point of the request.
        settings_wifi_known_remember(s_ssid, s_pass);
        {
            // Count only - never a WIFI_KNOWN_MAX buffer here. This runs on the
            // system event task, whose stack is under 3 KB; a 588-byte array on it
            // is a stack-protection fault, which is exactly how this got flashed
            // once and crash-looped (2026-08-05).
            int kn_n = settings_wifi_known_count();
            ESP_LOGI(TAG, "remembered '%s' (%d network%s known)",
                     s_ssid, kn_n, kn_n == 1 ? "" : "s");
        }

        // Kick off SNTP on first connect.
        static bool sntp_started = false;
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.sync_cb = sntp_sync_cb;
            esp_netif_sntp_init(&cfg);
            ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
        }

        webserver_start();
        rigctld_server_start();
    }
}

// Ensure the STA netif exists exactly once before calling esp_wifi_start().
//
// The "netif already added" assert that bricked WiFi on newer ESP-Hosted/C6
// firmware comes from a DUPLICATE WIFI_EVENT_STA_START: IDF's default
// esp_netif_action_start handler is not idempotent (esp_netif_lwip.c always
// calls netif_add()), so the second STA_START adds the same netif twice → panic.
// (Field-proven on Roy's unit, on the SSID-scan path: two "STA started" lines
// then the assert.)
//
// Fix: don't install IDF's default (un-guarded) STA handlers at all. Create the
// netif with esp_netif_new()/esp_netif_attach_wifi_station() and drive its
// lifecycle from on_wifi_event()/on_ip_event() with the s_netif_started guard,
// replicating the default handlers' RX-callback glue (manual_netif_start() +
// the STA_CONNECTED rxcb registration) so the data path still works on hosted.
//
// We still poll ~2 s for an ESP-Hosted auto-created WIFI_STA_DEF first; if one
// exists, its own default handlers manage it and we leave it alone.
static void ensure_sta_netif(void)
{
    if (s_sta_netif) return;  // idempotent across boot / scan / reconnect paths

    for (int i = 0; i < 20 && esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_sta_netif != NULL) {
        s_manual_netif = false;
        ESP_LOGI(TAG, "STA netif auto-created by ESP-Hosted; reusing");
        return;
    }

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    esp_netif_config_t cfg = {
        .base   = &base,
        .driver = NULL,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };
    s_sta_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(s_sta_netif));
    s_manual_netif  = true;
    s_netif_started = false;
    ESP_LOGI(TAG, "STA netif created (manual guarded handlers, hosted-safe)");
}

// Init runs in its own task so app_main is not blocked --------------
static void wifi_task(void *arg)
{
    ESP_LOGI(TAG, "calling esp_hosted_init() explicitly (constructor not running)");
    extern esp_err_t esp_hosted_init(void);
    esp_err_t hosted_err = esp_hosted_init();
    if (hosted_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(hosted_err));
        return;
    }
    ESP_LOGI(TAG, "esp_hosted_init OK");

    ESP_LOGI(TAG, "powering on C6 co-processor");
    bsp_set_wifi_power_enable(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    set_sdio_gpio_drive();

    // ESP-IDF core init (event loop, default STA netif).
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    // Load credentials from NVS.
    qmx_settings_t cfg;
    settings_load_all(&cfg);
    strncpy(s_ssid, cfg.wifi_ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, cfg.wifi_pass, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';

    wifi_config_t sta_cfg = { 0 };
    // s_ssid/s_pass already NUL-terminated; sta.ssid/password are zero-init via { 0 }.
    memcpy(sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
    memcpy(sta_cfg.sta.password, s_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Only start the radio if we actually have credentials. esp_wifi_start()
    // is what raises WIFI_EVENT_STA_START and drives the netif start/add path;
    // not starting it with no SSID both avoids that path entirely (belt-and-
    // suspenders against the double-add crash above) and saves power. WiFi is
    // brought up later from panadapter_wifi_reconnect() when the user saves
    // credentials in the settings drawer.
    if (s_ssid[0] != '\0' && cfg.wifi_enabled) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ensure_sta_netif();
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else if (s_ssid[0] != '\0') {
        ESP_LOGW(TAG, "WiFi credentials present but WiFi boot-initiation disabled");
    } else {
        ESP_LOGW(TAG, "no WiFi credentials configured; WiFi idle until configured");
    }

    // Periodic status log so user can see what's going on.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        EventBits_t b = xEventGroupGetBits(s_events);
        if (b & BIT_CONNECTED) {
            time_t now = time(NULL);
            struct tm tm_utc;
            gmtime_r(&now, &tm_utc);
            ESP_LOGI(TAG, "online; UTC %04d-%02d-%02d %02d:%02d:%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                     tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
        }
    }
}

// Public API -----------------------------------------------------------
void panadapter_wifi_start(void)
{
    if (s_events) return;  // idempotent
    s_events = xEventGroupCreate();
    psram_task_create(wifi_task, "wifi", 4096, NULL, 5, tskNO_AFFINITY);
}

bool wifi_is_connected(void)
{
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

const char *wifi_get_ssid(void)
{
    static char ssid_buf[33];
    if (!wifi_is_connected()) { ssid_buf[0] = '\0'; return ssid_buf; }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { ssid_buf[0] = '\0'; return ssid_buf; }
    memcpy(ssid_buf, ap.ssid, sizeof(ssid_buf) - 1);
    ssid_buf[sizeof(ssid_buf) - 1] = '\0';
    return ssid_buf;
}

int wifi_get_rssi_dbm(void)
{
    if (!wifi_is_connected()) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

const char *wifi_get_ip(void)
{
    static char ip_buf[16];
    ip_buf[0] = '\0';
    if (!wifi_is_connected()) return ip_buf;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return ip_buf;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return ip_buf;
    if (ip_info.ip.addr == 0) return ip_buf;
    snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
    return ip_buf;
}

bool wifi_time_is_valid(void)
{
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & BIT_TIME_OK) != 0;
}
void panadapter_wifi_update_credentials(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "update_credentials: empty SSID, ignoring");
        return;
    }
    // Update live creds.
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    if (pass) {
        strncpy(s_pass, pass, sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = '\0';
    } else {
        s_pass[0] = '\0';
    }
    // Persist.
    settings_set_wifi_ssid(s_ssid);
    settings_set_wifi_pass(s_pass);
    // Push into the driver config so a later connect/start uses them. Valid
    // even before esp_wifi_start() — esp_wifi_init() always ran at boot.
    wifi_config_t sta_cfg = { 0 };
    memcpy(sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
    memcpy(sta_cfg.sta.password, s_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    ESP_LOGI(TAG, "credentials updated for '%s' (connection state unchanged)", s_ssid);
}

void panadapter_wifi_reconnect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "reconnect: empty SSID, ignoring");
        return;
    }
    ESP_LOGI(TAG, "reconnect: switching to '%s'", ssid);

    // Explicit intent to connect — clear any prior "user turned WiFi off"
    // state so the connect below isn't suppressed by the STA_DISCONNECTED guard.
    s_wifi_user_disabled = false;

    panadapter_wifi_update_credentials(ssid, pass);

    // Reset retry counter so fast retries get a fresh budget.
    s_retry_count = 0;

    if (!s_wifi_started) {
        // First credentials this boot (booted with no SSID, so the radio was
        // left idle). Bring it up now; esp_wifi_start() raises STA_START and
        // the event handler issues the connect.
        ESP_LOGI(TAG, "starting WiFi for the first time this boot");
        ensure_sta_netif();
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        // Already running: cycle the connection with the new config.
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
}

// Live WiFi on/off. Runs off the LVGL thread because ensure_sta_netif() can
// poll for up to ~2 s and esp_wifi_start()/stop() can block. esp_wifi_init()
// always ran at boot (in wifi_task, regardless of the enabled flag), so the
// radio is initialised and can be started/stopped here even if boot left it
// idle.
static void wifi_set_enabled_task(void *arg)
{
    bool en = (bool)(intptr_t)arg;
    if (en) {
        s_wifi_user_disabled = false;
        s_retry_count = 0;
        if (s_ssid[0] == '\0') {
            ESP_LOGW(TAG, "enable: no SSID configured; nothing to connect to");
        } else if (!s_wifi_started) {
            ESP_LOGI(TAG, "enable: starting WiFi");
            ensure_sta_netif();
            if (esp_wifi_start() == ESP_OK) s_wifi_started = true;
            else ESP_LOGE(TAG, "enable: esp_wifi_start failed");
        } else {
            ESP_LOGI(TAG, "enable: reconnecting");
            esp_wifi_connect();
        }
    } else {
        // Disconnect only — deliberately NOT esp_wifi_stop(). Leaving the radio
        // "started" keeps the netif in place, so re-enabling is a plain
        // esp_wifi_connect() (the well-trodden retry path) instead of a
        // stop→start→netif-re-add cycle, which is the fragile path this driver
        // has a long crash history with (see ensure_sta_netif / s_netif_started).
        // The s_wifi_user_disabled guard in on_wifi_event() suppresses the
        // auto-reconnect loop, and STA_DISCONNECTED already stops the webserver,
        // so this is functionally "off": no association, no traffic, no retries.
        s_wifi_user_disabled = true;
        esp_wifi_disconnect();
        ESP_LOGI(TAG, "disable: WiFi disconnected (radio left started for fast re-enable)");
    }
    vTaskDelete(NULL);
}

void panadapter_wifi_set_enabled(bool enabled)
{
    settings_set_wifi_enabled(enabled);   // persist the boot preference regardless
    if (!s_events) return;                // subsystem not up yet; NVS flag applies at boot
    psram_task_create(wifi_set_enabled_task, "wifi_en", 4096,
                      (void *)(intptr_t)enabled, 5, tskNO_AFFINITY);
}

// ---- WiFi scan (SSID picker) -----------------------------------------
// Runs the (potentially slow) radio bring-up + scan kick-off off the LVGL
// thread. ensure_sta_netif() polls for up to ~2 s, and esp_wifi_start() can
// block, so neither may run on the UI task.
static void wifi_scan_task(void *arg)
{
    (void)arg;
    if (!s_wifi_started) {
        ensure_sta_netif();
        if (esp_wifi_start() != ESP_OK) {
            s_scan_state = WIFI_SCAN_FAILED;
            vTaskDelete(NULL);
            return;
        }
        s_wifi_started = true;
    }
    s_scan_n = 0;
    // Not connected -> the reconnect chain owns the radio (an unreachable
    // stored SSID retries forever) and starves the scan to 0 APs. Hold the
    // chain and abort any in-flight connect attempt so the scan gets real
    // airtime; the SCAN_DONE handler resumes connecting.
    if (!(xEventGroupGetBits(s_events) & BIT_CONNECTED)) {
        s_scan_hold_us = esp_timer_get_time();
        s_scan_hold = true;
        esp_wifi_disconnect();               // no-op error if idle - fine
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    wifi_scan_config_t scan_cfg = { 0 };  // active scan, all channels
    if (esp_wifi_scan_start(&scan_cfg, false) != ESP_OK) {
        s_scan_state = WIFI_SCAN_FAILED;
        if (s_scan_hold) {                   // resume the chain we held
            s_scan_hold = false;
            if (!s_wifi_user_disabled) esp_wifi_connect();
        }
    }
    vTaskDelete(NULL);
}

void panadapter_wifi_scan_start(void)
{
    // "Already scanning" only blocks a FRESH scan - a RUNNING state older
    // than 20 s means the SCAN_DONE event was lost (esp_hosted RPC drop),
    // and without this escape every later press would be refused until
    // reboot (Scan permanently dead is worse than a doubled scan).
    static int64_t s_scan_started_us = 0;
    int64_t now = esp_timer_get_time();
    if (s_scan_state == WIFI_SCAN_RUNNING &&
        (now - s_scan_started_us) < 20LL * 1000000) return;
    s_scan_started_us = now;
    s_scan_state = WIFI_SCAN_RUNNING;
    s_scan_auto_retried = false;   // each user press gets one free retry
    psram_task_create(wifi_scan_task, "wifi_scan", 4096, NULL, 5, tskNO_AFFINITY);
}

wifi_scan_state_t panadapter_wifi_scan_state(void)
{
    return s_scan_state;
}

int panadapter_wifi_scan_get(wifi_scan_ap_t *out, int max)
{
    if (!out || max <= 0 || s_scan_state != WIFI_SCAN_DONE) return 0;
    int n = s_scan_n;
    if (n > max) n = max;
    memcpy(out, s_scan, n * sizeof(wifi_scan_ap_t));
    return n;
}
