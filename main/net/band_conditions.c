// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
// HF band conditions from hamqsl.com - see band_conditions.h. Ported from
// the sibling rbn_monitor project's band_conditions_client.cpp, adapted
// from C++ to plain C and from that project's psram-stack task helper to
// this one's (util/psram_task.h).

#include "band_conditions.h"
#include "wifi.h"
#include "net/net_quiet.h"
#include "storage/settings.h"
#include "util/psram_task.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "band_cond";
static const char *SOLAR_URL = "https://www.hamqsl.com/solarxml.php";

const char *const BAND_COND_GROUP_NAMES[BAND_COND_GROUP_COUNT] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};

#define POLL_INTERVAL_MS  3600000  // 60 min once a fetch has succeeded (hamqsl.com itself updates roughly hourly)
#define RETRY_INTERVAL_MS 60000    // 1 min while nothing has been fetched yet
#define RX_BUF_SIZE       4096     // the whole feed is well under 2 KB

static SemaphoreHandle_t s_mutex;
static band_conditions_t s_conditions;
static bool s_have_conditions;

// ---- HTTP fetch --------------------------------------------------------

typedef struct { char *buf; int len; } rx_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        rx_ctx_t *ctx = (rx_ctx_t *)evt->user_data;
        int room = RX_BUF_SIZE - 1 - ctx->len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, n);
            ctx->len += n;
        }
    }
    return ESP_OK;
}

// Extracts the first <tag>...</tag> anywhere in xml, trimming leading
// whitespace (hamqsl's XML pads several values, e.g. "<aindex> 7</aindex>").
// No nesting/attribute support needed for the flat top-level fields this is
// used for.
static bool extract_tag(const char *xml, const char *tag, char *out, size_t out_len)
{
    char open_tag[24], close_tag[24];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) return false;
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
    if (!end || end < start) return false;
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    size_t n = (size_t)(end - start);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

// hamqsl's <calculatedconditions> block has 8 <band name="X" time="Y">
// entries (4 groups x day/night) - these aren't simple flat tags, so this
// searches for the exact opening tag (attributes and all) instead of
// reusing extract_tag().
static bool extract_band_rating(const char *xml, const char *group_name, const char *time_name,
                                 char *out, size_t out_len)
{
    char open_tag[48];
    snprintf(open_tag, sizeof(open_tag), "<band name=\"%s\" time=\"%s\">", group_name, time_name);
    const char *start = strstr(xml, open_tag);
    if (!start) return false;
    start += strlen(open_tag);
    const char *end = strstr(start, "</band>");
    if (!end || end < start) return false;
    size_t n = (size_t)(end - start);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static void parse_and_store(const char *xml)
{
    band_conditions_t c = {0};

    char field[32];
    if (extract_tag(xml, "solarflux", field, sizeof(field))) c.solar_flux = atoi(field);
    if (extract_tag(xml, "aindex", field, sizeof(field)))    c.a_index    = atoi(field);
    if (extract_tag(xml, "kindex", field, sizeof(field)))    c.k_index    = atoi(field);
    if (extract_tag(xml, "sunspots", field, sizeof(field)))  c.sunspots   = atoi(field);
    extract_tag(xml, "geomagfield", c.geomag_field, sizeof(c.geomag_field));
    extract_tag(xml, "signalnoise", c.signal_noise, sizeof(c.signal_noise));

    bool got_any_band = false;
    for (int i = 0; i < BAND_COND_GROUP_COUNT; i++) {
        if (extract_band_rating(xml, BAND_COND_GROUP_NAMES[i], "day", c.day[i], sizeof(c.day[i])))
            got_any_band = true;
        if (extract_band_rating(xml, BAND_COND_GROUP_NAMES[i], "night", c.night[i], sizeof(c.night[i])))
            got_any_band = true;
    }
    if (!got_any_band) {
        ESP_LOGW(TAG, "no band-condition entries found in response - feed format may have changed");
        return;
    }
    c.fetched_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_conditions = c;
    s_have_conditions = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "SFI=%d A=%d K=%d SN=%d %s/%s day, %s/%s night",
             c.solar_flux, c.a_index, c.k_index, c.sunspots,
             c.day[0], c.day[1], c.night[0], c.night[1]);
}

static void poll_once(char *rx_buf)
{
    rx_ctx_t ctx = { rx_buf, 0 };
    esp_http_client_config_t config = {
        .url = SOLAR_URL,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return;
    }
    rx_buf[ctx.len] = '\0';
    parse_and_store(rx_buf);
}

static void band_conditions_task(void *arg)
{
    (void)arg;
    char *rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!rx_buf) {
        ESP_LOGE(TAG, "failed to allocate response buffer");
        vTaskDelete(NULL);
        return;
    }
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    for (;;) {
        if (net_quiet_active()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        // Opt-in with the rest of the spot map - see settings.h, spotmap_en.
        // Re-read every pass so the drawer switch applies without a reboot.
        if (!settings_get_spotmap_en()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        poll_once(rx_buf);
        int next_ms = s_have_conditions ? POLL_INTERVAL_MS : RETRY_INTERVAL_MS;
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

void band_conditions_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    psram_task_create(band_conditions_task, "band_cond", 6144, NULL, 3, tskNO_AFFINITY);
}

bool band_conditions_get(band_conditions_t *out)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool have = s_have_conditions;
    if (have) *out = s_conditions;
    xSemaphoreGive(s_mutex);
    return have;
}
