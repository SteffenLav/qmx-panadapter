#include "wifi.h"
#include "wifi_credentials.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "wifi";

// State -----------------------------------------------------------------
static EventGroupHandle_t s_events = NULL;
#define BIT_CONNECTED  (1 << 0)
#define BIT_TIME_OK    (1 << 1)

static int s_retry_count = 0;
#define MAX_FAST_RETRIES 5

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
}

// Event handlers -------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting to '%s'", WIFI_STA_SSID);
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
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
    }
}

// Init runs in its own task so app_main is not blocked --------------
static void wifi_task(void *arg)
{
    ESP_LOGI(TAG, "powering on C6 co-processor");
    bsp_set_wifi_power_enable(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    set_sdio_gpio_drive();

    // ESP-IDF core init (event loop, default STA netif).
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, WIFI_STA_SSID, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, WIFI_STA_PASS, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // accept any; AP advertises actual mode

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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
void wifi_start(void)
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

bool wifi_time_is_valid(void)
{
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & BIT_TIME_OK) != 0;
}