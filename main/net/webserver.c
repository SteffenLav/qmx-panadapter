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
#include "storage/sd_archive.h"  // sd_archive_is_mounted / sd_archive_log_path / lock / unlock
#include "adif/qrz_upload.h"  // qrz_upload_pending
#include "adif/eqsl_upload.h" // eqsl_upload_pending
#include "settings.h"          // settings_load_all / settings_set_qrz_api_key
#include "config_io.h"         // config_io_export / config_io_import
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps / vTaskDeleteWithCaps
#include "freertos/queue.h"
#include <string.h>
#include <time.h>

static const char *TAG = "webserver";

static httpd_handle_t s_server = NULL;

// Background upload task: processes QRZ/eQSL uploads without blocking httpd.
// Runs at priority 3 (below audio/FT8, above idle). Clients poll /api/upload_status
// to check results instead of blocking the request handler.
typedef enum { UPLOAD_QRZ, UPLOAD_EQSL } upload_kind_t;
typedef struct {
    upload_kind_t kind;
} upload_request_t;

static QueueHandle_t s_upload_queue = NULL;
static TaskHandle_t  s_upload_task = NULL;

// Last upload result (protected by static var, single slot - ok for occasional uploads)
static struct {
    upload_kind_t kind;
    int uploaded;
    int failed;
    char error[80];
    bool busy;
} s_last_upload = {0};
static SemaphoreHandle_t s_upload_mutex = NULL;

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
                cat_request_ssb_bandwidth(bw);  // SSB: three-write recipe via poll task
            } else {
                cat_request_cw_passband(bw);    // CW: MMCW menu item (Kenwood FW is rejected)
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

    // Pause the spectrum stream so this download has the WiFi TX path to itself.
    webserver_ws_set_paused(true);

    size_t n = diag_log_size();
    esp_err_t err;
    if (n == 0) {
        // Always-on capture, so this is rare (only right after a clear before
        // anything new is logged).
        err = httpd_resp_sendstr(req, "(diagnostic log empty — reload after activity)\n");
    } else {
        char *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (!buf) buf = malloc(n);
        if (!buf) {
            err = httpd_resp_send_500(req);
        } else {
            size_t got = diag_log_snapshot(buf, n);
            err = httpd_resp_send(req, buf, got);
            heap_caps_free(buf);
            // Reset the ring once it's been handed off successfully so each
            // download is fresh since the last. The SD mirror keeps the full
            // history regardless (its cursor survives a clear).
            if (err == ESP_OK) diag_log_clear();
        }
    }

    webserver_ws_set_paused(false);
    return err;
}

// GET /api/log/saved — download the persisted diagnostic log. Unlike /api/log
// (the live PSRAM ring, wiped on reboot), this survives power-off — the POTA
// "log in the field, download at home" path. Prefers the SD card's full
// qmx-log.txt when a card is mounted (read under the sd_archive lock so it
// can't race the mirror task's writes); otherwise falls back to the smaller
// flash-persisted copy (/spiffs/diag.log).
static esp_err_t stream_file_chunks(httpd_req_t *req, const char *path, const char *empty_msg)
{
    FILE *f = fopen(path, "r");
    if (!f) return httpd_resp_sendstr(req, empty_msg);
    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && err == ESP_OK)
        err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t saved_log_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qmx-log-saved.txt");

    // Prefer the card's full log when present.
    if (sd_archive_is_mounted() && sd_archive_lock(2000)) {
        esp_err_t err = stream_file_chunks(req, sd_archive_log_path(),
                                           "(no diagnostic log on card yet)\n");
        sd_archive_unlock();
        return err;
    }
    // Fall back to the flash-persisted copy.
    return stream_file_chunks(req, diag_log_persist_path(), "(no saved diagnostic log yet)\n");
}

// GET /api/adif — download the ADIF QSO log from SPIFFS.
static esp_err_t adif_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qso.adi");

    // Pause the spectrum stream so this download has the WiFi TX path to itself.
    webserver_ws_set_paused(true);

    FILE *f = fopen(adif_log_file_path(), "r");
    if (!f) {
        esp_err_t err = httpd_resp_sendstr(req,
            "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n");
        webserver_ws_set_paused(false);
        return err;
    }

    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && err == ESP_OK)
        err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    webserver_ws_set_paused(false);
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

// POST /api/qrz_upload — queues upload, returns 202 Accepted immediately.
// Client polls /api/upload_status to check results.
static esp_err_t qrz_upload_handler(httpd_req_t *req)
{
    if (!s_upload_queue) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload task not ready");
    }

    xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
    if (s_last_upload.busy) {
        xSemaphoreGive(s_upload_mutex);
        httpd_resp_set_status(req, "423 Locked");
        return httpd_resp_sendstr(req, "upload in progress");
    }
    s_last_upload.busy = true;
    s_last_upload.kind = UPLOAD_QRZ;
    s_last_upload.uploaded = 0;
    s_last_upload.failed = 0;
    s_last_upload.error[0] = '\0';
    xSemaphoreGive(s_upload_mutex);

    upload_request_t up = { .kind = UPLOAD_QRZ };
    if (!xQueueSend(s_upload_queue, &up, 0)) {
        xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
        s_last_upload.busy = false;
        xSemaphoreGive(s_upload_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "queue full");
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"uploading\"}");
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

// POST /api/eqsl_upload — queues upload, returns 202 Accepted immediately.
// Client polls /api/upload_status to check results.
static esp_err_t eqsl_upload_handler(httpd_req_t *req)
{
    if (!s_upload_queue) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload task not ready");
    }

    xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
    if (s_last_upload.busy) {
        xSemaphoreGive(s_upload_mutex);
        httpd_resp_set_status(req, "423 Locked");
        return httpd_resp_sendstr(req, "upload in progress");
    }
    s_last_upload.busy = true;
    s_last_upload.kind = UPLOAD_EQSL;
    s_last_upload.uploaded = 0;
    s_last_upload.failed = 0;
    s_last_upload.error[0] = '\0';
    xSemaphoreGive(s_upload_mutex);

    upload_request_t up = { .kind = UPLOAD_EQSL };
    if (!xQueueSend(s_upload_queue, &up, 0)) {
        xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
        s_last_upload.busy = false;
        xSemaphoreGive(s_upload_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "queue full");
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"uploading\"}");
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
    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
static const httpd_uri_t uri_log_saved = {
    .uri = "/api/log/saved", .method = HTTP_GET, .handler = saved_log_handler,
};

// GET /api/upload_status — check result of last QRZ or eQSL upload
static esp_err_t upload_status_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        xSemaphoreGive(s_upload_mutex);
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root, "busy", s_last_upload.busy);
    if (!s_last_upload.busy && s_last_upload.kind != 0) {
        cJSON_AddStringToObject(root, "kind", s_last_upload.kind == UPLOAD_QRZ ? "qrz" : "eqsl");
        cJSON_AddNumberToObject(root, "uploaded", s_last_upload.uploaded);
        cJSON_AddNumberToObject(root, "failed",   s_last_upload.failed);
        cJSON_AddStringToObject(root, "error",    s_last_upload.error);
    }
    xSemaphoreGive(s_upload_mutex);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static const httpd_uri_t uri_upload_status = {
    .uri = "/api/upload_status", .method = HTTP_GET, .handler = upload_status_handler,
};

// Background upload task — processes QRZ/eQSL uploads without blocking httpd.
// Results stored in s_last_upload for polling via /api/upload_status.
static void upload_task(void *arg)
{
    (void)arg;
    upload_request_t up;

    while (xQueueReceive(s_upload_queue, &up, portMAX_DELAY) == pdTRUE) {
        // Free the CPU + TX path for the outbound TLS connection for the whole
        // upload. dsp_set_transfer_quiet stops fft_task (pri 4) preempting this
        // upload task (pri 3) and cascades FT8 to idle; the WS pause yields the
        // single SDIO->C6 link from the ~10 fps stream. The SD archive lock
        // blocks the SD-mirror task's periodic FatFs writes (diag log every
        // ~3s, plus an ADIF mirror right after the QSO that's usually what
        // triggered this upload) - both that traffic and the WiFi C6 link
        // share one physical SDMMC host on the ESP32-P4, and overlapping
        // SDMMC activity on both slots during a sustained HTTPS upload has
        // corrupted the WiFi RPC link permanently (no auto-recovery) in the
        // field. Best-effort: if the SD task is wedged the upload still
        // proceeds, just without this protection. All three resumed/released
        // in every case below.
        dsp_set_transfer_quiet(true);
        webserver_ws_set_paused(true);
        bool sd_locked = sd_archive_lock(5000);
        if (up.kind == UPLOAD_QRZ) {
            qrz_upload_result_t result;
            qrz_upload_pending(&result);
            xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
            s_last_upload.uploaded = result.uploaded;
            s_last_upload.failed = result.failed;
            strncpy(s_last_upload.error, result.error, sizeof(s_last_upload.error) - 1);
            s_last_upload.error[sizeof(s_last_upload.error) - 1] = '\0';
            s_last_upload.busy = false;
            xSemaphoreGive(s_upload_mutex);
        } else if (up.kind == UPLOAD_EQSL) {
            eqsl_upload_result_t result;
            eqsl_upload_pending(&result);
            xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
            s_last_upload.uploaded = result.uploaded;
            s_last_upload.failed = result.failed;
            strncpy(s_last_upload.error, result.error, sizeof(s_last_upload.error) - 1);
            s_last_upload.error[sizeof(s_last_upload.error) - 1] = '\0';
            s_last_upload.busy = false;
            xSemaphoreGive(s_upload_mutex);
        }
        if (sd_locked) sd_archive_unlock();
        webserver_ws_set_paused(false);
        dsp_set_transfer_quiet(false);
    }
}

esp_err_t webserver_start(void)
{
    if (s_server != NULL) { ESP_LOGD(TAG, "Already running"); return ESP_OK; }

    // Create background upload queue + task + mutex (priority 3: below audio/FT8, above idle)
    if (!s_upload_mutex) {
        s_upload_mutex = xSemaphoreCreateMutex();
        if (!s_upload_mutex) {
            ESP_LOGE(TAG, "Could not create upload mutex");
            return ESP_FAIL;
        }
    }
    if (!s_upload_queue) {
        s_upload_queue = xQueueCreate(1, sizeof(upload_request_t));
        if (!s_upload_queue) {
            ESP_LOGE(TAG, "Could not create upload queue");
            vSemaphoreDelete(s_upload_mutex);
            s_upload_mutex = NULL;
            return ESP_FAIL;
        }
    }
    if (!s_upload_task) {
        // Allocate the task stack from PSRAM (not the scarce internal DRAM that
        // SDIO/USB DMA depend on). The upload task only does network/TLS work,
        // never runs in ISR context, so a PSRAM stack is safe here.
        if (xTaskCreateWithCaps(upload_task, "upload", 8192, NULL, 3, &s_upload_task,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Could not create upload task");
            vQueueDelete(s_upload_queue);
            s_upload_queue = NULL;
            vSemaphoreDelete(s_upload_mutex);
            s_upload_mutex = NULL;
            return ESP_FAIL;
        }
    }

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.stack_size      = 12288;
    config.max_uri_handlers = 17;
    config.lru_purge_enable = true;
    // LWIP_MAX_SOCKETS is 16; httpd reserves 3, so up to 13 sessions are safe.
    // Give the browser headroom (WS + /api polls + reconnect bursts) so a stale
    // session can be LRU-purged instead of bouncing new connects off ENFILE.
    config.max_open_sockets = 10;

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
    httpd_register_uri_handler(s_server, &uri_log_saved);
    httpd_register_uri_handler(s_server, &uri_adif_get);
    httpd_register_uri_handler(s_server, &uri_adif_clear);
    httpd_register_uri_handler(s_server, &uri_qrz_key);
    httpd_register_uri_handler(s_server, &uri_qrz_upload);
    httpd_register_uri_handler(s_server, &uri_upload_status);
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

    if (s_upload_task) {
        // Created with xTaskCreateWithCaps -> must free the PSRAM stack with the
        // matching deleter.
        vTaskDeleteWithCaps(s_upload_task);
        s_upload_task = NULL;
    }
    if (s_upload_queue) {
        vQueueDelete(s_upload_queue);
        s_upload_queue = NULL;
    }
    if (s_upload_mutex) {
        vSemaphoreDelete(s_upload_mutex);
        s_upload_mutex = NULL;
    }
}
