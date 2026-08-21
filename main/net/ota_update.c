// See ota_update.h for the design and, in particular, why this is never
// automatic.

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_update.h"
#include "wifi.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "cat.h"
#include "dsp.h"
#include "webserver_ws.h"
#include "util/net_guard.h"
#include "esp_netif.h"

static const char *TAG = "ota";

static volatile ota_state_t s_state = OTA_IDLE;
static volatile int         s_pct   = 0;
static char                 s_msg[128];
static char                 s_url[256];
// Version being installed, taken from the incoming image's own descriptor so
// both screens can name it while the download runs (the operator asked: "1.8.8
// -> 1.8.9", not a bare "updating").
static char                 s_target_ver[32];

static void set_failed(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
    s_state = OTA_FAILED;
    ESP_LOGE(TAG, "update failed: %s", s_msg);
}

void ota_update_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;

    // We are on trial: the bootloader will revert to the previous image on the
    // next reset unless this call happens. Reaching here means the UI, the
    // network and the whole start-up sequence came up, which is the only
    // definition of "this image works" available from inside it.
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "new firmware confirmed good (%s on '%s') - rollback cancelled",
                 esp_app_get_description()->version, run->label);
    } else {
        ESP_LOGE(TAG, "could not confirm this image - it will roll back on reset");
    }
}


// Our IPv4 and netmask in HOST order. esp_netif stores NETWORK order, and
// getting that wrong is not theoretical - it shipped in v1.8.7 and made every
// LAN address fail the subnet test (Mark G4MEM). net_ipv4_from_network_order()
// is the one place that conversion lives.
static bool own_ipv4(uint32_t *ip_out, uint32_t *mask_out)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    *ip_out   = net_ipv4_from_network_order(info.ip.addr);
    *mask_out = net_ipv4_from_network_order(info.netmask.addr);
    return true;
}

// Firmware may come from GitHub over https, or from a server on the operator's
// OWN network over plain http - the identical rule Cloudlog uses, and the same
// host-tested net_guard that backs it. A firmware image is the most
// safety-critical thing this device will ever download, so the plaintext case
// stays pinned to a numeric address on our own subnet: a NAME would have to be
// resolved, and whatever answered then could differ from whatever answers when
// the transfer actually runs.
static bool source_allowed(const char *url, char *why, size_t why_sz)
{
    net_scheme_t scheme;
    char host[80];
    uint16_t port;

    if (!net_url_parse(url, &scheme, host, sizeof(host), &port)) {
        snprintf(why, why_sz, "that is not a usable download address");
        return false;
    }
    if (scheme == NET_SCHEME_HTTPS) return true;

    uint32_t target = 0, ours = 0, mask = 0;
    if (!own_ipv4(&ours, &mask)) {
        snprintf(why, why_sz, "no network address yet - connect to WiFi first");
        return false;
    }
    if (!net_ipv4_parse(host, &target)) {
        snprintf(why, why_sz, "http:// needs a numeric address, e.g. http://192.168.1.20");
        return false;
    }
    if (!net_plaintext_allowed(target, ours, mask)) {
        snprintf(why, why_sz, "%.20s is not on this network - use https://", host);
        return false;
    }
    return true;
}

static void ota_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "starting update from %s", s_url);

    // Same discipline as the QRZ/LoTW uploads: this board's WiFi link is the
    // fragile part and a multi-megabyte TLS download is the heaviest thing it
    // will ever be asked to do, so give it the link. TLS here runs on SOFTWARE
    // crypto (hardware AES cannot get DMA descriptors on this board), which
    // makes the transfer unusually expensive as well as large.
    webserver_ws_set_paused(true);
    dsp_set_transfer_quiet(true);

    esp_http_client_config_t http = {
        .url               = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        // GitHub's /releases/download/ URL is a 302 to objects.githubusercontent.com,
        // and without this esp_https_ota fails on the redirect rather than the
        // download - measured as "could not reach the download (0xffffffff)" on
        // the first real attempt. The bundle covers both hosts.
        .max_redirection_count = 5,
        // ⛔ THE ACTUAL ROOT CAUSE OF THAT SAME ERROR, FOUND 2026-08-21 - the
        // redirect above only got half fixed. esp_http_client's response buffer
        // defaults to 512 bytes (DEFAULT_HTTP_BUF_SIZE), and github.com's 302
        // itself carries 5,159 bytes of headers - dominated by a large
        // Content-Security-Policy header, MEASURED with a plain `curl -I` against
        // the real release URL. The header cannot fit, esp_http_client logs
        // "Out of buffer", and esp_https_ota_begin fails with the same generic
        // ESP_FAIL this file already had a comment about - so the earlier fix
        // addressed FOLLOWING the redirect, never RECEIVING its headers.
        //
        // This means OTA has likely never completed a real download from GitHub
        // on ANY shipped version since it was introduced in v1.8.9: the redirect
        // fix alone was never enough, and nothing exercised the real CDN response
        // until this was tested end-to-end for the first time.
        .buffer_size       = 8192,
        // ⛔ buffer_size ALONE WAS NOT ENOUGH - measured on hardware, still failed
        // identically after adding it. The real fault is on the SEND side:
        // http_client_prepare_first_line() builds "GET <path>?<query> HTTP/1.1"
        // into a buffer sized by buffer_size_TX, not buffer_size (that one only
        // covers what is RECEIVED). After GitHub's redirect, the request's own
        // path+query for the second hop IS the entire signed CDN URL - the same
        // ~930-byte string measured in the Location header - so building the
        // outgoing request line is what actually overflowed a 512-byte tx buffer,
        // not receiving GitHub's response. Both directions need headroom.
        .buffer_size_tx    = 8192,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) {
        set_failed("could not reach the download (0x%x)", err);
        goto out;
    }

    // Refuse an image that is not for this project/target before writing a byte
    // of it. esp_https_ota validates the header itself; this is the friendlier
    // check on top, because "wrong file" is a much likelier mistake than a
    // corrupt one.
    esp_app_desc_t incoming;
    if (esp_https_ota_get_img_desc(h, &incoming) == ESP_OK) {
        const esp_app_desc_t *cur = esp_app_get_description();
        if (strcmp(incoming.project_name, cur->project_name) != 0) {
            set_failed("that file is '%s', not %s firmware",
                       incoming.project_name, cur->project_name);
            esp_https_ota_abort(h);
            goto out;
        }
        strncpy(s_target_ver, incoming.version, sizeof(s_target_ver) - 1);
        s_target_ver[sizeof(s_target_ver) - 1] = '\0';
        ESP_LOGW(TAG, "incoming: %s %s", incoming.project_name, incoming.version);
    }

    // ⛔ HARDWARE WATCHDOG RESET, found on hardware 2026-08-21, right at the end
    // of a real download - a cyan flash, then rst:0x7 (HP_SYS_HP_WDT_RESET), the
    // system-level watchdog rather than a clean esp_restart(). Continuous audio
    // ring overflows (tens of thousands of samples/s dropped) ran for the WHOLE
    // download beforehand, not just at the crash - the same "interrupts/cache
    // disabled too long" mechanism this board's cyan-flash bug already documents,
    // but sustained for minutes instead of one frame.
    //
    // Traced to the buffer_size fix immediately above. esp_https_ota.c sizes its
    // OWN per-call image chunk as MAX(http_config->buffer_size, DEFAULT_OTA_BUF_SIZE)
    // - so raising buffer_size to 8192 to fit GitHub's redirect headers ALSO made
    // every download chunk 8192 bytes instead of a few hundred: one continuous
    // read+decrypt+flash-write burst per call, ~400 of them back to back over a
    // 3.2 MB image, with NO yield point anywhere in this loop or inside IDF's own
    // esp_https_ota_perform(). Each burst is exactly the class of stretch the
    // cyan-flash rule warns about, just repeated instead of one-off.
    //
    // A single explicit yield per chunk is the fix, not a smaller buffer_size -
    // shrinking it would only trade this bug for the "Out of buffer" one it was
    // added to solve. 1 tick (1 ms @ CONFIG_FREERTOS_HZ=1000) is enough to hand
    // the scheduler a real gap without measurably slowing a background download.
    int total = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        s_pct = (total > 0) ? (int)((int64_t)done * 100 / total) : 0;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (err != ESP_OK) {
        set_failed("download stopped early (0x%x)", err);
        esp_https_ota_abort(h);
        goto out;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        // Truncated but reported OK - refuse rather than switch the boot slot.
        set_failed("the download was incomplete");
        esp_https_ota_abort(h);
        goto out;
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        set_failed("the downloaded firmware was rejected (0x%x)", err);
        goto out;
    }

    s_pct   = 100;
    s_state = OTA_DONE;
    ESP_LOGW(TAG, "update written and verified - waiting for the operator to restart");

out:
    dsp_set_transfer_quiet(false);
    webserver_ws_set_paused(false);
    vTaskDelete(NULL);
}

bool ota_update_start(const char *url, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';

    if (s_state == OTA_RUNNING) {
        if (err) snprintf(err, err_len, "An update is already running");
        return false;
    }
    if (s_state == OTA_DONE) {
        if (err) snprintf(err, err_len, "An update is ready - restart to use it");
        return false;
    }
    if (!url || !url[0]) {
        if (err) snprintf(err, err_len, "No download address");
        return false;
    }
    if (!wifi_is_connected()) {
        if (err) snprintf(err, err_len, "No WiFi connection");
        return false;
    }

    // ⛔ Never interrupt a transmission or a contact. Applying this ends with a
    // reboot, and the operator may be several messages into an exchange.
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) {
        if (err) snprintf(err, err_len, "Transmitting - try again when TX is idle");
        return false;
    }
    char busy_with[32];
    if (ft8_qso_is_busy(busy_with, sizeof(busy_with))) {
        if (err) snprintf(err, err_len, "In a QSO with %s - finish it first", busy_with);
        return false;
    }

    char why[96];
    if (!source_allowed(url, why, sizeof(why))) {
        if (err) snprintf(err, err_len, "%s", why);
        return false;
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_msg[0] = '\0';
    s_pct    = 0;
    s_state  = OTA_RUNNING;

    // ⛔ INTERNAL STACK, NOT PSRAM - and this is not a preference.
    //
    // Writing flash disables the cache, and a task whose stack lives in PSRAM
    // cannot execute with the cache off. IDF asserts on exactly that:
    //   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
    //                  cache_utils.c:152
    // Measured here 2026-08-20 with a PSRAM stack: the download reached 96% and
    // the device panicked on the task that was writing. This project's default
    // is a PSRAM stack for background work (util/psram_task.h), which is right
    // for every other background task and WRONG for any task that touches
    // flash. 8 KB internal, held only for the duration of the update.
    BaseType_t rc = xTaskCreatePinnedToCore(
        ota_task, "ota", 8192, NULL, tskIDLE_PRIORITY + 2, NULL, 1);
    if (rc != pdPASS) {
        s_state = OTA_IDLE;
        if (err) snprintf(err, err_len, "Could not start the update task");
        return false;
    }
    return true;
}

ota_state_t ota_update_get_state(int *pct, char *msg, size_t msg_len)
{
    if (pct) *pct = s_pct;
    if (msg && msg_len) { strncpy(msg, s_msg, msg_len - 1); msg[msg_len - 1] = '\0'; }
    return s_state;
}

bool ota_update_reboot_pending(void) { return s_state == OTA_DONE; }

void ota_update_get_target_version(char *out, size_t out_sz)
{
    if (!out || !out_sz) return;
    strncpy(out, s_target_ver, out_sz - 1);
    out[out_sz - 1] = '\0';
}
