/* wsprnet.org reception-report upload. See wsprnet.h for the rules this obeys
 * and why they are not negotiable.
 *
 * ---- THE WIRE FORMAT, AND HOW IT WAS ESTABLISHED ------------------------
 *
 * wsprnet's own upload documentation is not publicly readable (the pages 403,
 * and the API owner's note says to contact the custodian), so the PARAMETER
 * LIST comes from what WSJT-X and its forks send to http://wsprnet.org/post:
 *
 *   function=wspr &rcall &rgrid &rqrg &date &time &sig &dt &drift
 *                 &tqrg &tcall &tgrid &dbm &version &mode
 *
 * The VALUE formats are wsprd's own printed output, because that is literally
 * where WSJT-X takes them from - it parses its decoder's text and forwards the
 * fields. This repository carries real wsprd output on disk
 * (test/wav_reference/wspr/ *.txt), so every field below was checked against it
 * rather than assumed:
 *
 *     1910 -13 -0.4  14.097011  0  G8MCD IO91 23
 *     time  snr  dt   MHz     drift call  grid dbm
 *
 * ⚠ A summarised web fetch described `sig` as dBm. It is the SNR in dB - the
 * two are different fields and `dbm` is the separate transmit power. Checking
 * the line above is what caught it; this project has been bitten by trusting a
 * summarised fetch before (the ARRL FD bit layout had to be re-verified
 * against packjt77.f90 line by line).
 *
 * ⛔ AND THE ONE THAT WOULD HAVE POISONED EVERY SPOT WE EVER PUBLISHED:
 * wsprd reports the signal's CENTRE frequency, while wspr_decode_result_t
 * (and therefore wspr_spot_t.freq_hz) holds the TONE-0 frequency - 1.5 tone
 * spacings lower, 2.197265625 Hz. wspr_subtract.h documents the convention
 * difference because mixing the two up has cost this project before. Sending
 * our number straight out would have put EVERY spot 2.2 Hz low in a public
 * dataset, consistently, invisibly, and in a way nobody downstream could tell
 * from a real measurement.
 */
#include "wsprnet.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wspr_spots.h"
#include "wspr_rx.h"
#include "storage/settings.h"
#include "util/psram_task.h"
#include "net/net_quiet.h"

static const char *TAG = "wsprnet";

/* WSPR tone spacing is 12000/8192 Hz; wsprd reports the centre, which is 1.5
 * spacings above tone 0. See the header note - this is the single most
 * dangerous constant in the file. */
#define WSPR_CENTRE_OFFSET_HZ  (1.5 * 12000.0 / 8192.0)   /* 2.197265625 */

#define POST_URL   "http://wsprnet.org/post"
#define BATCH_MAX  16      /* spots examined per pass; wsprnet takes one each */
#define PERIOD_MS  60000   /* once a minute: a cycle is two, so never behind */

static char s_status[80] = "off";

const char *wsprnet_status(void) { return s_status; }

/* Build the query for one spot. Returns the length written, or 0 if the spot
 * cannot honestly be described (no reporter identity, or an unmeasured field
 * that would have to be invented to fill the slot). */
static int build_query(char *out, size_t n, const wspr_spot_t *sp,
                       const qmx_settings_t *cfg)
{
    if (!cfg->my_callsign[0] || !cfg->my_grid[0]) return 0;

    /* ⛔ NEVER SUBSTITUTE FOR AN UNMEASURED VALUE. snr_db and drift_hz carry
     * explicit sentinels precisely so an unmeasured reading cannot quietly
     * become a number - the same rule that deleted the ADIF "599". A spot we
     * cannot fully describe is simply not published; a missing report is
     * honest where a fabricated one is not. */
    if (sp->snr_db == WSPR_SNR_UNKNOWN || sp->drift_hz == WSPR_DRIFT_UNKNOWN)
        return 0;

    struct tm t;
    const time_t when = (time_t)sp->cycle_utc;
    gmtime_r(&when, &t);

    /* Dial in MHz, and the TRANSMITTER's frequency as wsprd would report it:
     * dial + audio offset, where the audio offset is the CENTRE, not tone 0. */
    const double dial_mhz = (double)cfg->wspr_dial_hz / 1e6;
    const double tqrg_mhz = dial_mhz +
        ((double)sp->freq_hz + WSPR_CENTRE_OFFSET_HZ) / 1e6;

    const esp_app_desc_t *app = esp_app_get_description();

    /* dt is not measured per spot here, and it is OPTIONAL to the receiver's
     * usefulness - it describes the transmitter's timing, not the path. Sent
     * as 0.0 rather than omitted because the field is positional in every
     * implementation seen; it is the one value here that is a placeholder, and
     * it is a placeholder for something we do not claim to have measured. */
    return snprintf(out, n,
        "function=wspr&rcall=%s&rgrid=%s&rqrg=%.6f"
        "&date=%02d%02d%02d&time=%02d%02d"
        "&sig=%d&dt=0.0&drift=%d"
        "&tqrg=%.6f&tcall=%s&tgrid=%s&dbm=%d"
        "&version=%s&mode=2",
        cfg->my_callsign, cfg->my_grid, dial_mhz,
        (t.tm_year + 1900) % 100, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min,
        (int)sp->snr_db, (int)sp->drift_hz,
        tqrg_mhz, sp->call, sp->grid, (int)sp->power_dbm,
        app && app->version[0] ? app->version : "qmx");
}

static bool post_one(const char *query)
{
    /* Sized so the compiler can see the query can never be truncated into it:
     * the query buffer is 512, and a silently clipped URL would publish a
     * MALFORMED spot rather than fail, which is the worse outcome. */
    char url[512 + sizeof(POST_URL) + 2];
    snprintf(url, sizeof(url), "%s?%s", POST_URL, query);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_perform(c) == ESP_OK) {
        const int code = esp_http_client_get_status_code(c);
        ok = (code >= 200 && code < 300);
        if (!ok) ESP_LOGW(TAG, "wsprnet returned HTTP %d", code);
    } else {
        ESP_LOGW(TAG, "wsprnet POST failed (no response)");
    }
    esp_http_client_cleanup(c);
    return ok;
}

bool wsprnet_dry_run(void)
{
    qmx_settings_t cfg;
    settings_load_all(&cfg);

    wspr_spot_t sp[BATCH_MAX];
    const int n = wspr_spots_pending_upload(sp, BATCH_MAX);
    if (n <= 0) { ESP_LOGW(TAG, "dry run: nothing publishable yet"); return false; }

    char q[512];
    /* The OLDEST eligible one - the next that would actually go. */
    if (build_query(q, sizeof(q), &sp[n - 1], &cfg) <= 0) {
        ESP_LOGW(TAG, "dry run: the oldest publishable spot cannot be described "
                      "(callsign/grid unset, or an unmeasured field)");
        return false;
    }
    ESP_LOGW(TAG, "dry run, NOT SENT:");
    ESP_LOGW(TAG, "  %s?%s", POST_URL, q);
    return true;
}

static void wsprnet_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));

        qmx_settings_t cfg;
        settings_load_all(&cfg);

        if (!cfg.wspr_net_en) { snprintf(s_status, sizeof(s_status), "off"); continue; }
        if (!cfg.my_callsign[0] || !cfg.my_grid[0]) {
            snprintf(s_status, sizeof(s_status), "needs callsign and grid");
            continue;
        }
        /* Same courtesy every other feed on this board observes: stand down
         * while something that needs the link more is using it (an OTA). */
        if (net_quiet_active()) continue;

        wspr_spot_t sp[BATCH_MAX];
        const int n = wspr_spots_pending_upload(sp, BATCH_MAX);
        if (n <= 0) {
            snprintf(s_status, sizeof(s_status), "on - nothing new to publish");
            continue;
        }

        int sent = 0;
        /* Oldest first, so a truncated batch leaves the NEWEST for next time
         * rather than stranding the oldest forever. */
        for (int i = n - 1; i >= 0; i--) {
            char q[512];
            if (build_query(q, sizeof(q), &sp[i], &cfg) <= 0) continue;
            if (!post_one(q)) break;          /* stop on the first failure */
            wspr_spots_mark_sent(sp[i].cycle_utc, sp[i].call);
            sent++;
            vTaskDelay(pdMS_TO_TICKS(500));   /* be a polite client */
        }
        if (sent) ESP_LOGW(TAG, "published %d spot(s)", sent);
        snprintf(s_status, sizeof(s_status), "on - %d published, %d waiting",
                 sent, n - sent);
    }
}

void wsprnet_init(void)
{
    /* PSRAM stack: background, not latency-critical - the standing rule in
     * util/psram_task.h, and internal RAM is what the OTA verify runs out of. */
    psram_task_create(wsprnet_task, "wsprnet", 5120, NULL, 2, 0);
}
