#include "webserver.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "battery.h"     // battery_get_level, battery_is_charging
#include "wifi.h"        // wifi_get_ssid, wifi_get_rssi_dbm, wifi_get_ip
#include "cat.h"         // cat_get_frequency

static const char *TAG = "webserver";

static httpd_handle_t s_server = NULL;

// index.html is embedded as a null-terminated string via EMBED_TXTFILES.
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_handler(httpd_req_t *req)
{
    const size_t len = index_html_end - index_html_start - 1;  // strip NUL
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }

    cJSON *batt = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNumberToObject(batt, "level",    battery_get_level());
    cJSON_AddBoolToObject  (batt, "charging", battery_is_charging());

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", wifi_get_ssid());
    cJSON_AddNumberToObject(wifi, "rssi", wifi_get_rssi_dbm());
    cJSON_AddStringToObject(wifi, "ip",   wifi_get_ip());

    cJSON_AddNumberToObject(root, "freq_hz", (double)cat_get_frequency());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static const httpd_uri_t uri_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = root_handler,
};

static const httpd_uri_t uri_status = {
    .uri      = "/api/status",
    .method   = HTTP_GET,
    .handler  = status_handler,
};

esp_err_t webserver_start(void)
{
    if (s_server != NULL) {
        ESP_LOGD(TAG, "Already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size  = 8192;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_status);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void webserver_stop(void)
{
    if (s_server == NULL) {
        return;
    }
    ESP_LOGI(TAG, "Stopping HTTP server");
    httpd_stop(s_server);
    s_server = NULL;
}