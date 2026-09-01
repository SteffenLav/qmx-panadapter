// See mdns_svc.h.
//
// Announces the Tab5 as "qmx.local" and advertises its web server, so an
// operator never has to know the IP address. That matters more here than on a
// typical device: this one roams between remembered networks (wifi.c), so its
// address changes without anyone deciding it should - a hotel lease one evening,
// a phone hotspot the next.

#include "mdns_svc.h"

#include "mdns.h"
#include "esp_log.h"
#include "esp_app_desc.h"

#include <string.h>

static const char *TAG = "mdns";

static bool s_up = false;

void mdns_svc_start(void)
{
    // Idempotent: the got-IP path calls this on every connect, including the
    // reconnects and roams that are routine on this device. mdns_init() would
    // return an error on the second call, and the service list would be
    // duplicated - so own that here rather than making every caller remember.
    if (s_up) {
        ESP_LOGD(TAG, "already running");
        return;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        // Not fatal, and deliberately not retried in a loop: the web server is
        // still reachable by IP, and the bottom bar shows it. A name is a
        // convenience, not a dependency.
        ESP_LOGW(TAG, "mdns_init failed (%s) - reachable by IP only", esp_err_to_name(err));
        return;
    }

    if ((err = mdns_hostname_set(MDNS_SVC_HOSTNAME)) != ESP_OK) {
        ESP_LOGW(TAG, "hostname set failed (%s)", esp_err_to_name(err));
        mdns_free();
        return;
    }
    mdns_instance_name_set("QMX Panadapter");

    // Advertise the web UI. Browsers and OS service browsers list it, which is
    // how someone finds the device without being told anything at all.
    const esp_app_desc_t *app = esp_app_get_description();
    mdns_txt_item_t txt[] = {
        { "path", "/" },
        { "fw",   app ? app->version : "" },
    };
    if ((err = mdns_service_add(NULL, "_http", "_tcp", 80, txt,
                                sizeof(txt) / sizeof(txt[0]))) != ESP_OK) {
        // The name still resolves without the service record, so this is worth a
        // line but not worth tearing mDNS down for.
        ESP_LOGW(TAG, "service advert failed (%s) - name still resolves", esp_err_to_name(err));
    }

    s_up = true;
    ESP_LOGI(TAG, "responder up: http://%s.local (and its IP, as before)", MDNS_SVC_HOSTNAME);
}

bool mdns_svc_is_up(void)
{
    return s_up;
}
