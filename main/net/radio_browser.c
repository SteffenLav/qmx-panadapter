// radio-browser.info client. See radio_browser.h for why this directory.
#include "radio_browser.h"

#include "storage/settings.h"
#include "ft8/js8_message.h"      /* js8_grid_to_latlon - the only position we trust */

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "radio_br";

static char s_err[80] = "";

/* ⛔ PSRAM, AND BIG. A geo search for 60 stations comes back as a few hundred
 * kilobytes of JSON - every row carries tags, homepage, favicon and a dozen
 * fields we throw away. This cannot go on a task stack and must not go in
 * internal RAM, which this board runs within ~30 KB of (#65). */
#define RB_BUF_BYTES (320 * 1024)

/* How many rows to ASK for, against how many we keep. The directory returns
 * duplicates and AAC rows that we drop, so asking for exactly RB_MAX_STATIONS
 * would hand back a short list after filtering. */
#define RB_FETCH_ROWS 60

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} rb_sink_t;

static esp_err_t http_ev(esp_http_client_event_t *evt)
{
    rb_sink_t *sink = (rb_sink_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !sink || !sink->buf) return ESP_OK;
    /* Truncate rather than overflow. A cut JSON document fails to parse and is
     * reported as such, which is a better outcome than a heap overrun. */
    size_t room = (sink->cap > sink->len + 1) ? (sink->cap - sink->len - 1) : 0;
    size_t n    = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    if (n) {
        memcpy(sink->buf + sink->len, evt->data, n);
        sink->len += n;
        sink->buf[sink->len] = '\0';
    }
    return ESP_OK;
}

/* Upper-cased, spaces and punctuation dropped: "DR P6 Beat" and "DR P6 BEAT"
 * are the same station listed twice, and both are in the real data. */
static void norm_name(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) out[j++] = (char)toupper(c);
    }
    out[j] = '\0';
}

const char *radio_browser_last_error(void) { return s_err; }

int radio_browser_fetch(rb_station_t *out, int max, int radius_km)
{
    s_err[0] = '\0';
    if (!out || max <= 0) return -3;
    if (max > RB_MAX_STATIONS) max = RB_MAX_STATIONS;

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (!cfg.my_grid[0]) {
        snprintf(s_err, sizeof(s_err), "Set your grid square first");
        return -1;
    }

    float lat = 0.0f, lon = 0.0f;
    js8_grid_to_latlon(cfg.my_grid, &lat, &lon);
    if (lat == 0.0f && lon == 0.0f) {
        snprintf(s_err, sizeof(s_err), "'%s' is not a grid I can read", cfg.my_grid);
        return -1;
    }

    char *buf = heap_caps_malloc(RB_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(s_err, sizeof(s_err), "No PSRAM for the station list");
        return -2;
    }
    buf[0] = '\0';
    rb_sink_t sink = { .buf = buf, .cap = RB_BUF_BYTES, .len = 0 };

    /* hidebroken drops the dead streams the directory already knows about;
     * ordering by clickcount puts the stations people actually listen to at
     * the top, which is what makes a 24-row cap usable. */
    char url[320];
    snprintf(url, sizeof(url),
             "https://de1.api.radio-browser.info/json/stations/search"
             "?geo_lat=%.4f&geo_long=%.4f&geo_distance=%d"
             "&hidebroken=true&order=clickcount&reverse=true&limit=%d",
             lat, lon, radius_km * 1000, RB_FETCH_ROWS);

    esp_http_client_config_t hc = {
        .url                   = url,
        .event_handler         = http_ev,
        .user_data             = &sink,
        .timeout_ms            = 15000,
        .crt_bundle_attach     = NULL,
        .skip_cert_common_name_check = true,
        /* ⛔ NO CERTIFICATE BUNDLE ON PURPOSE. Attaching one costs ~64 KB of
         * flash and the payload is a public list of radio station URLs - there
         * is nothing here worth authenticating a server for, and a failed
         * chain would turn a list refresh into a support question. Nothing
         * from this reply is executed; it is parsed as data and the only field
         * that leaves is a URL the operator then chooses to play. */
        .transport_type        = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size           = 4096,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    if (!cl) {
        free(buf);
        snprintf(s_err, sizeof(s_err), "Could not start the request");
        return -2;
    }
    esp_http_client_set_header(cl, "User-Agent", "qmx-panadapter/1.0");

    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    if (err != ESP_OK || status != 200) {
        snprintf(s_err, sizeof(s_err), "Directory unreachable (%s, HTTP %d)",
                 esp_err_to_name(err), status);
        ESP_LOGW(TAG, "fetch failed: %s status=%d", esp_err_to_name(err), status);
        free(buf);
        return -2;
    }
    ESP_LOGI(TAG, "fetched %u bytes for %.3f,%.3f r=%d km",
             (unsigned)sink.len, lat, lon, radius_km);

    cJSON *root = cJSON_Parse(buf);
    free(buf);                      /* parsed into its own tree - drop the text */
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        snprintf(s_err, sizeof(s_err), "The directory's reply did not parse");
        return -3;
    }

    /* Normalised names of what we have kept, for the duplicate test. */
    static char seen[RB_MAX_STATIONS][RB_NAME_LEN];
    int n = 0;

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, root) {
        if (n >= max) break;

        const cJSON *jcodec = cJSON_GetObjectItemCaseSensitive(it, "codec");
        if (!cJSON_IsString(jcodec) || strcasecmp(jcodec->valuestring, "MP3") != 0)
            continue;                       /* Helix decodes MP3 and nothing else */

        const cJSON *jname = cJSON_GetObjectItemCaseSensitive(it, "name");
        const cJSON *jurl  = cJSON_GetObjectItemCaseSensitive(it, "url_resolved");
        if (!cJSON_IsString(jurl) || !jurl->valuestring[0])
            jurl = cJSON_GetObjectItemCaseSensitive(it, "url");
        if (!cJSON_IsString(jname) || !cJSON_IsString(jurl)) continue;
        if (!jname->valuestring[0] || !jurl->valuestring[0]) continue;
        /* A URL longer than the field is a DIFFERENT stream once truncated,
         * so it is skipped rather than shortened - the same rule the callsign
         * codec states for a name it cannot carry. */
        if (strlen(jurl->valuestring) >= RB_URL_LEN) continue;

        char key[RB_NAME_LEN];
        norm_name(jname->valuestring, key, sizeof(key));
        if (!key[0]) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = (strcmp(seen[i], key) == 0);
        if (dup) continue;
        snprintf(seen[n], RB_NAME_LEN, "%s", key);

        rb_station_t *st = &out[n];
        memset(st, 0, sizeof(*st));
        snprintf(st->name, sizeof(st->name), "%s", jname->valuestring);
        snprintf(st->url,  sizeof(st->url),  "%s", jurl->valuestring);

        const cJSON *jbr = cJSON_GetObjectItemCaseSensitive(it, "bitrate");
        if (cJSON_IsNumber(jbr)) st->bitrate_kbps = (uint16_t)jbr->valuedouble;
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "geo_distance");
        if (cJSON_IsNumber(jd)) st->distance_km = (uint16_t)(jd->valuedouble / 1000.0);
        n++;
    }
    cJSON_Delete(root);

    if (!n) snprintf(s_err, sizeof(s_err), "No MP3 stations within %d km", radius_km);
    ESP_LOGI(TAG, "kept %d station(s) of %d asked for", n, RB_FETCH_ROWS);
    return n;
}
