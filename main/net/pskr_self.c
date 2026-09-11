// Live self-spotting via PSK Reporter's MQTT broker - see pskr_self.h. Ported
// from the sibling rbn_monitor project's pskr_self_client.cpp, adapted from
// C++ to plain C and from that project's psram-stack task helper to this
// one's (util/psram_task.h).

#include "pskr_self.h"
#include "wifi.h"
#include "net/net_quiet.h"
#include "storage/settings.h"
#include "util/maidenhead.h"
#include "util/psram_task.h"

#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

static const char *TAG = "pskr_self";
static const char *BROKER_URI = "mqtt://mqtt.pskreporter.info:1883";

// Ring buffer of the last 100 finds (operator's own cap, for overview rather
// than a resource limit - oldest evicted first) plus a manual Flush
// (pskr_self_clear()) so the map can be started fresh on demand.
#define PSKR_SELF_MAX 100
#define PSKR_SELF_TTL_S 1800   // same half-hour map lifetime as net/rbn.c's self-spots

static EXT_RAM_BSS_ATTR pskr_self_spot_t s_store[PSKR_SELF_MAX];
static int              s_count;
static SemaphoreHandle_t s_mutex;

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_subscribed_call[16];   // "" until the first successful subscribe

// ---- store -----------------------------------------------------------------

static void store_expire(int64_t now)
{
    int keep = 0;
    for (int i = 0; i < s_count; i++)
        if (now - s_store[i].heard_unix <= PSKR_SELF_TTL_S) s_store[keep++] = s_store[i];
    s_count = keep;
}

// Same dedupe-by-reporter shape as net/rbn.c's note_self_spot().
static void store_add(const pskr_self_spot_t *in)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    store_expire(in->heard_unix);
    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_store[i].call, in->call) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count < PSKR_SELF_MAX) {
            slot = s_count++;
        } else {
            slot = 0;
            for (int i = 1; i < PSKR_SELF_MAX; i++)
                if (s_store[i].heard_unix < s_store[slot].heard_unix) slot = i;
        }
    }
    s_store[slot] = *in;
    xSemaphoreGive(s_mutex);
}

int pskr_self_spots_get(pskr_self_spot_t *out, int max)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    store_expire((int64_t)time(NULL));
    int n = s_count < max ? s_count : max;
    memcpy(out, s_store, n * sizeof(pskr_self_spot_t));
    xSemaphoreGive(s_mutex);
    return n;
}

bool pskr_self_is_connected(void) { return s_connected; }

// Manual Flush (ui/spot_map_view.c's sidebar button). New self-spots keep
// arriving afterward as normal - the MQTT subscription is untouched.
void pskr_self_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_mutex);
}

// ---- MQTT --------------------------------------------------------------

// PSK Reporter's topic segments are case-sensitive; the settings field is not
// guaranteed to be typed in upper case, so every subscribe/compare goes
// through this first.
static void call_upper(const char *in, char *out, size_t out_sz)
{
    size_t i = 0;
    for (; in[i] && i + 1 < out_sz; i++) out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = '\0';
}

// pskr/filter/v2/{band}/{mode}/{tx_call}/{rx_call}/{tx_grid}/{rx_grid}/{tx_dxcc}/{rx_dxcc}
// - band and mode wildcarded (self-spotting wants every mode, not just one),
// tx_call fixed to our own callsign, everything after it wildcarded too.
static void do_subscribe(const char *call)
{
    if (!s_client || !call[0]) return;
    if (s_subscribed_call[0] && strcmp(s_subscribed_call, call) != 0) {
        char old_topic[80];
        snprintf(old_topic, sizeof(old_topic), "pskr/filter/v2/+/+/%s/+/+/+/+/+", s_subscribed_call);
        esp_mqtt_client_unsubscribe(s_client, old_topic);
        // The operator is now a different station - every spot already in the
        // ring buffer is a report about the OLD callsign and would otherwise
        // sit on ui/spot_map_view.c's map/table under the new identity until
        // its 30-minute TTL expires.
        pskr_self_clear();
        ESP_LOGI(TAG, "callsign changed (%s -> %s) - self-spot buffer cleared", s_subscribed_call, call);
    }
    char topic[80];
    snprintf(topic, sizeof(topic), "pskr/filter/v2/+/+/%s/+/+/+/+/+", call);
    esp_mqtt_client_subscribe(s_client, topic, 0);
    snprintf(s_subscribed_call, sizeof(s_subscribed_call), "%s", call);
    ESP_LOGI(TAG, "subscribed: %s", topic);
}

static void handle_payload(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "sc");   // sender (us) call
    const cJSON *rc = cJSON_GetObjectItemCaseSensitive(root, "rc");   // receiver call
    const cJSON *rl = cJSON_GetObjectItemCaseSensitive(root, "rl");   // receiver grid
    const cJSON *md = cJSON_GetObjectItemCaseSensitive(root, "md");   // mode
    const cJSON *f  = cJSON_GetObjectItemCaseSensitive(root, "f");    // frequency, Hz
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(root, "rp");   // SNR they measured on us

    pskr_self_spot_t sp = {0};
    if (cJSON_IsString(rc)) snprintf(sp.call, sizeof(sp.call), "%s", rc->valuestring);
    if (cJSON_IsString(md)) snprintf(sp.mode, sizeof(sp.mode), "%s", md->valuestring);
    if (cJSON_IsNumber(f))  sp.freq_hz = (uint32_t)f->valuedouble;
    if (cJSON_IsNumber(rp)) sp.snr_db  = (int)rp->valuedouble;
    sp.heard_unix = (int64_t)time(NULL);

    double lat, lon;
    if (cJSON_IsString(rl) && maidenhead_to_latlon(rl->valuestring, &lat, &lon)) {
        sp.lat = (float)lat;
        sp.lon = (float)lon;
        sp.has_pos = true;
    }

    // Copy sc's string out BEFORE deleting root - cJSON_GetObjectItemCaseSensitive()
    // returns a pointer INTO the tree, not a copy, so sc (like every other
    // cJSON* above) is dangling the instant root is freed. Reading
    // sc->valuestring after cJSON_Delete() below is what actually crashed
    // mqtt_task on hardware (Load access fault, reliably ~30 s in) - every
    // other field here was already copied into `sp` first for the same
    // reason, this check just wasn't.
    char sender_call[16] = {0};
    if (cJSON_IsString(sc)) snprintf(sender_call, sizeof(sender_call), "%s", sc->valuestring);

    cJSON_Delete(root);

    // Defensive - the broker should only ever deliver spots matching our
    // subscribed tx_call topic segment, but confirm before storing rather
    // than trusting that blindly.
    if (sp.call[0] && sender_call[0] && strcasecmp(sender_call, s_subscribed_call) == 0) {
        store_add(&sp);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to broker");
        s_connected = true;
        qmx_settings_t s;
        settings_load_all(&s);
        char call[16];
        call_upper(s.my_callsign, call, sizeof(call));
        s_subscribed_call[0] = '\0';   // force a fresh SUBSCRIBE, not just the change-detect path
        do_subscribe(call);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_payload(event->data, event->data_len);
        break;
    default:
        break;
    }
}

// Own callsign can change in the settings drawer without a reboot; this is
// the only thing that would otherwise go stale, since the MQTT connection
// itself is long-lived and IDF's client reconnects on its own.
static void watchdog_task(void *arg)
{
    (void)arg;
    while (!wifi_is_connected() || net_quiet_active()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Stagger past the post-Got-IP internal-RAM crunch. esp-mqtt's own
    // client task is created with a bare xTaskCreate() (mqtt_client.c) -
    // always INTERNAL RAM, no PSRAM option, unlike every background task
    // this project starts itself via util/psram_task.h. SNTP, POTA, psk_rx,
    // the web server and mDNS all start within the same second of Got IP, and
    // starting the MQTT task right in that window is how
    // esp_mqtt_client_start() failed outright on hardware ("E mqtt_client:
    // Error create mqtt task") - with the return value previously unchecked,
    // that silently killed self-spotting for the rest of the session.
    vTaskDelay(pdMS_TO_TICKS(8000));

    int backoff_ms = 2000;
    while (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGW(TAG, "esp_mqtt_client_start failed (internal RAM likely still tight) - retrying in %d ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 30000) backoff_ms *= 2;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!s_connected) continue;
        qmx_settings_t s;
        settings_load_all(&s);
        char call[16];
        call_upper(s.my_callsign, call, sizeof(call));
        if (call[0] && strcmp(call, s_subscribed_call) != 0) {
            do_subscribe(call);
        }
    }
}

void pskr_self_init(void)
{
    if (s_client) return;
    s_mutex = xSemaphoreCreateMutex();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = BROKER_URI,
        // The library default (6 KB, mqtt_config.h) sizes for a TLS
        // handshake on this same stack - see storage/settings.h-adjacent
        // precedent in net/psk_rx.c's own 8 KB HTTP task comment for why that
        // matters elsewhere. Ours is plain mqtt://, no TLS, so this can be
        // smaller - and every KB less asked for is one KB less that
        // esp_mqtt_client_start()'s internal xTaskCreate() (always INTERNAL
        // RAM, see watchdog_task()'s comment) needs to find contiguous.
        .task.stack_size = 4096,
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    psram_task_create(watchdog_task, "pskr_self", 4096, NULL, 3, tskNO_AFFINITY);
    ESP_LOGI(TAG, "live self-spotting ready (MQTT, %s)", BROKER_URI);
}
