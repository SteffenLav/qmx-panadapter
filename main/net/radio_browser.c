// radio-browser.info client. See radio_browser.h for why this directory.
#include "radio_browser.h"

#include "storage/settings.h"
#include "ft8/js8_message.h"      /* js8_grid_to_latlon - the only position we trust */

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"        /* vTaskDelay - the gap between retries */

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
/* ⛔ MEASURED, not estimated (2026-11-01, from the PC against the same query
 * the device sends): 174 rows at 300 km is 203 KB of JSON and 250 rows at
 * 500 km is 313 KB. 512 KB therefore has real margin, and an overflow is not
 * a short list but a parse failure - the sink truncates mid-object and the
 * operator sees "The directory's reply did not parse". PSRAM: this cannot go
 * on a task stack and must not go in internal RAM (#65). */
#define RB_BUF_BYTES (512 * 1024)

/* How many rows to ASK for, against how many we keep. The directory returns
 * duplicates and AAC rows that we drop, so asking for exactly RB_MAX_STATIONS
 * would hand back a short list after filtering.
 *
 * ⚠ 250 IS THE DIRECTORY'S OWN CEILING on this query, measured: limit=250 and
 * limit=200 both returned 250 rows at 500 km, and at 150 km both returned the
 * same 54. Asking for more is free and changes nothing. */
#define RB_FETCH_ROWS 250

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} rb_sink_t;

/* A-Z for the pane, case-insensitive. See the call site for why the SELECTION
 * stays ordered by clickcount and only the display order changes. */
static int by_name_ci(const void *a, const void *b)
{
    const rb_station_t *x = (const rb_station_t *)a;
    const rb_station_t *y = (const rb_station_t *)b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

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

/* ⭐ SUPERSEDED FOR DISPLAY, 2026-11-01 - now the DE-DUPLICATION KEY ONLY.
 *
 * This was the whole answer while the display font was ASCII: LVGL's built-in
 * montserrat carries "0x20-0x7F,0xB0,0x2022" and nothing else, so "DR P4
 * København" drew with a square and the operator saw it on the first list.
 * Folding to "Koebenhavn" hid that honestly but wrote his own language wrong,
 * and the ICY programme text was never folded at all, so it kept showing the
 * squares raw - he sent a screenshot of exactly that.
 *
 * The real fix is font_ui_latin_* (Montserrat with Latin-1), so the stored
 * name is now UTF-8 via to_latin1_utf8() below. Folding survives here because
 * it is still the right thing for the KEY: "DR P4 København" and "DR P4
 * Kobenhavn" are one station and must dedupe together.
 *
 * ⛔ THE NAME ONLY. The URL is never touched - folding a byte there would
 * point at a different stream, or at nothing.
 *
 * Anything outside this table becomes '?' rather than being dropped, so a
 * name in an alphabet we cannot show is still obviously a name and still
 * distinguishable from its neighbours. */
static void fold_ascii(const char *in, char *out, size_t cap)
{
    static const struct { uint16_t cp; const char *rep; } MAP[] = {
        { 0xE6, "ae" }, { 0xC6, "AE" },      /* æ Æ */
        { 0xF8, "oe" }, { 0xD8, "OE" },      /* ø Ø */
        { 0xE5, "aa" }, { 0xC5, "AA" },      /* å Å */
        { 0xE4, "ae" }, { 0xC4, "AE" },      /* ä Ä */
        { 0xF6, "oe" }, { 0xD6, "OE" },      /* ö Ö */
        { 0xFC, "ue" }, { 0xDC, "UE" },      /* ü Ü */
        { 0xDF, "ss" },                      /* ß */
        { 0xE9, "e"  }, { 0xC9, "E"  },
        { 0xE8, "e"  }, { 0xC8, "E"  },
        { 0xEA, "e"  }, { 0xEB, "e"  },
        { 0xE0, "a"  }, { 0xE1, "a"  }, { 0xE2, "a"  }, { 0xE3, "a"  },
        { 0xED, "i"  }, { 0xEE, "i"  }, { 0xEF, "i"  },
        { 0xF3, "o"  }, { 0xF2, "o"  }, { 0xF4, "o"  }, { 0xF5, "o"  },
        { 0xFA, "u"  }, { 0xF9, "u"  }, { 0xFB, "u"  },
        { 0xF1, "n"  }, { 0xD1, "N"  },
        { 0xE7, "c"  }, { 0xC7, "C"  },
        { 0xFD, "y"  }, { 0xFF, "y"  },
    };
    const int NMAP = (int)(sizeof(MAP) / sizeof(MAP[0]));

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; ) {
        if (*p < 0x80) { out[o++] = (char)*p++; continue; }

        /* Decode just enough UTF-8 to get the code point; anything past the
         * Basic Multilingual Plane is not something this table covers. */
        uint16_t cp = 0;
        int len = 0;
        if ((*p & 0xE0) == 0xC0)      { cp = (uint16_t)(*p & 0x1F); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = (uint16_t)(*p & 0x0F); len = 3; }
        else                          { p++; out[o++] = '?'; continue; }
        bool ok = true;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
            cp = (uint16_t)((cp << 6) | (p[i] & 0x3F));
        }
        if (!ok) { p++; out[o++] = '?'; continue; }
        p += len;

        const char *rep = NULL;
        for (int i = 0; i < NMAP && !rep; i++) if (MAP[i].cp == cp) rep = MAP[i].rep;
        if (!rep) { out[o++] = '?'; continue; }
        for (const char *r = rep; *r && o + 1 < cap; r++) out[o++] = *r;
    }
    out[o] = '\0';
}

/* What the operator actually READS. Keeps UTF-8 for everything font_ui_latin_*
 * can draw - ASCII plus Latin-1 Supplement, which covers æøå, the German
 * umlauts and ß, and the accented vowels of every language in range of this
 * radio - and replaces anything else with '?'.
 *
 * ⛔ A '?' RATHER THAN A DROP, and rather than a square. A Cyrillic or Greek
 * station is still obviously a name and still distinguishable from the one
 * above it; a dropped character silently merges two different stations in the
 * eye, and a square is what this whole change exists to remove.
 *
 * ⛔ NEVER CUTS A CHARACTER IN HALF. A 2-byte sequence is written whole or not
 * at all, so a truncated name is still valid UTF-8 - LVGL draws a stray
 * continuation byte as a square, which would reintroduce the fault at exactly
 * the length limit. */
void rb_text_to_latin1(const char *in, char *out, int capi)
{
    if (!out || capi <= 0) return;
    size_t cap = (size_t)capi;
    out[0] = '\0';
    if (!in) return;

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; ) {
        uint32_t cp;

        if (*p < 0x80) {
            if (*p < 0x20) { p++; continue; }        /* no control codes */
            out[o++] = (char)*p++;
            continue;
        }

        int len = 0;
        if      ((*p & 0xE0) == 0xC0) { cp = (uint32_t)(*p & 0x1F); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = (uint32_t)(*p & 0x0F); len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = (uint32_t)(*p & 0x07); len = 4; }
        else                          { cp = *p; len = 0; }

        if (len) {
            bool ok = true;
            for (int i = 1; i < len; i++) {
                if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
            }
            /* ⭐ NOT UTF-8 AFTER ALL -> READ IT AS LATIN-1. The ICY spec never
             * settled an encoding and plenty of European Icecast servers send
             * raw ISO-8859-1, where a lone 0xF8 IS 'ø'. Rejecting it as bad
             * UTF-8 would print '?' for exactly the stations this change is
             * meant to fix. */
            if (!ok) { cp = *p; len = 1; }
            p += len;
        } else {
            p++;                                      /* lone high byte = Latin-1 */
        }

        if (cp >= 0xA0 && cp <= 0xFF) {
            if (o + 2 >= cap) break;                 /* whole, or not at all */
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp >= 0x20 && cp < 0x7F) {
            out[o++] = (char)cp;
        } else {
            out[o++] = '?';
        }
    }
    out[o] = '\0';
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
    /* ⛔ ONE HOST AND NO RETRY MADE EVERY TRANSIENT A DEAD END.
     *
     * The first cut hard-coded de1 and reported "Directory unreachable" on any
     * failure. MEASURED on field 2026-11-01: two attempts died with
     * `mbedtls_ssl_handshake returned -0x0050` (MBEDTLS_ERR_NET_CONN_RESET)
     * while the SAME host answered HTTP 200 from the PC on the same network in
     * 128 ms, and HTTPS from the same board to GitHub worked seconds later
     * (update_check forced over the API). So it was neither the host, nor the
     * cert bundle, nor the board's TLS - it was a transient reset with nothing
     * to catch it.
     *
     * `all.api` is the project's documented round-robin and is tried first;
     * de1 is the named mirror we used to pin. ⚠ nl1 and at1 did not answer at
     * all from this network on that date, so they are NOT in the list - an
     * unreachable mirror in a retry chain only spends the operator's time. */
    static const char *const RB_HOSTS[] = {
        "all.api.radio-browser.info",
        "de1.api.radio-browser.info",
    };
    const int rb_hosts = (int)(sizeof(RB_HOSTS) / sizeof(RB_HOSTS[0]));
    const int rb_tries = 2;                 /* per host */

    char url[320];
    esp_err_t err = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; attempt < rb_hosts * rb_tries; attempt++) {
    const char *host = RB_HOSTS[attempt % rb_hosts];
    sink.len = 0; buf[0] = '\0';
    snprintf(url, sizeof(url),
             "https://%s/json/stations/search"
             "?geo_lat=%.4f&geo_long=%.4f&geo_distance=%d"
             "&hidebroken=true&order=clickcount&reverse=true&limit=%d",
             host, lat, lon, radius_km * 1000, RB_FETCH_ROWS);

    esp_http_client_config_t hc = {
        .url                   = url,
        .event_handler         = http_ev,
        .user_data             = &sink,
        .timeout_ms            = 15000,
        /* ⛔ THE BUNDLE IS NOT OPTIONAL. The first cut passed NULL here with a
         * comment arguing that a public list of radio stations is not worth
         * authenticating a server for. That reasoning never got the chance to
         * be wrong or right: esp-tls REFUSES to build a client with no
         * verification option at all -
         *   "No server verification option set in esp_tls_cfg_t structure"
         *   -> ESP_ERR_MBEDTLS_SSL_SETUP_FAILED -> ESP_ERR_HTTP_CONNECT
         * and the operator saw "Directory unreachable" on the first try
         * (bench, 2026-11-01). Skipping verification is a deliberate API call,
         * not an omission; the bundle is already in this build
         * (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE), so it costs nothing to attach
         * and it is the correct answer anyway. */
        .crt_bundle_attach     = esp_crt_bundle_attach,
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

    err = esp_http_client_perform(cl);
    status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    if (err == ESP_OK && status == 200) break;

    /* WARN every attempt, with the host named: a retry that hides its
     * failures turns a mirror that is always down into a slow success and
     * nobody ever learns which one it was. */
    ESP_LOGW(TAG, "attempt %d/%d via %s failed: %s status=%d",
             attempt + 1, rb_hosts * rb_tries, host,
             esp_err_to_name(err), status);
    if (attempt + 1 < rb_hosts * rb_tries) vTaskDelay(pdMS_TO_TICKS(400));
    }

    if (err != ESP_OK || status != 200) {
        snprintf(s_err, sizeof(s_err), "Directory unreachable (%s, HTTP %d)",
                 esp_err_to_name(err), status);
        ESP_LOGW(TAG, "fetch failed after %d attempts: %s status=%d",
                 rb_hosts * rb_tries, esp_err_to_name(err), status);
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
        char folded[RB_NAME_LEN];
        fold_ascii(jname->valuestring, folded, sizeof(folded));
        norm_name(folded, key, sizeof(key));
        if (!key[0]) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = (strcmp(seen[i], key) == 0);
        if (dup) continue;
        snprintf(seen[n], RB_NAME_LEN, "%s", key);

        rb_station_t *st = &out[n];
        memset(st, 0, sizeof(*st));
        rb_text_to_latin1(jname->valuestring, st->name, (int)sizeof(st->name));
        snprintf(st->url,  sizeof(st->url),  "%s", jurl->valuestring);

        const cJSON *jbr = cJSON_GetObjectItemCaseSensitive(it, "bitrate");
        if (cJSON_IsNumber(jbr)) st->bitrate_kbps = (uint16_t)jbr->valuedouble;
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "geo_distance");
        if (cJSON_IsNumber(jd)) st->distance_km = (uint16_t)(jd->valuedouble / 1000.0);
        n++;
    }
    cJSON_Delete(root);

    /* ⭐ SELECTION BY POPULARITY, DISPLAY BY NAME - two different orders, and
     * conflating them would be the wrong answer to "sort them A-Z".
     *
     * The query above orders by clickcount so the rows we KEEP are the ones
     * people actually listen to; sorting here then puts those rows in the
     * order the operator reads them. Sorting the directory instead would have
     * handed back the 50 alphabetically-first stations in northern Europe,
     * which is not a list anybody wants. Asked for 2026-11-01.
     *
     * Case-insensitive, and a tie falls back to the signed compare so the
     * order is total - qsort is not stable and two stations differing only in
     * case would otherwise swap places between fetches. */
    qsort(out, (size_t)n, sizeof(out[0]), by_name_ci);

    if (!n) snprintf(s_err, sizeof(s_err), "No MP3 stations within %d km", radius_km);
    ESP_LOGI(TAG, "kept %d station(s) of %d asked for, sorted A-Z", n, RB_FETCH_ROWS);
    return n;
}
