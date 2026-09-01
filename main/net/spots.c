// Live spots store + the two HTTP fetchers: POTA (api.pota.app) and SOTA
// (spothole.app). See spots.h for the contract, and net/rbn.c + net/dxcluster.c
// for the two socket-based sources that publish into the same store.
//
// Sizing, measured against the live API 2026-08-04: 94 spots in 39.6 KB, mixed
// SSB/CW/FT8. So one fetch is a ~40 KB body - trivial to parse, but far too
// big for internal RAM, hence the PSRAM response buffer (and CLAUDE.md's rule
// that anything under 16 KB silently lands in internal RAM cuts the other way
// here: this is deliberately a big allocation so it goes where we want).
//
// WiFi discipline: this board's esp_hosted link is fragile under concurrent
// load, which is why the LoTW import and the QRZ/eQSL uploads all quiet the
// spectrum WebSocket for their duration. A 40 KB fetch once a minute is far
// lighter than those, but it follows the same rule for the same reason.

#include "spots.h"
#include "net/net_quiet.h"
#include "spot_sig.h"         // spot_sig_for() - the ADIF SIG for a reference
#include "webserver_ws.h"     // webserver_ws_set_paused
#include "wifi.h"             // wifi_is_connected
#include "storage/settings.h"
#include "util/psram_task.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"     // esp_app_get_description() - for the User-Agent
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "spots";

#define POTA_URL       "https://api.pota.app/spot/activator"
// Sized for the LARGER of the two bodies. POTA measured ~40 KB; a SOTA response
// runs ~1.25 KB per record (spothole records carry flags, lat/long, zones, DXCC
// and a nested sig_refs array), so the 60-record cap below is ~75 KB. This has
// to hold it WHOLE: on_data() truncates safely at the cap, but a truncated body
// fails cJSON_Parse, which would cost every SOTA spot rather than the last few.
// PSRAM, allocated per fetch and freed straight after.
#define RESP_CAP       (160 * 1024)
#define FETCH_PERIOD_S 60              // POTA spots carry an ~expire of minutes
#define RETRY_PERIOD_S 20              // after a failure

// spothole.app - Ian Renton M0TRT's aggregator, use granted 2026-08-10. ONE
// constant on purpose: v2 endpoints exist and v1 keeps working, so moving is a
// literal v1->v2 swap here and nowhere else.
#define SPOTHOLE_BASE  "https://spothole.app/api/v1"

// What we ask spothole for, and why each parameter is there. The unfiltered
// /spots body is 862 KB (measured), so filtering server-side is not an
// optimisation, it is the difference between usable and not.
//
//   sig=SOTA       the PROGRAMME, not the feed: this also catches a SOTA
//                  activation posted to a DX cluster, which source=SOTA misses.
//   band=...       HF plus 6 m only. SOTA is largely a VHF/UHF game (the first
//                  probe came back full of 2 m FM), and the QMX cannot tune it.
//   allow_qrt=false spothole DEFAULTS THIS TO TRUE. A station that has packed up
//                  and gone down the hill is worse than no spot at all.
//   dedupe=true    latest spot per callsign; the store's own merge then keeps
//                  one entry per call+frequency.
//   max_age=3600   a summit activation runs far longer than a park one, and the
//                  lane fades on age anyway (invisible at 30 minutes).
//   limit=60       the response-buffer budget above. Newest first, so a busy
//                  weekend loses the oldest rather than the nearest.
#define SOTA_QUERY \
    "sig=SOTA" \
    "&band=160m,80m,60m,40m,30m,20m,17m,15m,12m,11m,10m,6m" \
    "&allow_qrt=false&dedupe=true&max_age=3600&limit=60"

// SOTA moves slower than POTA (a summit activation is a walk up and an hour on
// the air), so once every two minutes is plenty - and it halves what we cost a
// server we were lent.
#define SOTA_PERIOD_S     120
// On failure this backs OFF, doubling to a quarter of an hour, where POTA
// retries SOONER (20 s). That asymmetry is the whole point: spothole is a hobby
// box its owner says he occasionally breaks, so its being down has to be an
// ordinary state we ride out quietly - not something we hammer. Held spots stay
// on screen and age out on their own; nothing is shown to the operator.
#define SOTA_BACKOFF_MAX_S 900

static spot_t           *s_store;              // PSRAM, SPOTS_MAX entries
static spot_t           *s_scratch;            // PSRAM, parse target (see parse_pota)
static int               s_count;
static SemaphoreHandle_t s_lock;
static int64_t           s_last_ok_us;
static volatile bool     s_refresh_req;
static volatile uint32_t s_version;

// ---- store -----------------------------------------------------------------

static bool lock_ms(int ms) { return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE; }
static bool lock(void)   { return lock_ms(200); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

uint32_t spots_version(void) { return s_version; }

// How long a spot survives with no further mention. Matches the fade the UIs
// already apply (invisible at 30 minutes), so nothing is dropped while still
// being drawn.
#define SPOT_STALE_S 1800

void spots_publish(spot_source_t src, const spot_t *list, int n)
{
    if (!s_store || !lock()) return;

    // MERGE, not replace (operator, 2026-08-09: "sometimes all the spots
    // delete/shift to a new picture - I would think they change at a much more
    // steady and slow pace").
    //
    // This used to compact the whole source out and append whatever the fetch
    // returned, so every 60 s the entire POTA set was swapped for the API's
    // current view. A station the API happened to omit from one response
    // vanished and came back a minute later, and the ORDER changed wholesale -
    // which, since labels are placed first-come, re-shuffled which spots got a
    // label at all. The display churned far more than the band did.
    //
    // Now a fetch REFRESHES what it mentions and leaves the rest to age out on
    // their own. The spots carry heard_unix and both UIs already fade on it, so
    // ageing is the honest expiry - absence from one response is not evidence a
    // station has gone.
    int64_t now = (int64_t)time(NULL);

    // 1. Drop this source's genuinely stale entries (and keep every other source).
    int keep = 0;
    for (int i = 0; i < s_count; i++) {
        bool mine  = (s_store[i].source == src);
        bool stale = mine && s_store[i].heard_unix > 0 &&
                     (now - s_store[i].heard_unix) > SPOT_STALE_S;
        if (!stale) s_store[keep++] = s_store[i];
    }
    s_count = keep;

    // 2. Fold the fetch in: update a spot already known on the same frequency,
    //    otherwise append. Matching on call AND frequency so the same operator
    //    active on two bands stays two spots.
    for (int i = 0; i < n && list; i++) {
        int at = -1;
        for (int j = 0; j < s_count; j++) {
            if (s_store[j].source != src) continue;
            if (s_store[j].freq_hz != list[i].freq_hz) continue;
            if (strncmp(s_store[j].call, list[i].call, sizeof(s_store[j].call)) != 0) continue;
            at = j; break;
        }
        if (at >= 0) {
            s_store[at] = list[i];                 // same station, fresher details
        } else if (s_count < SPOTS_MAX) {
            s_store[s_count++] = list[i];
        }
    }

    s_version++;
    unlock();
}

int spots_get(spot_t *out, int max)
{
    if (!out || max <= 0 || !lock()) return 0;
    int n = s_count < max ? s_count : max;
    memcpy(out, s_store, (size_t)n * sizeof(spot_t));
    unlock();
    return n;
}

// How far apart two spots for the same callsign may sit and still be the same
// station. The RBN reports the CW carrier a skimmer measured; a POTA/SOTA spot
// carries whatever the activator (or a chaser) typed in. They routinely differ
// by a few hundred Hz, and rounding to the nearest kHz is common by hand.
#define SPOT_DUP_TOL_HZ 2000

static inline bool spot_same_station(const spot_t *a, const spot_t *b)
{
    uint32_t d = (a->freq_hz > b->freq_hz) ? a->freq_hz - b->freq_hz
                                           : b->freq_hz - a->freq_hz;
    return d <= SPOT_DUP_TOL_HZ && strcasecmp(a->call, b->call) == 0;
}

static int get_in_range_locked(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz)
{
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++)
        if (s_store[i].freq_hz >= lo_hz && s_store[i].freq_hz <= hi_hz)
            out[n++] = s_store[i];

    // Collapse repeats of the same station into one entry. Two kinds occur, and
    // both were measured on the live feed (20 m, 2026-08-09, 79 spots in
    // window): an activator spotted on POTA AND heard by the RBN (2 of only 4
    // activation spots were doubled - which is why it stood out so badly), and
    // the RBN doubling ITSELF (5 pairs, every one exactly 100 Hz apart, i.e.
    // two skimmers rounding the same signal differently). Both put two labels
    // at almost the same x and make a busy band's lane unreadable.
    //
    // The earlier entry wins, because the store is newest-first - EXCEPT that
    // an activation spot always beats an RBN one, since it carries the park or
    // summit reference, which is the whole reason the operator is looking.
    //
    // Runs over the IN-RANGE subset (a band slice), not the whole 200-entry
    // store. The frequency test is an integer compare and rejects almost every
    // pair before the string compare, so the O(k^2) scan stays cheap enough for
    // the LVGL thread holding the store lock.
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {
            if (!spot_same_station(&out[i], &out[j])) continue;
            // Keep the activation spot in the surviving slot, whichever way
            // round the two happen to be.
            if (out[i].source != SPOT_SRC_RBN && out[j].source == SPOT_SRC_RBN) {
                spot_t tmp = out[j]; out[j] = out[i]; out[i] = tmp;
            }
            if (out[i].source == SPOT_SRC_RBN && out[j].source != SPOT_SRC_RBN)
                out[j].rbn_confirmed = true;
            if (out[i].rbn_confirmed) out[j].rbn_confirmed = true;
            // Take the later timestamp. A skimmer copying the station five
            // minutes ago is direct evidence it was on the air five minutes
            // ago, which beats the hour-old self-spot the activation feed is
            // still serving - and it makes the lane's age fade correct with no
            // change needed there.
            if (out[i].heard_unix > out[j].heard_unix)
                out[j].heard_unix = out[i].heard_unix;
            memmove(&out[i], &out[i + 1], (size_t)(n - i - 1) * sizeof(spot_t));
            n--;
            i--;                        // re-test the entry shifted into this slot
            break;
        }
    }
    return n;
}

int spots_get_in_range(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz)
{
    if (!out || max <= 0 || !lock()) return 0;
    int n = get_in_range_locked(out, max, lo_hz, hi_hz);
    unlock();
    return n;
}

int spots_get_in_range_wait(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz, int wait_ms)
{
    if (!out || max <= 0) return 0;
    if (!lock_ms(wait_ms)) return -1;      // caller keeps whatever it already drew
    int n = get_in_range_locked(out, max, lo_hz, hi_hz);
    unlock();
    return n;
}

bool spots_activation_for_call(const char *call, uint32_t freq_hz,
                               char *sig_out, size_t sig_sz,
                               char *ref_out, size_t ref_sz)
{
    if (sig_out && sig_sz) sig_out[0] = '\0';
    if (ref_out && ref_sz) ref_out[0] = '\0';
    if (!call || !call[0] || !ref_out || ref_sz == 0) return false;
    // Short, bounded wait: this runs on the QSO-completion path, which must not
    // stall behind a fetch swapping the table in. Losing the reference on a
    // contended moment is a missed chase credit; blocking the QSO machine is
    // worse.
    if (!lock_ms(50)) return false;

    bool found = false;
    for (int i = 0; i < s_count; i++) {
        if (s_store[i].source == SPOT_SRC_RBN) continue;   // never carries a reference
        if (!s_store[i].ref[0]) continue;
        // EXACT match, deliberately - do NOT add a base-callsign fallback.
        //
        // Every SOTA spot carries a portable suffix (16 of 16 in a live sample:
        // EA2GM/P, G4IPB/P, LA/SP9WLG/P...), so if a station is spotted as
        // EA2GM/P and worked as EA2GM, no reference is logged and the chase
        // credit is lost. Matching on the base call would recover those - and
        // was rejected on purpose (operator's call, 2026-08-10).
        //
        // The reason is which way the error falls. EA2GM at home and EA2GM/P on
        // a summit are the same operator in different places, so a loose match
        // can write a summit into a QSO that never was one: an unearned claim in
        // both logs, uploaded to QRZ/eQSL/LoTW as fact. The strict version only
        // ever omits something. Same principle as never inventing an RST (see
        // CLAUDE.md) - a missing field is honest, a wrong one is not.
        if (strcasecmp(s_store[i].call, call) != 0) continue;
        if (freq_hz) {
            uint32_t d = (s_store[i].freq_hz > freq_hz) ? s_store[i].freq_hz - freq_hz
                                                        : freq_hz - s_store[i].freq_hz;
            if (d > SPOT_DUP_TOL_HZ) continue;
        }
        snprintf(ref_out, ref_sz, "%s", s_store[i].ref);
        // The SIG has to agree with the reference, because that is what a chase
        // is matched on at the other end: a summit filed as SIG=POTA earns
        // nobody anything. Decided in spot_sig.c, which is dependency-free so
        // the rule is covered by test/spot_sig_harness.c.
        if (sig_out && sig_sz)
            snprintf(sig_out, sig_sz, "%s",
                     spot_sig_for(s_store[i].source, s_store[i].ref));
        found = true;
        break;
    }
    unlock();
    return found;
}

int spots_age_s(void)
{
    if (!s_last_ok_us) return -1;
    return (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
}

void spots_request_refresh(void) { s_refresh_req = true; }

// Is ANY spot source switched on? The lane is drawn on this, not on spots_en.
//
// spots_en used to gate both the POTA fetch AND the whole display, which made
// it a hidden master switch: the drawer shows three checkboxes that look like
// three equal sources, so turning off "Live spots (POTA)" and leaving RBN or
// the DX cluster on produced an empty lane with no explanation. Reported by
// the operator, who had exactly that combination. Each checkbox is now purely
// its own source.
bool spots_any_source_enabled(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    return s.spots_en || s.rbn_en || s.cluster_en || s.sota_en;
}

// ---- fetch -----------------------------------------------------------------

typedef struct { char *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_buf_t *r = (resp_buf_t *)evt->user_data;
    if (!r || r->len + 1 >= r->cap) return ESP_OK;    // full: keep what we have
    size_t avail = r->cap - r->len - 1;
    size_t n = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
    memcpy(r->buf + r->len, evt->data, n);
    r->len += n;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static spot_mode_t mode_from_str(const char *m)
{
    if (!m || !m[0]) return SPOT_MODE_OTHER;
    if (strcasecmp(m, "CW") == 0)  return SPOT_MODE_CW;
    if (strcasecmp(m, "SSB") == 0 || strcasecmp(m, "USB") == 0 ||
        strcasecmp(m, "LSB") == 0 || strcasecmp(m, "PHONE") == 0) return SPOT_MODE_SSB;
    if (strncasecmp(m, "FT", 2) == 0 || strcasecmp(m, "PSK") == 0 ||
        strcasecmp(m, "RTTY") == 0 || strcasecmp(m, "DATA") == 0) return SPOT_MODE_DIGI;
    return SPOT_MODE_OTHER;
}

// "2026-08-04T16:02:24" (UTC, no zone suffix) -> unix seconds. Returns 0 when
// unparseable, which the UI treats as "age unknown" rather than "ancient".
static int64_t parse_spot_time(const char *s)
{
    if (!s) return 0;
    struct tm tmv = {0};
    int y, mo, d, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
    // ESP-IDF newlib runs UTC, so mktime() == timegm() here (same assumption
    // the RTC code documents and relies on).
    return (int64_t)mktime(&tmv);
}

// Replace the store with what the payload holds. Spots without a usable
// frequency are dropped rather than clamped - a spot you cannot tune to is
// worse than no spot.
//
// Parses into s_scratch and swaps the finished table in under ONE lock. The
// first version wrote each entry straight into s_store and only set s_count at
// the end, so for the whole duration of a parse the live table held a mix of
// new and old entries while readers still saw the previous count - the lane
// could draw a call at another station's frequency. Fixed-size entries under a
// mutex meant it could never crash, which is exactly why it would have been an
// annoying one to find later.
static int parse_pota(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "POTA: unparseable JSON"); return -1; }
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); ESP_LOGW(TAG, "POTA: not an array"); return -1; }

    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, root) {
        if (n >= SPOTS_MAX) break;
        const cJSON *jf = cJSON_GetObjectItem(it, "frequency");
        double khz = 0;
        if (cJSON_IsNumber(jf))       khz = jf->valuedouble;
        else if (cJSON_IsString(jf))  khz = atof(jf->valuestring);
        if (khz < 1000.0 || khz > 60000.0) continue;      // not HF/6m: skip

        spot_t sp = {0};
        sp.freq_hz = (uint32_t)(khz * 1000.0);
        sp.source  = SPOT_SRC_POTA;
        const cJSON *jc = cJSON_GetObjectItem(it, "activator");
        const cJSON *jr = cJSON_GetObjectItem(it, "reference");
        const cJSON *jm = cJSON_GetObjectItem(it, "mode");
        const cJSON *jt = cJSON_GetObjectItem(it, "spotTime");
        if (cJSON_IsString(jc)) snprintf(sp.call, sizeof(sp.call), "%s", jc->valuestring);
        if (cJSON_IsString(jr)) snprintf(sp.ref,  sizeof(sp.ref),  "%s", jr->valuestring);
        sp.mode = mode_from_str(cJSON_IsString(jm) ? jm->valuestring : NULL);
        sp.heard_unix = cJSON_IsString(jt) ? parse_spot_time(jt->valuestring) : 0;
        if (!sp.call[0]) continue;

        s_scratch[n++] = sp;
    }
    cJSON_Delete(root);

    spots_publish(SPOT_SRC_POTA, s_scratch, n);   // replaces only the POTA slice
    return n;
}

// spothole's record shape, from the live feed rather than from the docs. Only
// the six fields below are read; the rest of a ~1.25 KB record is ignored.
//
//   dx_call    "HB0/HB9BXQ/P"       the activator
//   freq       7031000.0            HERTZ, as a float. POTA sends kHz - getting
//                                  this the POTA way put spots 1000x off band.
//   mode       "CW"/"SSB"/"FM"      absent on some spots, hence mode_type
//   mode_type  "CW"/"PHONE"/"DATA"  the coarse class, used as the fallback
//   sig_refs[] [{"id":"G/LD-049"}]  an ARRAY: a spot can carry more than one
//                                  reference (a summit inside a park). The
//                                  first is the one the spot is about.
//   time       1786362640.3         unix seconds, float. There is a time_iso
//                                  too; the number needs no parsing.
static int parse_sota(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "SOTA: unparseable JSON"); return -1; }
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); ESP_LOGW(TAG, "SOTA: not an array"); return -1; }

    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, root) {
        if (n >= SPOTS_MAX) break;

        const cJSON *jf = cJSON_GetObjectItem(it, "freq");
        double hz = 0;
        if (cJSON_IsNumber(jf))      hz = jf->valuedouble;
        else if (cJSON_IsString(jf)) hz = atof(jf->valuestring);
        // HF and 6 m only. The query already asks for those bands, but a feed is
        // not a contract - and a spot we cannot tune to is worse than no spot.
        if (hz < 1000000.0 || hz > 60000000.0) continue;

        spot_t sp = {0};
        sp.freq_hz = (uint32_t)hz;
        sp.source  = SPOT_SRC_SOTA;

        const cJSON *jc = cJSON_GetObjectItem(it, "dx_call");
        if (cJSON_IsString(jc)) snprintf(sp.call, sizeof(sp.call), "%s", jc->valuestring);
        if (!sp.call[0]) continue;

        // Skip a QRT spot even though allow_qrt=false already should have. Same
        // reasoning as the frequency clamp: cheap, and it is the operator's time
        // being wasted if the server's default ever changes back.
        const cJSON *jq = cJSON_GetObjectItem(it, "qrt");
        if (cJSON_IsBool(jq) && cJSON_IsTrue(jq)) continue;

        const cJSON *jm  = cJSON_GetObjectItem(it, "mode");
        const cJSON *jmt = cJSON_GetObjectItem(it, "mode_type");
        sp.mode = mode_from_str(cJSON_IsString(jm) ? jm->valuestring : NULL);
        if (sp.mode == SPOT_MODE_OTHER && cJSON_IsString(jmt))
            sp.mode = mode_from_str(jmt->valuestring);

        // A SOTA spot with NO reference at all is normal, not a broken record:
        // 2 of 16 in a live sample (LX/ON4UP/P, ON/PA9HR/P) were sig=SOTA with
        // an empty sig_refs. Keep them - the operator can still work the
        // station, and it is only the chase credit that is unavailable. Leaving
        // ref empty is what stops spots_activation_for_call() inventing one.
        const cJSON *jrefs = cJSON_GetObjectItem(it, "sig_refs");
        if (cJSON_IsArray(jrefs)) {
            const cJSON *r0 = cJSON_GetArrayItem(jrefs, 0);
            const cJSON *jid = r0 ? cJSON_GetObjectItem(r0, "id") : NULL;
            if (cJSON_IsString(jid)) snprintf(sp.ref, sizeof(sp.ref), "%s", jid->valuestring);
        }

        const cJSON *jt = cJSON_GetObjectItem(it, "time");
        if (cJSON_IsNumber(jt)) sp.heard_unix = (int64_t)jt->valuedouble;

        s_scratch[n++] = sp;
    }
    cJSON_Delete(root);

    spots_publish(SPOT_SRC_SOTA, s_scratch, n);   // replaces only the SOTA slice
    return n;
}

// GET url into a PSRAM buffer. Returns the body length, or -1 on any failure
// (the caller must not parse). Shared by both sources so the WebSocket courtesy
// and the buffer discipline cannot drift apart between them.
static int http_get_json(const char *url, char **buf_out)
{
    *buf_out = NULL;
    char *buf = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "no PSRAM for the response buffer"); return -1; }
    buf[0] = '\0';
    resp_buf_t ctx = { buf, 0, RESP_CAP };

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .event_handler     = on_data,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(buf); return -1; }
    esp_http_client_set_header(client, "Accept", "application/json");
    // Identify ourselves. Ian asked for this specifically ("you're already doing
    // better than some clients I see in the log"), and a server operator who can
    // see who is calling can tell us apart from a runaway client.
    const esp_app_desc_t *app = esp_app_get_description();
    char ua[80];
    snprintf(ua, sizeof(ua), "QMX-Panadapter/%s (+https://tab5.lav.dk)",
             app ? app->version : "dev");
    esp_http_client_set_header(client, "User-Agent", ua);

    // Same courtesy the uploads pay: keep the spectrum stream off the link
    // while the transfer runs (see the LoTW note in CLAUDE.md).
    webserver_ws_set_paused(true);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    webserver_ws_set_paused(false);

    if (status != 200 || ctx.len == 0) {
        ESP_LOGW(TAG, "GET failed (status=%d err=0x%x) %s", status, err, url);
        heap_caps_free(buf);
        return -1;
    }
    *buf_out = buf;
    return (int)ctx.len;
}

static void fetch_pota(void)
{
    char *buf = NULL;
    int len = http_get_json(POTA_URL, &buf);
    if (len < 0) return;                               // logged by http_get_json()

    int n = parse_pota(buf);
    if (n >= 0) {
        s_last_ok_us = esp_timer_get_time();
        ESP_LOGI(TAG, "POTA: %d spots (%u bytes)", n, (unsigned)len);
    }
    heap_caps_free(buf);
}

// Returns true when the fetch succeeded, so the caller can reset its backoff.
static bool fetch_sota(void)
{
    char *buf = NULL;
    int len = http_get_json(SPOTHOLE_BASE "/spots?" SOTA_QUERY, &buf);
    if (len < 0) return false;

    int n = parse_sota(buf);
    heap_caps_free(buf);
    if (n < 0) return false;

    s_last_ok_us = esp_timer_get_time();
    ESP_LOGI(TAG, "SOTA: %d spots (%u bytes)", n, (unsigned)len);
    return true;
}

// One task, two sources, each with its own cadence: POTA every minute, SOTA
// every two with a quiet backoff. Both are due-time driven rather than "sleep
// the shorter period and count", so adding a third source is a due time and not
// a rewrite - and a slow SOTA backoff can never delay a POTA fetch.
static void spots_task(void *arg)
{
    (void)arg;
    int64_t next_pota_us = 0;                        // 0 = due now
    int64_t next_sota_us = 0;
    int     sota_backoff_s = 0;                      // 0 = last attempt was fine
    // Start as if both were on, so a boot with a source switched off clears its
    // (empty) slice exactly once and then leaves the store alone.
    bool    was_pota_en = true, was_sota_en = true;

    for (;;) {
        // 500 ms granularity so a refresh request (band change, source just
        // switched on) is acted on promptly.
        vTaskDelay(pdMS_TO_TICKS(500));
        bool forced = s_refresh_req;
        if (forced) {
            s_refresh_req = false;
            next_pota_us = next_sota_us = 0;
            sota_backoff_s = 0;                      // an explicit ask resets the backoff
        }

        qmx_settings_t s;
        settings_load_all(&s);
        int64_t now = esp_timer_get_time();

        // Clear a source's slice when it is switched off. Merely stopping the
        // fetch leaves its spots in the store to age out over SPOT_STALE_S (30
        // minutes), so unticking POTA kept showing POTA spots - which looks
        // exactly like the checkbox not working. RBN and the DX cluster already
        // do this on their own disable paths.
        //
        // ON THE EDGE ONLY. spots_publish() bumps the store version, which is
        // what the lane repaints on, and this loop now ticks at 2 Hz - clearing
        // unconditionally rebuilt every label twice a second for as long as a
        // source was switched off.
        if (was_pota_en && !s.spots_en) spots_publish(SPOT_SRC_POTA, NULL, 0);
        if (was_sota_en && !s.sota_en)  spots_publish(SPOT_SRC_SOTA, NULL, 0);
        was_pota_en = s.spots_en;
        was_sota_en = s.sota_en;

        if (!wifi_is_connected()) continue;

        // net_quiet: an OTA verify needs internal heap, and a POTA fetch is a
        // whole TLS session built and torn down. Skipping it costs one polling
        // interval; colliding with the verify has cost a watchdog reset.
        if (s.spots_en && now >= next_pota_us && !net_quiet_active()) {
            fetch_pota();
            bool ok = (spots_age_s() == 0);
            next_pota_us = esp_timer_get_time() +
                           (int64_t)(ok ? FETCH_PERIOD_S : RETRY_PERIOD_S) * 1000000;
        }

        // ⚠ MISSED ON THE FIRST PASS. Only the POTA fetch was gated, so a
        // 23 KB SOTA response was still being pulled over TLS in the middle of
        // an update - measured mid-download as "spots: SOTA: 18 spots (23600
        // bytes)" while internal free was 5 KB. Closing three of four doors is
        // not closing the door.
        if (s.sota_en && now >= next_sota_us && !net_quiet_active()) {
            bool ok = fetch_sota();
            if (ok) {
                sota_backoff_s = 0;
            } else {
                // Double, from one period up to the cap. Quietly - a failed
                // fetch is logged and nothing else: no banner, no toast, and the
                // spots already held stay on screen until they age out.
                sota_backoff_s = sota_backoff_s ? sota_backoff_s * 2 : SOTA_PERIOD_S;
                if (sota_backoff_s > SOTA_BACKOFF_MAX_S) sota_backoff_s = SOTA_BACKOFF_MAX_S;
                ESP_LOGW(TAG, "SOTA: spothole unreachable - next try in %d s", sota_backoff_s);
            }
            next_sota_us = esp_timer_get_time() +
                           (int64_t)(ok ? SOTA_PERIOD_S : sota_backoff_s) * 1000000;
        }
    }
}

void spots_init(void)
{
    if (s_store) return;
    s_store   = heap_caps_calloc(SPOTS_MAX, sizeof(spot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch = heap_caps_calloc(SPOTS_MAX, sizeof(spot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock    = xSemaphoreCreateMutex();
    if (!s_store || !s_scratch || !s_lock) { ESP_LOGE(TAG, "init failed"); return; }
    psram_task_create(spots_task, "spots", 6144, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "spot fetcher started (POTA, SOTA)");
}
