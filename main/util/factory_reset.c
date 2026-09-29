#include "factory_reset.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "factory_reset";

/* ⛔ WiFi-specific NVS keys, duplicated from storage/settings.c's #defines.
 *
 * This file runs BEFORE nvs_flash_init()/settings_init() (see
 * factory_reset_apply_pending()'s call site in main.c), so it cannot use
 * settings.h's getters/setters - those need s_ready and a loaded
 * qmx_settings_t, neither of which exist yet. It has to talk to the "qmx"
 * namespace in the "user_nvs" partition directly, by the same key names
 * settings.c uses.
 *
 * SWEEP THIS TABLE whenever settings.c gains a new WiFi-related NVS key
 * (grep it for KEY_WIFI_). Miss one here and "Reset settings" silently wipes
 * it, or "Reset WiFi" silently keeps it - the exact bug this file fixes for
 * Randy N4OPI, 2026-09-26. */
#define WIFI_NVS_PARTITION "user_nvs"
#define WIFI_NVS_NAMESPACE "qmx"           // must match settings.c's NVS_NS
#define WIFI_KNOWN_BLOB_MAX (6 * 98)       // WIFI_KNOWN_MAX * sizeof(wifi_known_t)

typedef enum { WK_STR, WK_U8, WK_BLOB } wifi_key_type_t;
typedef struct { const char *key; wifi_key_type_t type; size_t max_len; } wifi_key_t;

static const wifi_key_t WIFI_KEYS[] = {
    { "wifi_ssid",   WK_STR,  33 },
    { "wifi_pass",   WK_STR,  65 },
    { "wifi_ip",     WK_STR,  16 },
    { "wifi_mask",   WK_STR,  16 },
    { "wifi_gw",     WK_STR,  16 },
    { "wifi_dns",    WK_STR,  16 },
    { "wifi_pref",   WK_STR,  33 },
    { "wifi_stssid", WK_STR,  33 },
    { "wifi_en",     WK_U8,   1  },
    { "wifi_known",  WK_BLOB, WIFI_KNOWN_BLOB_MAX },
};
#define WIFI_KEY_N (sizeof(WIFI_KEYS) / sizeof(WIFI_KEYS[0]))

typedef struct {
    uint8_t buf[WIFI_KNOWN_BLOB_MAX];
    size_t  len;
    bool    present;   // absent must stay absent - an empty string is not the
                        // same as "never configured" to code that reads it back
} wifi_saved_t;

static void wifi_keys_save(nvs_handle_t h, wifi_saved_t *out)
{
    for (size_t i = 0; i < WIFI_KEY_N; i++) {
        out[i].len = WIFI_KEYS[i].max_len;
        esp_err_t e;
        if (WIFI_KEYS[i].type == WK_STR)
            e = nvs_get_str(h, WIFI_KEYS[i].key, (char *)out[i].buf, &out[i].len);
        else if (WIFI_KEYS[i].type == WK_U8)
            e = nvs_get_u8(h, WIFI_KEYS[i].key, out[i].buf);
        else
            e = nvs_get_blob(h, WIFI_KEYS[i].key, out[i].buf, &out[i].len);
        out[i].present = (e == ESP_OK);
    }
}

static void wifi_keys_restore(nvs_handle_t h, const wifi_saved_t *in)
{
    for (size_t i = 0; i < WIFI_KEY_N; i++) {
        if (!in[i].present) continue;
        if (WIFI_KEYS[i].type == WK_STR)
            nvs_set_str(h, WIFI_KEYS[i].key, (const char *)in[i].buf);
        else if (WIFI_KEYS[i].type == WK_U8)
            nvs_set_u8(h, WIFI_KEYS[i].key, in[i].buf[0]);
        else
            nvs_set_blob(h, WIFI_KEYS[i].key, in[i].buf, in[i].len);
    }
    nvs_commit(h);
}

// Read every WiFi key out of "user_nvs" into `saved`, returning true if the
// partition could be opened at all (an unformatted partition on first boot
// is not an error - there is simply nothing to preserve).
static bool wifi_keys_read_all(wifi_saved_t *saved)
{
    bool ok = false;
    if (nvs_flash_init_partition(WIFI_NVS_PARTITION) == ESP_OK) {
        nvs_handle_t h;
        if (nvs_open_from_partition(WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE,
                                     NVS_READONLY, &h) == ESP_OK) {
            wifi_keys_save(h, saved);
            nvs_close(h);
            ok = true;
        }
        nvs_flash_deinit_partition(WIFI_NVS_PARTITION);
    }
    return ok;
}

// RTC_NOINIT_ATTR survives esp_restart() (RTC RAM is retained across a soft
// reboot) but is NOT zero-initialised, so after a real cold power-on it holds
// garbage. The magic word guards against that: only a value written by
// factory_reset_request() this power-cycle is honoured. A 1-in-2^32 chance of
// garbage matching the magic once is acceptable (worst case: one spurious
// settings reset, recoverable by re-entering settings).
RTC_NOINIT_ATTR static uint32_t s_reset_magic;
RTC_NOINIT_ATTR static uint32_t s_reset_flags;

#define RESET_MAGIC     0x0FACE5E7u   // "factory reset" sentinel
#define FLAG_SETTINGS   (1u << 0)     // erase user_nvs (app settings)
#define FLAG_NETWORK    (1u << 1)     // erase default nvs (WiFi/system)

void factory_reset_apply_pending(void)
{
    if (s_reset_magic != RESET_MAGIC) {
        return;  // normal boot — nothing pending
    }
    uint32_t flags = s_reset_flags;
    // Consume the request immediately so a crash mid-erase can't wedge us into
    // an erase-reboot loop.
    s_reset_magic = 0;
    s_reset_flags = 0;

    if (flags & FLAG_SETTINGS) {
        // "user_nvs" holds all app settings + memory channels AND the WiFi
        // keys (settings.c keeps everything in one "qmx" namespace) - so a
        // plain erase of the whole partition takes the WiFi credentials and
        // static IP with it, despite the button's own tooltip promising they
        // are kept. Save them first and write them straight back.
        static wifi_saved_t wifi_saved[WIFI_KEY_N];
        bool have_wifi = wifi_keys_read_all(wifi_saved);

        esp_err_t err = nvs_flash_erase_partition("user_nvs");
        ESP_LOGW(TAG, "SETTINGS reset: erased user_nvs -> 0x%x", err);

        if (have_wifi && nvs_flash_init_partition("user_nvs") == ESP_OK) {
            nvs_handle_t h;
            if (nvs_open_from_partition("user_nvs", WIFI_NVS_NAMESPACE,
                                         NVS_READWRITE, &h) == ESP_OK) {
                wifi_keys_restore(h, wifi_saved);
                nvs_close(h);
            }
            nvs_flash_deinit_partition("user_nvs");
        }
    }
    if (flags & FLAG_NETWORK) {
        // Default "nvs" partition: PHY calibration and any esp_hosted/system
        // state. This is what clears a stuck WiFi/link state that a normal
        // reflash leaves in place.
        esp_err_t err = nvs_flash_erase();
        ESP_LOGW(TAG, "NETWORK reset: erased default nvs -> 0x%x", err);

        // The WiFi credentials themselves are NOT in that partition - they are
        // specific keys inside "user_nvs" (see WIFI_KEYS above). Erase just
        // those, not the whole partition, so app settings survive "Reset WiFi"
        // as the button's own tooltip says they do.
        if (nvs_flash_init_partition("user_nvs") == ESP_OK) {
            nvs_handle_t h;
            if (nvs_open_from_partition("user_nvs", WIFI_NVS_NAMESPACE,
                                         NVS_READWRITE, &h) == ESP_OK) {
                for (size_t i = 0; i < WIFI_KEY_N; i++)
                    nvs_erase_key(h, WIFI_KEYS[i].key);
                nvs_commit(h);
                nvs_close(h);
            }
            nvs_flash_deinit_partition("user_nvs");
        }
    }
    // Fall through into the normal boot: the following nvs_flash_init() /
    // settings_init() will re-format the blank partition(s) and load defaults.
}

static void reboot_task(void *arg)
{
    (void)arg;
    // Give the HTTP handler's response time to flush over WiFi before we pull
    // the rug out. The request came in over the same link we're about to reset.
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "rebooting to apply factory reset (flags=0x%lx)",
             (unsigned long)s_reset_flags);
    esp_restart();
}

void factory_reset_request(bool reset_settings, bool reset_network)
{
    if (!reset_settings && !reset_network) {
        return;  // nothing to do
    }
    s_reset_flags = (reset_settings ? FLAG_SETTINGS : 0) |
                    (reset_network  ? FLAG_NETWORK  : 0);
    s_reset_magic = RESET_MAGIC;
    ESP_LOGW(TAG, "factory reset requested (settings=%d network=%d) — rebooting",
             reset_settings, reset_network);
    xTaskCreate(reboot_task, "reset_reboot", 2048, NULL, 6, NULL);
}
