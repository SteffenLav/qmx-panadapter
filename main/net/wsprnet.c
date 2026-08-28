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
#include "freertos/semphr.h"
#include "esp_attr.h"

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

/* ⛔ THE WORK AREA IS STATIC, NOT ON THE STACK, AND THAT IS NOT AN
 * OPTIMISATION - IT IS WHY THIS TASK STOPPED CRASHING.
 *
 * First version put all of it on a 5 KB task stack: qmx_settings_t is 844
 * bytes, wspr_spot_t[16] is 896, the query is 512 and the URL another 530 -
 * and then esp_http_client_perform() runs on top of what is left. It took a
 * Stack protection fault in the wsprnet task at 905 s, on the first real
 * upload pass. CLAUDE.md states the rule plainly and I did not apply it:
 * "before adding a local bigger than a couple of hundred bytes, identify which
 * task the code runs on".
 *
 * ⚠ The dry run had the SAME locals on the HTTPD task, which is a second
 * fault that had not fired yet - it is called straight from a request handler.
 * One shared work area fixes both.
 *
 * The mutex is required, not decorative: the uploader task and an /api/cmd dry
 * run genuinely can run at once, and they would otherwise be writing the same
 * buffers. */
typedef struct {
    qmx_settings_t cfg;
    wspr_spot_t    sp[BATCH_MAX];
    char           q[512];
    char           url[512 + sizeof(POST_URL) + 2];
} work_t;

static EXT_RAM_BSS_ATTR work_t   s_work;
static SemaphoreHandle_t         s_work_mtx;

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
    /* The caller holds s_work_mtx; this borrows its URL buffer rather than
     * putting another half-kilobyte on the stack. */
    char *url = s_work.url;
    snprintf(url, sizeof(s_work.url), "%s?%s", POST_URL, query);

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
    /* Called from an httpd request handler, so it must not put the work area
     * on that task's stack either - see the note beside s_work. */
    if (!s_work_mtx || xSemaphoreTake(s_work_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "dry run: uploader busy, try again");
        return false;
    }
    settings_load_all(&s_work.cfg);

    const int n = wspr_spots_pending_upload(s_work.sp, BATCH_MAX);
    bool ok = false;
    if (n <= 0) {
        ESP_LOGW(TAG, "dry run: nothing publishable yet");
    } else if (build_query(s_work.q, sizeof(s_work.q),
                           &s_work.sp[n - 1], &s_work.cfg) <= 0) {
        /* The OLDEST eligible one - the next that would actually go. */
        ESP_LOGW(TAG, "dry run: the oldest publishable spot cannot be described "
                      "(callsign/grid unset, or an unmeasured field)");
    } else {
        ESP_LOGW(TAG, "dry run, NOT SENT:");
        ESP_LOGW(TAG, "  %s?%s", POST_URL, s_work.q);
        ok = true;
    }
    xSemaphoreGive(s_work_mtx);
    return ok;
}

static void wsprnet_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));

        if (!s_work_mtx ||
            xSemaphoreTake(s_work_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) continue;

        /* Everything below works out of s_work. NOTHING large goes on this
         * stack - see the note beside s_work for what happened when it did. */
        settings_load_all(&s_work.cfg);

        int sent = 0, n = 0;
        if (!s_work.cfg.wspr_net_en) {
            snprintf(s_status, sizeof(s_status), "off");
        } else if (!s_work.cfg.my_callsign[0] || !s_work.cfg.my_grid[0]) {
            snprintf(s_status, sizeof(s_status), "no call/grid");
        } else if (net_quiet_active()) {
            /* Same courtesy every other feed on this board observes: stand
             * down while something that needs the link more is using it. */
        } else {
            n = wspr_spots_pending_upload(s_work.sp, BATCH_MAX);
            if (n <= 0) {
                snprintf(s_status, sizeof(s_status), "idle");
            } else {
                /* Oldest first, so a truncated batch leaves the NEWEST for
                 * next time rather than stranding the oldest forever. */
                for (int i = n - 1; i >= 0; i--) {
                    if (build_query(s_work.q, sizeof(s_work.q),
                                    &s_work.sp[i], &s_work.cfg) <= 0) continue;
                    if (!post_one(s_work.q)) break;   /* stop on first failure */
                    wspr_spots_mark_sent(s_work.sp[i].cycle_utc, s_work.sp[i].call);
                    sent++;
                    /* ⚠ 1500 ms, not 500. Roughly half the posts were coming
                     * back "no response" at 500 ms - the cause is NOT
                     * established (wsprnet is a busy volunteer service, each
                     * post opens a fresh connection and so a fresh DNS lookup,
                     * and this board's WiFi link is not robust), so this is a
                     * courtesy rather than a diagnosis. It costs nothing: a
                     * cycle is 120 s and a busy one yields under ten spots. */
                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
                if (sent) ESP_LOGW(TAG, "published %d spot(s)", sent);
                /* Says what is still owed, because a pass that stops on a
                 * failure leaves the rest for next time - and "waiting" is the
                 * honest word for a spot that is neither lost nor sent.
                 * Nothing is marked sent unless the server accepted it, so a
                 * failed post costs a minute, never a spot. */
                /* ⚠ SHORT ENOUGH FOR THE PANEL, which is 340 px at 22 pt - about
                 * 28 characters. "on - 8 sent, 3 waiting" wrapped to a third
                 * line and collided with the BAND HOP heading below it. The
                 * status string is consumed by a fixed-width label, so its
                 * LENGTH is part of its contract, not a cosmetic detail. */
                snprintf(s_status, sizeof(s_status),
                         n - sent > 0 ? "%d sent %d wait" : "%d sent",
                         sent, n - sent);
            }
        }

        /* ⛔ ONE RELEASE, ON EVERY PATH. The first version returned early from
         * several branches and would have left the mutex held - which would
         * have wedged the dry run permanently rather than crashing, i.e. the
         * quiet kind of broken. */
        xSemaphoreGive(s_work_mtx);
    }
}

void wsprnet_init(void)
{
    s_work_mtx = xSemaphoreCreateMutex();
    if (!s_work_mtx) { ESP_LOGE(TAG, "no mutex - uploader not started"); return; }

    /* PSRAM stack: background, not latency-critical - the standing rule in
     * util/psram_task.h, and internal RAM is what the OTA verify runs out of. */
    /* 8192, not 5120. The work area is off the stack now, but
     * esp_http_client_perform() still needs real room and the first version
     * took a Stack protection fault here. pskreporter and update_check both
     * use 6144 for comparable work; the extra is PSRAM and costs nothing. */
    psram_task_create(wsprnet_task, "wsprnet", 8192, NULL, 2, tskNO_AFFINITY);
}
