#include "webserver.h"
#include "webserver_ws.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "battery.h"          // battery_get_level, battery_is_charging
#include "wifi.h"             // wifi_get_ssid, wifi_get_rssi_dbm, wifi_get_ip
#include "cat.h"              // cat_get_frequency, cat_get_band_list, cat_set_*
#include "ui.h"               // ui_get_*, ui_set_zoom
#include "dsp.h"              // dsp_get_peak_dbm_around_vfo
#include "display/display.h"  // display_lock / display_unlock
#include "screenshot/screenshot.h"  // screenshot_capture_rgb565
#include "diag_log.h"         // diag_log_size / diag_log_snapshot
#include "adif/adif_log.h"    // adif_log_count / adif_log_file_path / adif_log_clear
#include "adif/qrz_upload.h"  // qrz_upload_pending
#include "adif/eqsl_upload.h" // eqsl_upload_pending
#include "settings.h"          // settings_load_all / settings_set_qrz_api_key
#include "config_io.h"         // config_io_export / config_io_import
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include <string.h>
#include <time.h>

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
    if (!root) return httpd_resp_send_500(req);

    cJSON *batt = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNumberToObject(batt, "level",    battery_get_level());
    cJSON_AddNumberToObject(batt, "mv",       battery_get_mv());
    cJSON_AddBoolToObject  (batt, "charging", battery_is_charging());

    cJSON *wifi_obj = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi_obj, "ssid", wifi_get_ssid());
    cJSON_AddNumberToObject(wifi_obj, "rssi", wifi_get_rssi_dbm());
    cJSON_AddStringToObject(wifi_obj, "ip",   wifi_get_ip());

    cJSON_AddNumberToObject(root, "freq_hz",     (double)cat_get_frequency());
    cJSON_AddStringToObject(root, "qmx_fw",       cat_get_qmx_fw());
    cJSON_AddStringToObject(root, "mode",         ui_get_mode_str());
    cJSON_AddStringToObject(root, "band",         ui_get_band_str());
    // Apply mode defaults if CAT has not yet reported BW (matches Tab5 compute_passband_edges_hz)
    {
        uint32_t bw = ui_get_passband_width_hz();
        if (bw == 0) {
            const char *m = ui_get_mode_str();
            if      (strstr(m, "CW"))   bw = 300;
            else if (strstr(m, "AM"))   bw = 6000;
            else if (strstr(m, "FM"))   bw = 10000;
            else                        bw = 2700;  // USB/LSB/DiGi
        }
        cJSON_AddNumberToObject(root, "passband_hz", (double)bw);
    }

    float peak_dbm = -999.0f;
    int vfo_bin = ((ui_get_if_bin_shift(DSP_FFT_SIZE) % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE;
    if (dsp_get_peak_dbm_around_vfo(vfo_bin, 64, &peak_dbm) == ESP_OK)
        cJSON_AddNumberToObject(root, "signal_dbm", (double)peak_dbm);
    else
        cJSON_AddNullToObject(root, "signal_dbm");

    cJSON_AddNumberToObject(root, "zoom",        (double)ui_get_zoom_factor());
    cJSON_AddNumberToObject(root, "pan_bins",    (double)ui_get_pan_offset_bins());
    cJSON_AddNumberToObject(root, "cw_pitch_hz", (double)ui_get_cw_pitch_hz());
    cJSON_AddNumberToObject(root, "if_cal_hz",   (double)ui_get_if_cal_hz());
    cJSON_AddBoolToObject  (root, "flat_mode",   ui_get_flat_mode());
    cJSON_AddNumberToObject(root, "utc_epoch",   (double)time(NULL));
    cJSON_AddNumberToObject(root, "qso_count",   (double)adif_log_count());
    {
        qmx_settings_t cfg;
        settings_load_all(&cfg);
        cJSON_AddBoolToObject(root, "qrz_key_set", cfg.qrz_api_key[0] != '\0');
        cJSON_AddBoolToObject(root, "eqsl_creds_set", cfg.eqsl_user[0] != '\0' && cfg.eqsl_pswd[0] != '\0');
    }
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "tab5_fw",     app ? app->version : "");

    int band_count = 0;
    const cat_band_entry_t *bands = cat_get_band_list(&band_count);
    cJSON *band_arr = cJSON_AddArrayToObject(root, "bands");
    for (int i = 0; i < band_count; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name",      bands[i].name);
        cJSON_AddNumberToObject(b, "center_hz", (double)bands[i].center_hz);
        cJSON_AddItemToArray(band_arr, b);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));

    if (action && strcmp(action, "set_freq") == 0) {
        cJSON *item = cJSON_GetObjectItem(root, "hz");
        if (cJSON_IsNumber(item)) cat_set_frequency((uint32_t)item->valuedouble);
    } else if (action && strcmp(action, "set_band") == 0) {
        cJSON *item = cJSON_GetObjectItem(root, "hz");
        if (cJSON_IsNumber(item)) {
            uint32_t center_hz = (uint32_t)item->valuedouble;
            uint32_t target    = ui_band_last_hz(center_hz);
            cat_set_frequency(target ? target : center_hz);
        }
    } else if (action && strcmp(action, "set_mode") == 0) {
        const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
        if (mode) cat_set_mode(mode);
    } else if (action && strcmp(action, "set_bw") == 0) {
        cJSON *item = cJSON_GetObjectItem(root, "hz");
        if (cJSON_IsNumber(item)) {
            uint32_t bw = (uint32_t)item->valuedouble;
            if (bw >= 1000) {
                cat_request_ssb_bandwidth(bw);  // uses the three-write recipe for SSB
            } else {
                cat_set_passband_hz(bw);
            }
        }
    } else if (action && strcmp(action, "set_zoom") == 0) {
        cJSON *item = cJSON_GetObjectItem(root, "zoom");
        if (cJSON_IsNumber(item)) {
            if (display_lock(50)) {
                ui_set_zoom((float)item->valuedouble, 0);
                display_unlock();
            }
        }
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Minimal 16bpp BMP (BITMAPFILEHEADER + BITMAPINFOHEADER + BI_BITFIELDS masks)
// wrapping the raw RGB565 framebuffer snapshot. Top-down (negative height) so
// the LVGL row order can be sent as-is.
static esp_err_t ss_bmp_handler(httpd_req_t *req)
{
    uint8_t *buf;
    size_t size;
    uint32_t w, h;
    if (screenshot_capture_rgb565(&buf, &size, &w, &h) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    uint8_t header[66] = {0};
    header[0] = 'B';
    header[1] = 'M';

    uint32_t file_size = (uint32_t)(sizeof(header) + size);
    uint32_t off_bits  = sizeof(header);
    uint32_t info_size = 40;
    int32_t  width     = (int32_t)w;
    int32_t  height    = -(int32_t)h;  // negative = top-down DIB
    uint16_t planes    = 1;
    uint16_t bpp       = 16;
    uint32_t comp      = 3;  // BI_BITFIELDS
    uint32_t img_size  = (uint32_t)size;
    uint32_t r_mask    = 0xF800;
    uint32_t g_mask    = 0x07E0;
    uint32_t b_mask    = 0x001F;

    memcpy(&header[2],  &file_size, 4);
    memcpy(&header[10], &off_bits,  4);
    memcpy(&header[14], &info_size, 4);
    memcpy(&header[18], &width,     4);
    memcpy(&header[22], &height,    4);
    memcpy(&header[26], &planes,    2);
    memcpy(&header[28], &bpp,       2);
    memcpy(&header[30], &comp,      4);
    memcpy(&header[34], &img_size,  4);
    memcpy(&header[54], &r_mask,    4);
    memcpy(&header[58], &g_mask,    4);
    memcpy(&header[62], &b_mask,    4);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=ss.bmp");

    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, (const char *)buf, size);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    heap_caps_free(buf);
    return err;
}

// GET /api/log — download the diagnostic ring buffer as a text file.
// Empty (or capture disabled) returns a short hint instead of a blank file.
static esp_err_t log_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qmx-log.txt");

    size_t n = diag_log_size();
    if (n == 0) {
        const char *hint =
            "(diagnostic log empty)\n"
            "Enable 'Diagnostic log' in the Tab5 settings drawer, reproduce the "
            "issue, then reload this page.\n";
        return httpd_resp_sendstr(req, hint);
    }

    char *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(n);
    if (!buf) return httpd_resp_send_500(req);

    size_t got = diag_log_snapshot(buf, n);
    esp_err_t err = httpd_resp_send(req, buf, got);
    heap_caps_free(buf);
    return err;
}

// GET /api/adif — download the ADIF QSO log from SPIFFS.
static esp_err_t adif_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qso.adi");

    FILE *f = fopen(adif_log_file_path(), "r");
    if (!f) {
        return httpd_resp_sendstr(req,
            "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n");
    }

    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && err == ESP_OK)
        err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// POST /api/adif/clear — erase all logged QSOs.
static esp_err_t adif_clear_handler(httpd_req_t *req)
{
    adif_log_clear();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_adif_get = {
    .uri = "/api/adif", .method = HTTP_GET, .handler = adif_get_handler,
};
static const httpd_uri_t uri_adif_clear = {
    .uri = "/api/adif/clear", .method = HTTP_POST, .handler = adif_clear_handler,
};

// POST /api/qrz_key — body is the raw API key text (no JSON wrapper, the web
// UI just sends the plain key string from a prompt()).
static esp_err_t qrz_key_handler(httpd_req_t *req)
{
    char buf[48];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    settings_set_qrz_api_key(buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/qrz_upload — uploads every ADIF record logged since the last
// successful QRZ upload. Blocks on the network for the whole batch (this
// handler runs on the httpd worker task, not the LVGL thread, so it's safe
// to block here).
static esp_err_t qrz_upload_handler(httpd_req_t *req)
{
    qrz_upload_result_t result;
    qrz_upload_pending(&result);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddNumberToObject(root, "uploaded", result.uploaded);
    cJSON_AddNumberToObject(root, "failed",   result.failed);
    cJSON_AddStringToObject(root, "error",    result.error);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static const httpd_uri_t uri_qrz_key = {
    .uri = "/api/qrz_key", .method = HTTP_POST, .handler = qrz_key_handler,
};
static const httpd_uri_t uri_qrz_upload = {
    .uri = "/api/qrz_upload", .method = HTTP_POST, .handler = qrz_upload_handler,
};

// POST /api/eqsl_creds — JSON body {"user":"...","pswd":"..."}. eQSL has no
// API-key scheme, so unlike QRZ this needs two fields.
static esp_err_t eqsl_creds_handler(httpd_req_t *req)
{
    char buf[160];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(root, "user"));
    const char *pswd = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pswd"));
    settings_set_eqsl_user(user);
    settings_set_eqsl_pswd(pswd);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/eqsl_upload — uploads every ADIF record logged since the last
// successful eQSL upload, in batches. Blocks on the network for the whole
// run (httpd worker task, not the LVGL thread - safe to block here).
static esp_err_t eqsl_upload_handler(httpd_req_t *req)
{
    eqsl_upload_result_t result;
    eqsl_upload_pending(&result);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddNumberToObject(root, "uploaded", result.uploaded);
    cJSON_AddNumberToObject(root, "failed",   result.failed);
    cJSON_AddStringToObject(root, "error",    result.error);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static const httpd_uri_t uri_eqsl_creds = {
    .uri = "/api/eqsl_creds", .method = HTTP_POST, .handler = eqsl_creds_handler,
};
static const httpd_uri_t uri_eqsl_upload = {
    .uri = "/api/eqsl_upload", .method = HTTP_POST, .handler = eqsl_upload_handler,
};

// GET /api/config — download all settings + memory channels as an editable
// INI text file (qmx-config.txt). Includes secrets (wifi_pass/qrz/eqsl) so it
// works as a full backup; the file is the user's to keep private.
static esp_err_t config_get_handler(httpd_req_t *req)
{
    size_t len = 0;
    char *body = config_io_export(&len);
    if (!body) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qmx-config.txt");
    esp_err_t err = httpd_resp_send(req, body, len);
    free(body);
    return err;
}

// POST /api/config — upload an INI config file; merge it into NVS.
static esp_err_t config_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 32768) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad/empty body");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) return httpd_resp_send_500(req);
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    int applied = config_io_import(body);   // mutates body in place
    free(body);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddNumberToObject(root, "applied", applied);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static const httpd_uri_t uri_config_get = {
    .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler,
};
static const httpd_uri_t uri_config_post = {
    .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler,
};

static const httpd_uri_t uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = root_handler,
};
static const httpd_uri_t uri_status = {
    .uri = "/api/status", .method = HTTP_GET, .handler = status_handler,
};
static const httpd_uri_t uri_cmd = {
    .uri = "/api/cmd", .method = HTTP_POST, .handler = cmd_handler,
};
static const httpd_uri_t uri_ss_bmp = {
    .uri = "/ss.bmp", .method = HTTP_GET, .handler = ss_bmp_handler,
};
static const httpd_uri_t uri_log = {
    .uri = "/api/log", .method = HTTP_GET, .handler = log_handler,
};

esp_err_t webserver_start(void)
{
    if (s_server != NULL) { ESP_LOGD(TAG, "Already running"); return ESP_OK; }

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.stack_size      = 12288;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_cmd);
    httpd_register_uri_handler(s_server, &uri_ss_bmp);
    httpd_register_uri_handler(s_server, &uri_log);
    httpd_register_uri_handler(s_server, &uri_adif_get);
    httpd_register_uri_handler(s_server, &uri_adif_clear);
    httpd_register_uri_handler(s_server, &uri_qrz_key);
    httpd_register_uri_handler(s_server, &uri_qrz_upload);
    httpd_register_uri_handler(s_server, &uri_eqsl_creds);
    httpd_register_uri_handler(s_server, &uri_eqsl_upload);
    httpd_register_uri_handler(s_server, &uri_config_get);
    httpd_register_uri_handler(s_server, &uri_config_post);
    webserver_ws_start(s_server);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void webserver_stop(void)
{
    if (s_server == NULL) return;
    ESP_LOGI(TAG, "Stopping HTTP server");
    webserver_ws_stop();
    httpd_stop(s_server);
    s_server = NULL;
}
