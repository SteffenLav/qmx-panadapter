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
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "webserver.h"
#include "rigctld_server.h"
#include "time_sync.h"

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
        static wifi_ap_record_t recs[WIFI_SCAN_MAX];
        uint16_t num = WIFI_SCAN_MAX;
        if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
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
        s_retry_count++;
        if (s_retry_count <= MAX_FAST_RETRIES) {
            ESP_LOGW(TAG, "Disconnected (reason=%d) retry %d/%d",
                     e->reason, s_retry_count, MAX_FAST_RETRIES);
            esp_wifi_connect();
        } else {
            // Back off — try again every 10 s.
            ESP_LOGW(TAG, "Disconnected (reason=%d) backing off", e->reason);
            vTaskDelay(pdMS_TO_TICKS(10000));
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
        xEventGroupSetBits(s_events, BIT_CONNECTED);

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
    xTaskCreate(wifi_task, "wifi", 4096, NULL, 5, NULL);
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
void panadapter_wifi_reconnect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "reconnect: empty SSID, ignoring");
        return;
    }
    ESP_LOGI(TAG, "reconnect: switching to '%s'", ssid);

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

    // Reset retry counter so fast retries get a fresh budget.
    s_retry_count = 0;

    wifi_config_t sta_cfg = { 0 };
    memcpy(sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
    memcpy(sta_cfg.sta.password, s_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

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
    wifi_scan_config_t scan_cfg = { 0 };  // active scan, all channels
    if (esp_wifi_scan_start(&scan_cfg, false) != ESP_OK) {
        s_scan_state = WIFI_SCAN_FAILED;
    }
    vTaskDelete(NULL);
}

void panadapter_wifi_scan_start(void)
{
    if (s_scan_state == WIFI_SCAN_RUNNING) return;  // already scanning
    s_scan_state = WIFI_SCAN_RUNNING;
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
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
