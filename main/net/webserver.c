#include "webserver.h"
#include "webserver_ws.h"
#include "filebrowser.h"      // filebrowser_register - microSD web file browser

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "battery.h"          // battery_get_level, battery_is_charging
#include "wifi.h"             // wifi_get_ssid, wifi_get_rssi_dbm, wifi_get_ip
#include "cat.h"              // cat_get_frequency, cat_get_band_list, cat_set_*
#include "ui.h"               // ui_get_*, ui_set_zoom
#include "qmx_term.h"         // /api/term
#include "ui/qmx_term_view.h" // the dev "term_view" action
#include "ft8_screen_view.h"  // ft8_screen_view_is_active
#include "ft8_tx.h"           // ft8_tx_get_status (web TX-status banner)
#include "ft8_qso.h"          // ft8_qso_get_state / get_target / get_cq_calls_sent
#include "ft8_status.h"       // ft8_status_get
#include "dsp.h"              // dsp_get_peak_dbm_around_vfo
#include "display/display.h"  // display_lock / display_unlock
#include "ui/reader_view.h"   // reader_view_open_help - the /api/cmd "help" action
#include "screenshot/screenshot.h"  // screenshot_capture_rgb565
#include "diag_log.h"         // diag_log_size / diag_log_snapshot
#include "adif/adif_log.h"    // adif_log_count / adif_log_file_path / adif_log_clear
#include "storage/sd_archive.h"  // sd_archive_is_mounted / sd_archive_log_path / lock / unlock
#include "adif/qrz_upload.h"  // qrz_upload_pending
#include "adif/eqsl_upload.h" // eqsl_upload_pending
#include "adif/lotw_upload.h" // lotw_upload_pending / cert storage
#include "settings.h"          // settings_load_all / settings_set_qrz_api_key
#include "factory_reset.h"     // factory_reset_request (web-triggered NVS reset)
#include "bandplan.h"          // bandplan_get_segments / _effective_region / _seg_color
#include "spots.h"             // spots_get_in_range - live spots for the web spectrum
#include "psk_rx.h"            // propagation feedback - who is hearing US
#include "bt_hid_mouse.h"
#include "hid_cursor.h"
#include "iq_balance.h"        // iq_balance_set_enabled - /api/settings
#include "spur_map.h"          // spur_map_set_enabled - /api/settings
#include "mem_channels.h"      // memory channels - /api/memory
#include "render_waterfall.h"  // live waterfall tuning - /api/settings display group
#include "ft8_pileup.h"        // pileup list - /api/decodes
#include "ft8_greylist.h"      // grey-list viewer - /api/decodes + greylist_clear
#include "time_sync.h"         // time_sync_get_effective_source - /api/status time_src
#include "ui/help_topics.h"    // help_triage_collect / help_topic_get - /api/help
#include "ft8_screen.h"        // decode table + shared ordering - /api/decodes
#include "ft8_robot.h"         // ft8_robot_stand_down - band change stops auto-answer
#include "maidenhead.h"        // km/bearing for /api/decodes, same math as the Tab5
#include "ft8_test.h"          // ft8_op_mode_get / ft8_op_mode_slot_ms
#include <ctype.h>
#include "net/manual_embed.h"  // manual_embed_get - /api/manual serves the built-in manual
#include "config_io.h"         // config_io_export / config_io_import
#include "usb_replug.h"        // usb_replug (hidden /api/cmd recovery action)
#include "util/usb_shutdown.h" // usb_shutdown_graceful - "prepare for flashing"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_timer.h"         // web tune 60 s safety timeout
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps / vTaskDeleteWithCaps
#include "freertos/queue.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "webserver";

static httpd_handle_t s_server = NULL;

// Background upload task: processes QRZ/eQSL uploads without blocking httpd.
// Runs at priority 3 (below audio/FT8, above idle). Clients poll /api/upload_status
// to check results instead of blocking the request handler.
// UPLOAD_NONE = 0 is the "no upload has run yet" sentinel: s_last_upload is
// zero-initialised, so kind starts here. Real kinds MUST be non-zero - the
// upload_status handler uses "kind != UPLOAD_NONE" to decide whether to emit
// the result fields, and QRZ being 0 previously made a finished QRZ upload
// look like "no result" (browser read uploaded=undefined -> "undefined QSOs").
typedef enum { UPLOAD_NONE = 0, UPLOAD_QRZ, UPLOAD_EQSL, UPLOAD_LOTW } upload_kind_t;
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

// node-forge (BSD-3-Clause), pre-gzipped, embedded via EMBED_FILES. Served
// with Content-Encoding: gzip for the web UI's browser-side p12 parsing -
// the LoTW cert import. Loaded lazily by the page only when that dialog
// opens, so it costs nothing on a normal page load.
extern const uint8_t forge_js_gz_start[] asm("_binary_forge_min_js_gz_start");
extern const uint8_t forge_js_gz_end[]   asm("_binary_forge_min_js_gz_end");

static esp_err_t root_handler(httpd_req_t *req)
{
    const size_t len = index_html_end - index_html_start - 1;  // strip NUL
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, len);
}

// FT8/FT4 TX + QSO state for the web page's status banner (Dennis WN4FLA:
// see from another room when the radio stopped calling CQ / timed out and
// needs attention). Mirrors the priority ladder of the Tab5's own left-pane
// label (t_clock_cb in ft8_screen_view.c): ACTIVE > ARMED > DONE > TIMEOUT >
// session-alive > ft8_status passthrough. The clash check is deliberately
// omitted - ft8_tx_is_clashing() walks a heap snapshot of the heard-station
// table and this runs on every 1 Hz status poll.
static void add_ft8_tx_status(cJSON *root)
{
    cJSON *f = cJSON_AddObjectToObject(root, "ft8");
    if (!f) return;

    char tx_text[32];
    int  secs_until = 0;
    ft8_tx_state_t  tx_st  = ft8_tx_get_status(tx_text, sizeof(tx_text), &secs_until);
    ft8_qso_state_t qso_st = ft8_qso_get_state();

    // CQ auto-stop progress, same wording as the Tab5 label.
    char cq_line[32] = "";
    int  cq_sent = ft8_qso_get_cq_calls_sent();
    if (cq_sent >= 0 && tx_st != FT8_TX_IDLE) {
        qmx_settings_t cfg;
        settings_load_all(&cfg);
        if (cfg.cq_max_calls > 0)
            snprintf(cq_line, sizeof(cq_line), " - call %d of %d", cq_sent + 1, cfg.cq_max_calls);
        else
            snprintf(cq_line, sizeof(cq_line), " - call %d", cq_sent + 1);
    }

    const char *st;
    char b[160];
    if (tx_st == FT8_TX_ACTIVE) {
        st = "active";
        snprintf(b, sizeof(b), "Transmitting: %s%s", tx_text, cq_line);
    } else if (tx_st == FT8_TX_ARMED) {
        st = "armed";
        snprintf(b, sizeof(b), "TX armed: %s%s (~%ds)", tx_text, cq_line, secs_until);
    } else if (qso_st == FT8_QSO_DONE) {
        st = "done";
        char target[FT8_CALL_MAX_LEN];
        ft8_qso_get_target(target, sizeof(target));
        snprintf(b, sizeof(b), "QSO %s: complete!", target);
    } else if (qso_st == FT8_QSO_TIMEOUT) {
        st = "timeout";
        char target[FT8_CALL_MAX_LEN];
        ft8_qso_get_target(target, sizeof(target));
        snprintf(b, sizeof(b), "QSO %s: timeout", target);
    } else if (qso_st == FT8_QSO_CQ || qso_st == FT8_QSO_WAIT_RPT ||
               qso_st == FT8_QSO_WAIT_ROGER || qso_st == FT8_QSO_WAIT_RR73) {
        // Session alive, nothing armed (busy-station hold, CQ auto-stop's
        // final listening slot, or a transient between re-arms).
        st = "wait";
        char status[96];
        ft8_status_get(status, sizeof(status));
        snprintf(b, sizeof(b), "%s", status[0] ? status : "QSO waiting");
    } else {
        // Idle passthrough - this is where "CQ stopped after N calls - no
        // answer" (the auto-stop's persistent message) shows up.
        st = "idle";
        char status[96];
        ft8_status_get(status, sizeof(status));
        snprintf(b, sizeof(b), "%s", status[0] ? status : "Idle");
    }
    cJSON_AddStringToObject(f, "st",   st);
    cJSON_AddStringToObject(f, "text", b);
    // Outcome of the last browser-initiated reply ("Armed: ...", "Busy: ...",
    // "X is no longer in the decode list"). Sticky until the next request.
    const char *wr = ft8_screen_view_get_web_reply_result();
    if (wr && wr[0]) cJSON_AddStringToObject(f, "web_r", wr);
}

// --- Web Antenna Tune (QMX 1.04+) -------------------------------------------
//
// Tune KEYS THE RADIO CONTINUOUSLY, so this session carries the same safety
// rails as the Tab5's tune_modal.c, independently:
//   * hard 60 s timeout (esp_timer) that restores the prior mode - a dropped
//     browser tab must never leave a carrier on the air
//   * the prior mode is restored on stop, never a bare MD0; (the CAT manual's
//     Set list has no 0 - see docs/qmx-1_04-cat-comparison.md)
//   * NO radio-side exit detection, because none is possible: after MD8; the
//     QMX answers MD; with the PRE-Tune mode digit, not 8 (CLAUDE.md, "MD;
//     reports the PRE-Tune mode while the radio is tuning"). This code used to
//     read ui_get_mode_str() every status poll and stand the session down when
//     it wasn't "TUNE" - which was true from the FIRST poll, so every web tune
//     silently cancelled itself ~1 s in: it stopped the 60 s safety timer and
//     the SWR poll WITHOUT restoring the mode, leaving the radio keyed with no
//     automatic recovery, and made tune_stop a no-op (web_tune_stop returns
//     early on !s_web_tune_active). Fixed 2026-08-09. tune_modal.c had already
//     learned this on 2026-07-03; this file re-made the mistake independently.
//     The 60 s timeout is the only automatic exit, exactly as on the Tab5.
// Gated on cat_qmx_fw_at_least(1,4,0) like the Tab5 button. If the Tab5's own
// tune modal is in use at the same moment the two would fight over the mode -
// single-operator device, judged acceptable, same as two fingers on one radio.
static volatile bool s_web_tune_active = false;
static char          s_web_tune_prior[8] = "USB";
static esp_timer_handle_t s_web_tune_timer = NULL;

static void web_tune_stop(bool restore)
{
    if (!s_web_tune_active) return;
    s_web_tune_active = false;
    if (s_web_tune_timer) esp_timer_stop(s_web_tune_timer);
    cat_tune_poll_set_active(false);
    if (restore) cat_request_mode(s_web_tune_prior);
    // The label has to be put back explicitly: the MD; poll only calls
    // ui_update_mode() when the DIGIT changes, and the digit never changed
    // (it read the pre-Tune mode the whole time), so nothing else will ever
    // clear the "TUNE" we set on start.
    ui_update_mode(s_web_tune_prior);
    ESP_LOGW(TAG, "web tune: stopped (%s)", restore ? "mode restored" : "radio already out");
}

static void web_tune_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "web tune: 60 s safety timeout");
    web_tune_stop(true);
}

static bool web_tune_start(void)
{
    if (!cat_qmx_fw_at_least(1, 4, 0)) return false;
    if (s_web_tune_active) return true;               // idempotent
    // Never over an FT8 burst - the poll pause and the mode write would fight.
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE) return false;
    // Never while the operator has released the radio: Tune keys a carrier, and
    // the mode write it needs would go into a menu they are standing in front of.
    if (cat_user_pause_active()) return false;
    const char *cur = cat_get_mode_str();
    snprintf(s_web_tune_prior, sizeof(s_web_tune_prior), "%s",
             (cur && cur[0] && strcmp(cur, "TUNE") != 0) ? cur : "USB");
    if (!s_web_tune_timer) {
        const esp_timer_create_args_t a = { .callback = web_tune_timeout_cb, .name = "web_tune" };
        if (esp_timer_create(&a, &s_web_tune_timer) != ESP_OK) return false;
    }
    s_web_tune_active = true;
    cat_request_mode("TUNE");
    cat_tune_poll_set_active(true);
    esp_timer_start_once(s_web_tune_timer, 60 * 1000000LL);
    // Say so on the Tab5 as well (TODO #95d). A tune started from a browser
    // keys the radio for up to a minute while anyone standing at the Tab5 sees
    // nothing at all - the top bar keeps showing the pre-Tune mode, because
    // MD; reports that throughout (see CLAUDE.md). Someone in the room has to
    // be able to tell the transmitter is on and that it was not them.
    ui_update_mode("TUNE");
    ui_toast(LV_SYMBOL_WARNING " Antenna Tune started from the web UI");
    ESP_LOGW(TAG, "web tune: STARTED (prior mode %s, 60 s limit)", s_web_tune_prior);
    return true;
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
    cJSON_AddStringToObject(root, "screen",       ft8_screen_view_is_active() ? "ft8" : "panadapter");
    // TX/QSO banner data, only while the FT8/FT4 screen is live (the FT8
    // engine doesn't run otherwise, so its status would be stale text).
    if (ft8_screen_view_is_active())
        add_ft8_tx_status(root);
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
    // RIT offset in Hz, 0 = off. Radio state, so the browser and the Tab5 pill show
    // the same number and the browser can draw the marker in the same place.
    cJSON_AddNumberToObject(root, "rit_hz",      (double)cat_get_rit_hz());

    // POTA/SOTA activation session. Sent on every poll because the hazard this
    // feature has is FORGETTING one is running - every QSO logged meanwhile claims
    // a reference - so the browser must be able to say so unprompted, exactly as
    // the Tab5's bottom bar does.
    //
    // The contact count comes from adif_log_count_activation(), which walks the log
    // file, so it is cached for ACT_COUNT_TTL_US rather than run at 1 Hz on the
    // httpd task. Only computed while a session is actually running.
    {
        cJSON *act = cJSON_AddObjectToObject(root, "activation");
        uint8_t at = settings_get_activation_type();
        char aref[24] = "";
        settings_get_activation_ref(aref, sizeof aref);
        cJSON_AddNumberToObject(act, "type", (double)at);
        cJSON_AddStringToObject(act, "ref", aref);
        if (at != 0 && aref[0]) {
            static const int64_t ACT_COUNT_TTL_US = 10 * 1000 * 1000;
            static int64_t s_act_count_at = 0;
            static int     s_act_count    = 0;
            static char    s_act_count_ref[24] = "";
            int64_t now = esp_timer_get_time();
            if (s_act_count_at == 0 || now - s_act_count_at > ACT_COUNT_TTL_US ||
                strcmp(s_act_count_ref, aref) != 0) {
                s_act_count = adif_log_count_activation(aref);
                s_act_count_at = now;
                snprintf(s_act_count_ref, sizeof s_act_count_ref, "%s", aref);
            }
            cJSON_AddNumberToObject(act, "qsos", (double)s_act_count);
        }
    }
    cJSON_AddBoolToObject  (root, "flat_mode",   ui_get_flat_mode());
    // Waterfall colourisation, so the browser can paint the SAME picture instead of
    // inventing its own. In the 1 Hz status poll rather than only in /api/settings
    // because that is fetched when the settings form opens - these four need to
    // follow a slider dragged on the Tab5 itself, within a second, which is the
    // whole point of them being here. Four small numbers.
    {
        qmx_settings_t ws;
        settings_load_all(&ws);
        cJSON *w = cJSON_AddObjectToObject(root, "wf");
        cJSON_AddNumberToObject(w, "black",    ws.wf_black_db);
        cJSON_AddNumberToObject(w, "contrast", ws.wf_contrast_db);
        // PERCENT, not a fraction - named so, because the stored setting is 0..100
        // while render_waterfall_set_floor_blend() takes 0..1 and both of its
        // callers divide by 100. I got this wrong once by sending it as "blend".
        cJSON_AddNumberToObject(w, "blend_pct", ws.wf_floor_blend);
        cJSON_AddNumberToObject(w, "cmap",     ws.colormap_idx);
        // The spectrum EMA the operator controls. render.c applies this to the raw
        // spectrum before the Tab5 renders anything, so the browser has to apply it
        // too or it is drawing a noisier picture from the same data - and the
        // smoothing slider does nothing there.
        cJSON_AddNumberToObject(w, "ema_pct",  (int)(ws.ema_alpha * 100.0f + 0.5f));
    }
    cJSON_AddNumberToObject(root, "utc_epoch",   (double)time(NULL));
    // What is maintaining the clock - same authority the Tab5's bottom-bar
    // "UTC(NTP)"/"UTC(QMX)" suffix shows, so the two labels can never disagree.
    {
        // SNTP is reported as NTP UNCONDITIONALLY, exactly as status.c does.
        // This used to read "GPS" whenever the remembered GPS verdict was set -
        // but the effective source is SNTP precisely when GPS is NOT live, so
        // that printed GPS at the moment the radio carrying the GPS was absent.
        // That is Don N2VGU's 2026-08-09 report, and it was fixed on the Tab5
        // while this copy of the same decision was missed: the browser went on
        // claiming GPS accuracy with the QMX unplugged. Found 2026-08-09 while
        // verifying the Tab5 fix from a screenshot. The comment above once said
        // these two labels "can never disagree" - they had already diverged,
        // which is the argument for deriving the string in ONE place.
        time_sync_source_t ts = time_sync_get_effective_source();
        const char *tsn = ts == TIME_SOURCE_SNTP   ? "NTP"
                        : ts == TIME_SOURCE_QMX    ? (time_sync_qmx_gps_confirmed() ? "GPS" : "QMX")
                        : ts == TIME_SOURCE_RTC    ? "RTC"
                        : ts == TIME_SOURCE_FT8    ? "FT8"
                        : ts == TIME_SOURCE_MANUAL ? "manual" : "none";
        cJSON_AddStringToObject(root, "time_src", tsn);
    }
    cJSON_AddNumberToObject(root, "qso_count",   (double)adif_log_count());
    {
        qmx_settings_t cfg;
        settings_load_all(&cfg);
        cJSON_AddBoolToObject(root, "qrz_key_set", cfg.qrz_api_key[0] != '\0');
        cJSON_AddBoolToObject(root, "eqsl_creds_set", cfg.eqsl_user[0] != '\0' && cfg.eqsl_pswd[0] != '\0');
        cJSON_AddBoolToObject(root, "lotw_ready", lotw_cert_present() && cfg.lotw_dxcc[0] != '\0');

        // Band-plan for the current band — whole-band strip on the web UI,
        // mirrors update_bandplan_strip() in ui.c. Null if the VFO isn't inside
        // a known amateur band (e.g. way off-band). Sent every status poll (no
        // new endpoint/poller — the web httpd is fragile under extra load).
        bandplan_region_t reg = bandplan_effective_region(
            (bandplan_region_t)cfg.bandplan_region, cfg.my_grid);
        const bp_seg_t *segs = NULL;
        int nseg = bandplan_get_segments(cat_get_frequency(), reg, &segs);
        if (nseg > 0 && segs) {
            cJSON *bp = cJSON_AddObjectToObject(root, "bandplan");
            cJSON_AddNumberToObject(bp, "lo", (double)segs[0].lo_hz);
            cJSON_AddNumberToObject(bp, "hi", (double)segs[nseg - 1].hi_hz);
            cJSON *sarr = cJSON_AddArrayToObject(bp, "segs");
            for (int i = 0; i < nseg; i++) {
                cJSON *sg = cJSON_CreateObject();
                cJSON_AddNumberToObject(sg, "lo", (double)segs[i].lo_hz);
                cJSON_AddNumberToObject(sg, "hi", (double)segs[i].hi_hz);
                char cbuf[8];
                snprintf(cbuf, sizeof(cbuf), "%06lX",
                         (unsigned long)bandplan_seg_color(segs[i].type));
                cJSON_AddStringToObject(sg, "c", cbuf);
                cJSON_AddStringToObject(sg, "l", bandplan_seg_label(segs[i].type));
                cJSON_AddItemToArray(sarr, sg);
            }
        } else {
            cJSON_AddNullToObject(root, "bandplan");
        }

        // Live spots for the browser's own spectrum, from the SAME store the
        // Tab5 lane draws from (net/spots.c) - so the two can never disagree
        // about who is on the air. Rides this status poll for the same reason
        // the band-plan does: no new endpoint, no new poller.
        //
        // Scoped to the band, not to the browser's visible window: the window is
        // the browser's own business (it zooms and pans without telling us), and
        // the band scope is also what the off-screen counts need. Same reasoning
        // as the Tab5 lane, which counts per band rather than across all of HF.
        //
        // Only while the panadapter is on screen. In FT8 the browser hides the
        // spectrum anyway, so this would be payload nobody draws.
        // Band for the spots, from the DIAL rather than the CAT poll (see
        // ui_get_dial_freq_hz): with the radio off, cat reads 0 while the Tab5
        // is still showing 20 m with a full lane of spots on it. Keyed to cat,
        // the browser would show an empty band under a Tab5 that is showing
        // twenty stations - the disagreement this whole payload exists to avoid.
        const bp_seg_t *sp_segs = segs;
        int sp_nseg = nseg;
        if (sp_nseg <= 0) {
            uint32_t dial = ui_get_dial_freq_hz();
            if (dial) sp_nseg = bandplan_get_segments(dial, reg, &sp_segs);
        }
        if (spots_any_source_enabled() && sp_nseg > 0 && sp_segs && !ft8_screen_view_is_active()) {
            // The spot store changes only when a source refreshes (POTA every few
            // minutes), but this poll runs every second. Sending ~6 KB of
            // unchanged JSON 60 times a minute on a link this fragile is exactly
            // the kind of load that has wedged the web server before, so the
            // browser sends back the version it already has (?sv=N) and gets the
            // array only when it is stale. spots_v is ALWAYS sent, so "no spots
            // key" is never ambiguous: it means "you are up to date".
            uint32_t sv = spots_version();
            cJSON_AddNumberToObject(root, "spots_v", (double)sv);
            char qbuf[64] = {0}, svbuf[16] = {0};
            bool want = true;
            if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
                httpd_query_key_value(qbuf, "sv", svbuf, sizeof(svbuf)) == ESP_OK &&
                svbuf[0] && (uint32_t)strtoul(svbuf, NULL, 10) == sv) {
                want = false;
            }
          if (want) {
            // ~48 B per spot: too big for the httpd task's stack (CLAUDE.md -
            // task stacks on this board are tiny), and under the 16 KB that
            // plain malloc would force into internal RAM, so ask for PSRAM.
            //
            // 96, not 64: a busy 20 m afternoon put 64 on ONE band, which is the
            // cap - i.e. it was already truncating, the same way SPOTS_MAX's
            // first cut did (see spots.h). The count that was found is sent as
            // spots_total regardless, so a truncation can never be silent.
            const int MAXSP = 96;
            spot_t *sp = heap_caps_malloc(sizeof(spot_t) * MAXSP,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (sp) {
                int ns = spots_get_in_range(sp, MAXSP, sp_segs[0].lo_hz, sp_segs[sp_nseg - 1].hi_hz);
                cJSON *sarr = cJSON_AddArrayToObject(root, "spots");
                int64_t now = (int64_t)time(NULL);
                for (int i = 0; i < ns; i++) {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, "c", sp[i].call);
                    if (sp[i].ref[0]) cJSON_AddStringToObject(o, "r", sp[i].ref);
                    cJSON_AddNumberToObject(o, "f", (double)sp[i].freq_hz);
                    cJSON_AddStringToObject(o, "m",
                        sp[i].mode == SPOT_MODE_CW   ? "cw"   :
                        sp[i].mode == SPOT_MODE_SSB  ? "ssb"  :
                        sp[i].mode == SPOT_MODE_DIGI ? "digi" : "");
                    // The browser only asks "is this RBN?" (anything else gets the
                    // activation colour), so naming SOTA here needs no browser
                    // change - but calling a summit "pota" in the API would be a
                    // plain untruth for anything else reading it.
                    cJSON_AddStringToObject(o, "s",
                        sp[i].source == SPOT_SRC_RBN  ? "rbn"  :
                        sp[i].source == SPOT_SRC_SOTA ? "sota" : "pota");
                    // Age in seconds, so the browser can fade exactly as the Tab5
                    // does without needing the clocks to agree.
                    int age = (int)(now - sp[i].heard_unix);
                    cJSON_AddNumberToObject(o, "a", age < 0 ? 0 : age);
                    // Worked on THIS band - the whole point of the grey state.
                    cJSON_AddBoolToObject(o, "w",
                        sp[i].call[0] && adif_log_contains_call_on_band(sp[i].call, sp[i].freq_hz));
                    cJSON_AddItemToArray(sarr, o);
                }
                cJSON_AddNumberToObject(root, "spots_total", ns);
                heap_caps_free(sp);
            }
          }
        }
    }
    // Web tune session: live power/SWR while active. There is deliberately NO
    // stand-down-if-the-radio-left-TUNE check here - see below.
    if (s_web_tune_active) {
        cJSON *tn = cJSON_AddObjectToObject(root, "tune");
        float pw = 0, swr = 0;
        cat_pwr_swr_async_read(&pw, &swr);
        cJSON_AddNumberToObject(tn, "watts", pw);
        cJSON_AddNumberToObject(tn, "swr",   swr);
    }
    // Bluetooth, mirroring the Tab5's bottom-bar glyph: "enabled" and
    // "something is actually connected" are separate facts.
    {
        qmx_settings_t bs;
        settings_load_all(&bs);
        cJSON *bt = cJSON_AddObjectToObject(root, "bt");
        cJSON_AddBoolToObject(bt, "en",   bs.bt_mouse_en && bt_hid_mouse_started());
        cJSON_AddBoolToObject(bt, "conn", hid_cursor_present());
    }
    cJSON_AddBoolToObject(root, "tune_ok", cat_qmx_fw_at_least(1, 4, 0));
    // Paused = the operator has released the radio to its own menu. Everything
    // frequency-, mode- and decode-shaped in this payload is frozen while it is
    // true, so the browser needs to say so rather than show stale numbers as if
    // they were live.
    cJSON_AddBoolToObject(root, "paused", cat_user_pause_active());

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
            // Same rule as the Tab5's own band buttons: a band change stands
            // auto-answer down, because the antenna is probably not tuned for
            // the new band. Doing it from the browser must not be the loophole.
            ft8_robot_stand_down("band changed");
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
    } else if (action && strcmp(action, "set_rit") == 0) {
        // RIT offset in Hz; 0 clears it and switches RIT off at the radio. Same
        // entry point the Tab5's tap-to-RIT uses, so the write goes through the
        // poll task with the mandatory RC; in front of it (see cat.c), and both
        // screens then show the one offset that is actually set.
        //
        // No "arm" here on purpose: arming is a property of the input surface, and
        // the browser owns its own. cat_request_rit_hz() clamps to CAT_RIT_MAX_HZ.
        cJSON *item = cJSON_GetObjectItem(root, "hz");
        if (cJSON_IsNumber(item)) {
            cat_request_rit_hz((int)item->valuedouble);
            ESP_LOGI(TAG, "web: RIT -> %+d Hz", (int)item->valuedouble);
        }
    } else if (action && strcmp(action, "set_activation") == 0) {
        // Start or stop a POTA/SOTA activation. type 0 stops; 1 = POTA, 2 = SOTA
        // with a reference. Mirrors activation_modal.c's go button, including its
        // refusals: a reference is required to start, and settings_set_activation()
        // is the thing that decides whether one is usable (it trims), so the answer
        // is read back from it rather than guessed here.
        cJSON *jt = cJSON_GetObjectItem(root, "type");
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ref"));
        int t = cJSON_IsNumber(jt) ? (int)jt->valuedouble : -1;
        const char *err = NULL;
        if (t == 0) {
            settings_set_activation(0, NULL);
            ESP_LOGI(TAG, "web: activation stopped");
        } else if (t == 1 || t == 2) {
            if (!ref || !ref[0]) {
                err = "Enter the park or summit reference first";
            } else {
                settings_set_activation((uint8_t)t, ref);
                if (settings_get_activation_type() == 0) err = "That reference is not usable";
                else ESP_LOGI(TAG, "web: activation started: %s %s", t == 1 ? "POTA" : "SOTA", ref);
            }
        } else {
            err = "Unknown activation type";
        }
        if (err) {
            cJSON_Delete(root);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            char msg[96];
            snprintf(msg, sizeof msg, "{\"ok\":false,\"error\":\"%s\"}", err);
            httpd_resp_sendstr(req, msg);
            return ESP_OK;
        }
    } else if (action && strcmp(action, "cq_start") == 0) {
        // Restart a CQ run from the browser (Dennis WN4FLA): a CQ that has timed out
        // or hit its call limit otherwise needs a walk back to the Tab5. Deferred to
        // the LVGL task inside ft8_screen_view - do NOT call the QSO machine from
        // this HTTP task.
        ft8_screen_view_request_cq();
    } else if (action && strcmp(action, "set_screen") == 0) {
        // Switch the Tab5 between the panadapter and FT8/FT4 from the browser.
        // Deferred to the LVGL task (see ui_request_base_mode) - the switch
        // spawns/stops ft8_task and moves widgets.
        const char *scr = cJSON_GetStringValue(cJSON_GetObjectItem(root, "screen"));
        if (scr) ui_request_base_mode(strcmp(scr, "ft8") == 0);
    } else if (action && strcmp(action, "reply") == 0) {
        // Reply to a decoded station from the browser - Phase 6 of web parity,
        // TX explicitly blessed by the operator. Deferred to the LVGL task like
        // cq_start; the outcome comes back through /api/status's ft8 block
        // (web_r), because a Tab5 toast is invisible from another room.
        const char *call = cJSON_GetStringValue(cJSON_GetObjectItem(root, "call"));
        if (call && call[0]) ft8_screen_view_request_reply(call);
    } else if (action && strcmp(action, "tune_start") == 0) {
        if (!web_tune_start()) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "Tune needs QMX firmware 1.04+ and an idle transmitter");
            return ESP_FAIL;
        }
    } else if (action && strcmp(action, "tune_stop") == 0) {
        web_tune_stop(true);
    } else if (action && strcmp(action, "greylist_clear") == 0) {
        // Un-skip everyone - band conditions change, and a station that timed
        // out an hour ago may be perfectly workable now.
        ft8_greylist_clear_all();
    } else if (action && strcmp(action, "help") == 0) {
        // Open the Tab5's own manual at a page (and optionally a heading), the
        // same call the context-help buttons make. Sending someone to the right
        // page from the browser is squarely in the parity theme, and it is also
        // the only way to see a Reader change without a finger on the glass.
        //
        // display_lock is recursive and safe cross-thread - the same reason
        // ui_toast() takes it when the decode task calls it.
        const char *page = cJSON_GetStringValue(cJSON_GetObjectItem(root, "page"));
        const char *anch = cJSON_GetStringValue(cJSON_GetObjectItem(root, "anchor"));
        if (!page || !page[0]) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "help needs a page");
            return ESP_FAIL;
        }
        if (display_lock(500)) {
            reader_view_open_help(page, anch);
            display_unlock();
        }
    } else if (action && strcmp(action, "pause") == 0) {
        // Release the radio to its own front panel (Stan's pause button, via
        // Samuel W7STF). Reachable from the browser as well as the drawer for
        // the same reason everything else is: the operator may be at the radio
        // with the Tab5 across the room, which is precisely when they want the
        // Tab5 to stop talking.
        ui_set_cat_paused(true);
    } else if (action && strcmp(action, "resume") == 0) {
        ui_set_cat_paused(false);
    } else if (action && strcmp(action, "usb_shutdown") == 0) {
        // Orderly USB teardown before the operator re-flashes (see
        // util/usb_shutdown.h). Blocking and bounded; the browser gets its
        // {"ok":true} once the radio is safely off the bus.
        usb_shutdown_graceful();
    } else if (action && strcmp(action, "usb_replug") == 0) {
        // Hidden dev/recovery action: emulate a physical USB-A unplug/replug
        // (root-port power cycle + VBUS cut). Optional "off_ms" (200..8000).
        cJSON *item = cJSON_GetObjectItem(root, "off_ms");
        uint32_t off_ms = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 2000;
        usb_replug(off_ms);
    } else if (action && strcmp(action, "drawer") == 0) {
        // Hidden dev action, like resmon below: open or close the Tab5's own
        // settings drawer. No web UI element references it - the browser has
        // its own Settings. It exists so a drawer layout change can be checked
        // on a screenshot instead of being taken on trust.
        cJSON *o = cJSON_GetObjectItem(root, "open");
        bool want = cJSON_IsBool(o) ? cJSON_IsTrue(o) : true;
        cJSON *ex = cJSON_GetObjectItem(root, "expert");
        cJSON *sy = cJSON_GetObjectItem(root, "scroll_y");
        if (display_lock(500)) {
            if (cJSON_IsBool(ex)) ui_set_drawer_expert(cJSON_IsTrue(ex));
            ui_set_drawer_open(want);
            // After open (which always scrolls to the top), so a section below
            // the fold can be brought into a screenshot.
            if (want && cJSON_IsNumber(sy)) ui_set_drawer_scroll_y((int)sy->valuedouble);
            display_unlock();
        }
    } else if (action && strcmp(action, "term_view") == 0) {
        // Hidden dev action, same reason as "drawer" above: open or close the
        // Tab5's own Radio-menus screen so its layout can be checked on a
        // screenshot rather than by asking the operator to tap it. No web UI
        // element references it - the browser has its own terminal page.
        cJSON *o = cJSON_GetObjectItem(root, "open");
        bool want = cJSON_IsBool(o) ? cJSON_IsTrue(o) : true;
        if (display_lock(500)) {
            if (want) qmx_term_view_open();
            else      qmx_term_view_close();
            display_unlock();
        }
    } else if (action && strcmp(action, "cat_raw") == 0) {
        // Developer escape hatch: send one raw CAT string to the radio. The
        // reply arrives on the normal RX path and is logged in full (non-poll
        // traffic is never de-duplicated), so read it from the diagnostic log.
        //
        // Exists for menu DISCOVERY: the QMX's MM command can Get, Set or Query
        // any configuration item by path, which means the exact menu tokens can
        // be ASKED FOR rather than guessed - and CLAUDE.md records that guessing
        // MM tokens has burned real time before.
        const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cmd"));
        if (!c || !c[0]) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"needs cmd\"}");
            return ESP_OK;
        }
        esp_err_t e = cat_send_raw_cmd("%s", c);
        cJSON_Delete(root);
        char out[64];
        snprintf(out, sizeof(out), "{\"ok\":%s}", e == ESP_OK ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    } else if (action && strcmp(action, "qmx_term_probe") == 0) {
        // Open port 2, press Enter, hex-dump the reply to the log. CAT on port 1
        // is not involved, so this cannot take the panadapter down.
        int n = cat_probe_terminal();
        cJSON_Delete(root);
        char out[64];
        snprintf(out, sizeof(out), "{\"ok\":%s,\"bytes\":%d}", n >= 0 ? "true" : "false", n);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    } else if (action && strcmp(action, "qmx_ports") == 0) {
        // Read-only: how many virtual COM ports does this radio expose? Decides
        // whether a Tab5 terminal can have its own port or would have to borrow
        // the CAT pipe. Writes nothing to the radio.
        int n = cat_probe_extra_cdc_ports();
        cJSON_Delete(root);
        char out[64];
        snprintf(out, sizeof(out), "{\"ok\":true,\"cdc_ports\":%d}", n);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    } else if (action && strcmp(action, "power_off") == 0) {
        // Shut the Tab5 down after putting the radio back into receive. Needs
        // {"confirm":"POWEROFF"} - this is not something to trigger by fat
        // fingers or a stale browser tab, and it does not come back on its own.
        //
        // Deliberately reachable from the API: Randy N4OPI runs a HEADLESS
        // QMX+, and a headless station is exactly where "shut down safely
        // without walking over to it" earns its place.
        const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(root, "confirm"));
        if (!c || strcmp(c, "POWEROFF") != 0) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"needs confirm=POWEROFF\"}");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "power off requested over HTTP");
        // Answer BEFORE powering down, or the caller sees a dropped connection
        // and cannot tell "it worked" from "it crashed".
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"powering_off\":true}");
        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(150));
        ui_power_off_safely();
        return ESP_OK;
    } else if (action && strcmp(action, "resmon") == 0) {
        // Hidden developer-only toggle for the resource-monitor overlay. No web
        // UI element references this — it's meant to be fired from the browser
        // console/bookmarklet on the dev's PC.
        ui_resource_monitor_toggle();
    } else if (action && strcmp(action, "reset_settings") == 0) {
        // Clear all app settings + memory channels (erases user_nvs on the next
        // boot), keeping the ADIF QSO log and WiFi. Replaces the esptool
        // full-erase for clearing a stuck stored value. Reboots to apply.
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
        factory_reset_request(true, false);
        return ESP_OK;
    } else if (action && strcmp(action, "reset_network") == 0) {
        // Clear WiFi/system NVS state (erases the default nvs partition on the
        // next boot), keeping all app settings. Clears a stuck WiFi/link state
        // that a normal reflash leaves in place. Reboots to apply.
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
        factory_reset_request(false, true);
        return ESP_OK;
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
// "log in the field, download at home" path and the post-crash lead-up.
//
// Stream one file's content as response chunks WITHOUT sending the
// terminating chunk, so several files can be concatenated into one
// download. Returns bytes streamed (0 if the file doesn't exist).
static size_t stream_file_part(httpd_req_t *req, const char *path, esp_err_t *err)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[1024];
    size_t n, total = 0;
    while (*err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        *err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        total += n;
    }
    fclose(f);
    return total;
}

static esp_err_t saved_log_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qmx-log-saved.txt");

    // Always serve the FLASH-persisted copy (rotated older generation first,
    // then the current file) - it is written every 30 s regardless of WiFi
    // or SD state, so it is always the freshest survivor of a crash, which
    // is what this endpoint exists for. The endpoint used to prefer the SD
    // card's qmx-log.txt whenever a card was mounted, which was wrong twice
    // over since the v1.3.2 WiFi-aware SD gate: with WiFi on the card file
    // either doesn't exist (Dennis WN4FLA got the empty placeholder right
    // after a crash, 2026-08-03) or is a STALE snapshot frozen at the last
    // WiFi-off session (operator's own unit served a months-old 512 KB
    // fragment) - both masking the fresh flash copy. The card's full deep
    // history remains available via the /files browser.
    esp_err_t err = ESP_OK;
    size_t total = 0;
    total += stream_file_part(req, diag_log_persist_path_rotated(), &err);
    total += stream_file_part(req, diag_log_persist_path(), &err);
    if (total == 0)
        return httpd_resp_sendstr(req, "(no saved diagnostic log yet)\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// GET /api/adif — download the ADIF QSO log from SPIFFS.
//
// ?activation=<REF> filters to the QSOs of ONE activation, which is what POTA
// and SOTA actually want uploaded - a park's log, not your whole life's. The
// filter is a line-wise MY_SIG_INFO match, which works because adif_log.c
// writes exactly one record per line. Case-insensitive, since a reference can
// reach the log from a config import or a hand edit in a different case.
static esp_err_t adif_get_handler(httpd_req_t *req)
{
    char act[24] = "";
    char date[16] = "";
    {
        size_t qlen = httpd_req_get_url_query_len(req) + 1;
        if (qlen > 1 && qlen < 256) {
            char q[256];
            if (httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
                httpd_query_key_value(q, "activation", act, sizeof(act));
                httpd_query_key_value(q, "date", date, sizeof(date));
            }
        }
    }

    // ?date=today (or YYYYMMDD) filters to one day's QSOs, and the file is named
    // for that day. Gyula HA3HZ files each day's contacts into cqrlog and needs
    // to tell the downloads apart: "Downloading the ADIF log daily and marking
    // the date in the downloaded file would be good."
    //
    // Resolved from the system clock, which is UTC on this device - the same
    // basis as the QSO_DATE field being matched, so "today" means the same thing
    // to both. A malformed value is treated as no filter rather than silently
    // matching nothing, since an empty ADIF looks like a lost log.
    char day[9] = "";
    if (date[0]) {
        if (strcasecmp(date, "today") == 0) {
            time_t now = time(NULL);
            struct tm tm;
            gmtime_r(&now, &tm);
            strftime(day, sizeof(day), "%Y%m%d", &tm);
        } else if (strlen(date) == 8) {
            bool digits = true;
            for (int i = 0; i < 8; i++) if (!isdigit((unsigned char)date[i])) digits = false;
            if (digits) snprintf(day, sizeof(day), "%s", date);
        }
        if (!day[0]) ESP_LOGW(TAG, "/api/adif: ignoring unusable date '%s'", date);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char cd[80];
    if (act[0]) {
        // Name the file after the reference - an activator ends up with one
        // file per park and needs to tell them apart later.
        snprintf(cd, sizeof(cd), "attachment; filename=%s.adi", act);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
    } else if (day[0]) {
        snprintf(cd, sizeof(cd), "attachment; filename=qso-%.4s-%.2s-%.2s.adi",
                 day, day + 4, day + 6);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
    } else {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=qso.adi");
    }

    // Pause the spectrum stream so this download has the WiFi TX path to itself.
    webserver_ws_set_paused(true);

    FILE *f = fopen(adif_log_file_path(), "r");
    if (!f) {
        esp_err_t err = httpd_resp_sendstr(req,
            "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n");
        webserver_ws_set_paused(false);
        return err;
    }

    esp_err_t err = ESP_OK;
    if (!act[0] && !day[0]) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && err == ESP_OK)
            err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    } else {
        // Line-wise so each record can be tested. The header line is always
        // emitted - an ADIF file without one is rejected by most loggers even
        // when every record in it is valid.
        char needle[40];
        if (act[0])
            snprintf(needle, sizeof(needle), "<MY_SIG_INFO:%u>%s", (unsigned)strlen(act), act);
        else
            snprintf(needle, sizeof(needle), "<QSO_DATE:8>%s", day);
        char line[1024];
        bool header_done = false;
        while (fgets(line, sizeof(line), f) && err == ESP_OK) {
            bool keep;
            if (!header_done) { header_done = true; keep = true; }
            else {
                keep = false;
                for (const char *p = line; *p && !keep; p++)
                    if (strncasecmp(p, needle, strlen(needle)) == 0) keep = true;
            }
            if (keep) err = httpd_resp_send_chunk(req, line, (ssize_t)strlen(line));
        }
    }
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

// POST /api/adif/delete?idx=<n>&call=<CALL> — delete ONE record (web ADIF
// viewer). The call must match the record currently at idx: the browser's
// copy of the log can be stale (a QSO may have logged since it fetched, and
// indices shift on every delete), and a bare index would then silently
// delete the wrong QSO. Mismatch = 409, viewer reloads and retries.
static esp_err_t adif_delete_handler(httpd_req_t *req)
{
    char query[96] = "", idx_s[12] = "", call_raw[24] = "", call[24] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "idx", idx_s, sizeof(idx_s)) != ESP_OK ||
        httpd_query_key_value(query, "call", call_raw, sizeof(call_raw)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "idx and call required");
        return ESP_FAIL;
    }
    // %-decode the call (portable calls carry '/', sent as %2F).
    size_t o = 0;
    for (size_t i = 0; call_raw[i] && o + 1 < sizeof(call); i++) {
        if (call_raw[i] == '%' && call_raw[i + 1] && call_raw[i + 2]) {
            char h[3] = { call_raw[i + 1], call_raw[i + 2], 0 };
            call[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            call[o++] = call_raw[i];
        }
    }
    call[o] = '\0';

    int idx = atoi(idx_s);
    char line[512], rec_call[24] = "";
    if (idx < 0 || !adif_log_get_record(idx, line, sizeof(line)) ||
        !adif_log_extract_field(line, "CALL", rec_call, sizeof(rec_call)) ||
        strcmp(rec_call, call) != 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"log changed - reload\"}");
    }
    bool ok = adif_log_delete_record(idx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}"
                                      : "{\"ok\":false,\"error\":\"delete failed\"}");
}

static const httpd_uri_t uri_adif_get = {
    .uri = "/api/adif", .method = HTTP_GET, .handler = adif_get_handler,
};
static const httpd_uri_t uri_adif_clear = {
    .uri = "/api/adif/clear", .method = HTTP_POST, .handler = adif_clear_handler,
};
static const httpd_uri_t uri_adif_delete = {
    .uri = "/api/adif/delete", .method = HTTP_POST, .handler = adif_delete_handler,
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

// POST /api/lotw_cert — JSON {"cert":"<b64 DER>","key":"<b64 PKCS#8 DER>",
// "dxcc":"221","cqz":"14","ituz":"18"}. The browser parsed the user's .p12
// with forge.js and sends only the extracted DER - the p12 passphrase never
// reaches the device. cert+key go to SPIFFS, station fields to NVS.
static esp_err_t lotw_cert_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 16384) {
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

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char *cert = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cert"));
    const char *key  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "key"));
    const char *dxcc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "dxcc"));
    const char *cqz  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cqz"));
    const char *ituz = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ituz"));
    // US subdivision - only meaningful for US stations, omitted by everyone
    // else. Without them a US operator's QSOs earn no WAS/county credit.
    const char *state  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
    const char *county = cJSON_GetStringValue(cJSON_GetObjectItem(root, "county"));
    bool ok = true;
    if (cert && key && cert[0] && key[0]) {
        // Is this actually a DIFFERENT certificate? The setup page is the only
        // place the station-location fields live, and re-running it needs the
        // .p12 picked again - so adding a state/county to an existing setup
        // re-submits the SAME cert. Rewinding on that would re-sign and re-send
        // the entire log for a two-field edit, which is why the comparison
        // exists rather than rewinding unconditionally.
        char *prev = lotw_read_cert_b64();
        bool cert_changed = (prev == NULL) || (strcmp(prev, cert) != 0);
        free(prev);

        ok = lotw_store_cert_b64(cert) == ESP_OK &&
             lotw_store_key_b64(key) == ESP_OK;
        // A NEW certificate means everything previously uploaded was signed
        // with a different key - rewind the cursor so the whole log is
        // re-signed and re-sent. LoTW discards records whose cert it can't
        // validate (field-hit 2026-07-14: a leftover bench test cert
        // "uploaded" 22 QSOs that LoTW's processing would silently drop,
        // then the advanced cursor blocked re-upload with the real cert),
        // and genuine re-sends after a cert renewal are just harmless
        // server-side duplicates.
        if (ok && cert_changed) {
            settings_set_lotw_uploaded_n(0);
            ESP_LOGI(TAG, "LoTW cert changed - upload cursor rewound to 0");
        } else if (ok) {
            ESP_LOGI(TAG, "LoTW cert unchanged - upload cursor left alone");
        }
    }
    if (dxcc)   settings_set_lotw_dxcc(dxcc);
    if (cqz)    settings_set_lotw_cqz(cqz);
    if (ituz)   settings_set_lotw_ituz(ituz);
    // Only apply when NON-EMPTY. The form doesn't pre-fill (nor do dxcc/cqz/
    // ituz), and Ctrl-click re-runs this whole flow for a cert renewal ~every
    // 3 years - so an empty box here means "I didn't retype it", not "clear
    // it". Wiping the state silently costs the operator WAS/county credit from
    // then on, which is exactly the failure this feature exists to fix. Clear
    // them deliberately via a config import (lotw_state = ) if ever needed.
    if (state  && state[0])  settings_set_lotw_state(state);
    if (county && county[0]) settings_set_lotw_county(county);
    cJSON_Delete(root);

    if (!ok) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
    // Freshly imported cert/key -> re-mirror them to the SD backup (if a card
    // is in). The NVS dxcc/cqz/ituz above already dirty the config mirror.
    sd_archive_mark_lotw_dirty();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/lotw_upload — queues upload, returns 202 Accepted immediately.
static esp_err_t lotw_upload_handler(httpd_req_t *req)
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
    s_last_upload.kind = UPLOAD_LOTW;
    s_last_upload.uploaded = 0;
    s_last_upload.failed = 0;
    s_last_upload.error[0] = '\0';
    xSemaphoreGive(s_upload_mutex);

    upload_request_t up = { .kind = UPLOAD_LOTW };
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

// GET /api/lotw_tq8 — the full ADIF log signed into one downloadable .tq8
// (all records, regardless of the upload cursor). Doubles as the offline
// verification path: gunzip on a PC must yield a well-formed TQ8, and the
// file can be hand-uploaded at lotw.arrl.org/lotw/upload.
static esp_err_t lotw_tq8_handler(httpd_req_t *req)
{
    int consumed = 0, nsigned = 0;
    size_t gz_len = 0;
    char err[120] = "";
    uint8_t *gz = lotw_build_tq8_gz(0, adif_log_count(),
                                    &consumed, &nsigned, &gz_len,
                                    err, sizeof err);
    if (!gz) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err[0] ? err : "no signable QSOs");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"qmx_panadapter.tq8\"");
    esp_err_t rc = httpd_resp_send(req, (const char *)gz, (ssize_t)gz_len);
    free(gz);
    return rc;
}

// GET /forge.min.js — embedded pre-gzipped node-forge for the p12 import.
static esp_err_t forge_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)forge_js_gz_start,
                           forge_js_gz_end - forge_js_gz_start);
}

static const httpd_uri_t uri_lotw_cert = {
    .uri = "/api/lotw_cert", .method = HTTP_POST, .handler = lotw_cert_handler,
};
static const httpd_uri_t uri_lotw_upload = {
    .uri = "/api/lotw_upload", .method = HTTP_POST, .handler = lotw_upload_handler,
};
static const httpd_uri_t uri_lotw_tq8 = {
    .uri = "/api/lotw_tq8", .method = HTTP_GET, .handler = lotw_tq8_handler,
};
static const httpd_uri_t uri_forge_js = {
    .uri = "/forge.min.js", .method = HTTP_GET, .handler = forge_js_handler,
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
    if (!s_last_upload.busy && s_last_upload.kind != UPLOAD_NONE) {
        cJSON_AddStringToObject(root, "kind",
            s_last_upload.kind == UPLOAD_QRZ  ? "qrz" :
            s_last_upload.kind == UPLOAD_LOTW ? "lotw" : "eqsl");
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

// GET /api/signal — just the S-meter peak dBm around the VFO, nothing else.
// Deliberately tiny so the web UI can poll it several times a second to make
// its S-meter track like the Tab5's (which samples at 5 Hz), instead of the
// GET /api/tone  - the TX audio tone, and the live occupancy of the 200-2800 Hz
//                  window that the Tab5's picker draws.
// POST /api/tone - {"hz":1650} and/or {"hold":true}
//
// The occupancy mask is the SAME one the automatic clear-slot picker uses
// (ft8_tx_get_tone_occupancy), not a second opinion assembled for display - so
// what the browser shows as busy is exactly what the device will refuse to
// transmit on.
//
// n_stations matters: a mask of all-clear with ZERO stations heard means
// "nothing decoded yet", not "the band is empty". The browser says so rather
// than painting a reassuring row of green.
static esp_err_t tone_get_handler(httpd_req_t *req)
{
    int n_slots = 0, n_stations = 0;
    uint64_t mask_e = 0, mask_o = 0;
    ft8_tx_get_tone_occupancy_split(&mask_e, &mask_o, &n_slots, &n_stations);
    // The single-window view the verdict runs on, same rule as the device:
    // our window when knowable, the union otherwise.
    bool we = false;
    uint64_t mask = ft8_tx_get_parity_lock(&we) ? (we ? mask_e : mask_o)
                                                : (mask_e | mask_o);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddNumberToObject(root, "hz",        ft8_tx_get_tone_pref_hz());
    cJSON_AddBoolToObject  (root, "hold",      ft8_tx_get_tone_hold());
    cJSON_AddNumberToObject(root, "min",       FT8_TX_TONE_MIN_HZ);
    cJSON_AddNumberToObject(root, "max",       FT8_TX_TONE_MAX_HZ);
    cJSON_AddNumberToObject(root, "step",      FT8_TX_TONE_STEP_HZ);
    cJSON_AddNumberToObject(root, "stations",  n_stations);
    // One char per 50 Hz slot: '1' busy, '0' free. A string, not a 64-bit number,
    // because JavaScript cannot hold 52 bits of integer without losing the low
    // ones to the double it would become.
    char bits[72];
    int n = n_slots > 0 && n_slots < (int)sizeof(bits) ? n_slots : 0;
    for (int i = 0; i < n; i++) bits[i] = (mask >> i) & 1ULL ? '1' : '0';
    bits[n] = '\0';
    cJSON_AddStringToObject(root, "busy", bits);
    // Both windows separately (Roy KI0ER): the browser draws an EVEN and an ODD
    // row, so the operator can choose window AND tone before arming anything.
    for (int i = 0; i < n; i++) bits[i] = (mask_e >> i) & 1ULL ? '1' : '0';
    cJSON_AddStringToObject(root, "busy_e", bits);
    for (int i = 0; i < n; i++) bits[i] = (mask_o >> i) & 1ULL ? '1' : '0';
    cJSON_AddStringToObject(root, "busy_o", bits);

    // The partner's tone, so the browser can mark it the way the Tab5 does -
    // you avoid it, but it is not "busy" in the same sense as a stranger.
    int partner = 0;
    if (ft8_qso_get_priority_freq(&partner) && partner > 0)
        cJSON_AddNumberToObject(root, "partner_hz", partner);

    // Whether a change would be accepted right now. A burst mid-flight refuses,
    // and saying so up front beats offering a control that will fail.
    cJSON_AddBoolToObject(root, "busy_tx", ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static esp_err_t tone_post_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    cJSON *jhz = cJSON_GetObjectItem(root, "hz");
    cJSON *jhold = cJSON_GetObjectItem(root, "hold");

    // Same ORDER as the Tab5's Apply (ft8_tone_modal.c): move a running QSO
    // FIRST, because that is the only step that can be refused, and storing the
    // preference before a refusal would leave the two half-committed.
    if (cJSON_IsNumber(jhz)) {
        int hz = (int)jhz->valuedouble;
        if (hz < FT8_TX_TONE_MIN_HZ) hz = FT8_TX_TONE_MIN_HZ;
        if (hz > FT8_TX_TONE_MAX_HZ) hz = FT8_TX_TONE_MAX_HZ;
        if (ft8_qso_get_state() != FT8_QSO_IDLE) {
            char err[64] = {0};
            if (!ft8_qso_set_tx_tone_hz(hz, err, sizeof(err))) {
                cJSON_Delete(root);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, "409 Conflict");
                char body[128];
                snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err[0] ? err : "refused");
                return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
            }
        }
        ft8_tx_set_tone_pref_hz(hz);
    }
    if (cJSON_IsBool(jhold)) ft8_tx_set_tone_hold(cJSON_IsTrue(jhold));

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/memory  - all 32 memory channels.
// POST /api/memory - write one: {"idx":n,"freq_hz":..,"mode":"USB","label":".."}
//                    or clear one: {"idx":n,"clear":true}
//
// Recalling a channel deliberately needs no endpoint: the browser already has
// set_freq and set_mode, and recall IS those two things. Adding a "recall" action
// would put a second copy of that decision on the device for no gain.
static esp_err_t memory_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON *arr = cJSON_AddArrayToObject(root, "slots");
    for (int i = 0; i < MEM_SLOTS; i++) {
        mem_slot_t s;
        if (!mem_channels_get(i, &s)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "idx", i);
        cJSON_AddBoolToObject  (o, "occupied", s.occupied != 0);
        if (s.occupied) {
            cJSON_AddNumberToObject(o, "freq_hz", (double)s.freq_hz);
            cJSON_AddStringToObject(o, "mode",  s.mode);
            cJSON_AddStringToObject(o, "label", s.label);
        }
        cJSON_AddItemToArray(arr, o);
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

static esp_err_t memory_post_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    cJSON *ji = cJSON_GetObjectItem(root, "idx");
    if (!cJSON_IsNumber(ji) || ji->valuedouble < 0 || ji->valuedouble >= MEM_SLOTS) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "idx out of range");
        return ESP_FAIL;
    }
    int idx = (int)ji->valuedouble;

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "clear"))) {
        mem_channels_clear(idx);
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

    // Start from what is already stored, so a partial edit (renaming a channel)
    // does not blank the fields it did not mention.
    mem_slot_t s;
    if (!mem_channels_get(idx, &s)) memset(&s, 0, sizeof(s));

    cJSON *jf = cJSON_GetObjectItem(root, "freq_hz");
    if (cJSON_IsNumber(jf)) s.freq_hz = (uint32_t)jf->valuedouble;
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
    if (m) snprintf(s.mode, sizeof(s.mode), "%s", m);
    const char *l = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
    if (l) snprintf(s.label, sizeof(s.label), "%s", l);

    // The Tab5 refuses to store an out-of-band memory (ui_validate_band_freq_hz);
    // the browser must not be a way around that - a channel that cannot legally
    // be tuned is worse than no channel.
    if (!s.freq_hz || !ui_validate_band_freq_hz(s.freq_hz, NULL, NULL)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "frequency is not inside an amateur band");
        return ESP_FAIL;
    }
    s.occupied = 1;
    mem_channels_set(idx, &s);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/settings - the settings a browser can usefully edit.
// POST /api/settings - apply a PARTIAL update; only the keys present are touched.
//
// Why a settings pair and not more per-setting actions: this is the drawer's
// content, and it grows. One endpoint that merges whatever it is given keeps the
// browser free to send a single field (a toggle) or a whole form, and keeps the
// device free of a dozen near-identical handlers.
//
// Deliberately NOT everything in the drawer. Callsign, grid, the CQ presets and
// the FT8 filter terms are here because they are TEXT, and typing them on glass
// is the worst part of setting the radio up. WiFi credentials are here too, so a
// second network can be added from a laptop - but the password is never sent
// BACK (see below). Left out on purpose: anything that is a live view control
// (zoom, pan, flat mode), which belongs to whichever screen you are looking at.
//
// Every setter below is the same one the drawer calls, so a value changed here is
// the value the Tab5 shows when its drawer is next opened.
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    qmx_settings_t c;
    settings_load_all(&c);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddStringToObject(root, "my_callsign", c.my_callsign);
    cJSON_AddStringToObject(root, "my_grid",     c.my_grid);

    cJSON *cq = cJSON_AddObjectToObject(root, "cq");
    cJSON *msgs = cJSON_AddArrayToObject(cq, "msg");
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(msgs, cJSON_CreateString(c.cq_msg[i]));
    cJSON_AddNumberToObject(cq, "sel",        c.cq_sel);
    cJSON_AddNumberToObject(cq, "max_calls",  c.cq_max_calls);
    cJSON_AddNumberToObject(cq, "listen_every", c.cq_listen_every);

    cJSON *f = cJSON_AddObjectToObject(root, "filters");
    cJSON *fi = cJSON_AddArrayToObject(f, "incl");
    cJSON *fx = cJSON_AddArrayToObject(f, "excl");
    for (int i = 0; i < 2; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "en", c.ft8_filters.incl_en[i]);
        cJSON_AddStringToObject(o, "text", c.ft8_filters.incl_text[i]);
        cJSON_AddItemToArray(fi, o);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "en", c.ft8_filters.excl_en[i]);
        cJSON_AddStringToObject(e, "text", c.ft8_filters.excl_text[i]);
        cJSON_AddItemToArray(fx, e);
    }
    cJSON_AddBoolToObject(f, "excl_worked_before", c.ft8_filters.excl_worked_before);
    cJSON_AddBoolToObject(f, "excl_plain_cq",      c.ft8_filters.excl_plain_cq);
    cJSON_AddBoolToObject(f, "incl_cq_only",       c.ft8_filters.incl_cq_only);
    cJSON_AddBoolToObject(f, "skip_tx1",           c.ft8_filters.skip_tx1);
    cJSON_AddBoolToObject(f, "auto_pileup",        c.ft8_filters.auto_pileup);
    cJSON_AddBoolToObject(f, "robot_en",           c.ft8_filters.robot_en);
    cJSON_AddNumberToObject(f, "robot_priority",   c.ft8_filters.robot_priority);

    cJSON_AddBoolToObject(root, "spots_en",          c.spots_en);
    cJSON_AddBoolToObject(root, "rbn_en",            c.rbn_en);
    cJSON_AddBoolToObject(root, "cluster_en",        c.cluster_en);
    cJSON_AddBoolToObject(root, "sota_en",           c.sota_en);
    cJSON_AddBoolToObject(root, "spots_mode_filter", c.spots_mode_filter);
    // The last of the drawer's controls that had no remote equivalent. CW pitch and
    // the IF trim are per-unit calibration you set once and forget, which is
    // exactly the kind of thing you do not want to need the glass for; the charge
    // limit matters most when the Tab5 is somewhere you are not.
    cJSON_AddNumberToObject(root, "cw_pitch_hz",     (double)ui_get_cw_pitch_hz());
    cJSON_AddNumberToObject(root, "if_cal_hz",       (double)ui_get_if_cal_hz());
    cJSON_AddBoolToObject  (root, "charge_limit_en",  c.charge_limit_en);
    cJSON_AddNumberToObject(root, "charge_limit_pct", (double)c.charge_limit_pct);
    cJSON_AddBoolToObject(root, "psk_rx_en",         c.psk_rx_en);
    cJSON_AddBoolToObject(root, "bt_mouse_en",       c.bt_mouse_en);
    cJSON_AddNumberToObject(root, "swr_limit_x10",   c.swr_limit_x10);
    cJSON_AddBoolToObject(root, "pskreporter_en",    c.pskreporter_en);
    cJSON_AddBoolToObject(root, "greylist_en",       c.greylist_en);
    cJSON_AddNumberToObject(root, "hound_mode",      c.hound_mode);
    cJSON_AddBoolToObject(root, "sim_mode_en",       c.sim_mode_en);
    cJSON_AddBoolToObject(root, "distance_in_miles", c.distance_in_miles);
    cJSON_AddBoolToObject(root, "rit_pill_show",     c.rit_pill_show);
    cJSON_AddNumberToObject(root, "spur_mode",       c.spur_mode);
    cJSON_AddBoolToObject(root, "iq_enabled",        c.iq_enabled);
    cJSON_AddNumberToObject(root, "qmx_vol_db",      c.qmx_vol_db);
    // -1 until the radio has answered RG;. The browser must show that as
    // "unknown" rather than as 0 dB, which is a real and very deaf setting.
    cJSON_AddNumberToObject(root, "cw_tx_offset_hz", c.cw_tx_offset_hz);
    cJSON_AddNumberToObject(root, "qmx_rf_gain_db",  cat_get_rf_gain());
    cat_query_rf_gain();   // refresh for the next GET, as the drawer does on open
    cJSON_AddNumberToObject(root, "bandplan_region", c.bandplan_region);

    // Display & waterfall - the "tune it from the laptop while watching the
    // Tab5" group. Flip-180 is deliberately absent: you set that standing at
    // the device, because it depends on how it is physically mounted.
    cJSON *d = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(d, "wf_black_db",    c.wf_black_db);
    cJSON_AddNumberToObject(d, "wf_contrast_db", c.wf_contrast_db);
    cJSON_AddNumberToObject(d, "wf_floor_blend", c.wf_floor_blend);
    cJSON_AddNumberToObject(d, "wf_window",      c.wf_window);
    cJSON_AddNumberToObject(d, "colormap",       c.colormap_idx);
    cJSON_AddNumberToObject(d, "brightness",     c.brightness_pct);
    cJSON_AddNumberToObject(d, "sleep_min",      c.display_sleep_min);
    cJSON_AddNumberToObject(d, "db_min",         c.db_min);
    cJSON_AddNumberToObject(d, "db_max",         c.db_max);
    cJSON_AddNumberToObject(d, "ema_pct",        (int)(c.ema_alpha * 100.0f + 0.5f));
    cJSON_AddBoolToObject  (d, "flip",           c.display_flip);

    // SSID yes, password never: it would put the operator's WiFi key in every
    // browser cache and proxy log that ever touched this page. The form sends a
    // password only when the operator types a new one.
    cJSON_AddStringToObject(root, "wifi_ssid", c.wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_pass_set", c.wifi_pass[0] != '\0');

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

// Current stored dB range, for a partial db_min/db_max update: the half not
// supplied keeps its stored value rather than an invented default.
static float c_cur_db_min(void) { qmx_settings_t c; settings_load_all(&c); return c.db_min; }
static float c_cur_db_max(void) { qmx_settings_t c; settings_load_all(&c); return c.db_max; }

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    // ⚠ READ THE WHOLE BODY. This was a fixed char buf[1024] and a SINGLE
    // httpd_req_recv() with no loop, so it worked only for as long as the settings
    // form stayed under 1 KB and every byte happened to arrive in one read. Adding
    // one checkbox took the form past 1024 bytes: the body arrived truncated, the
    // JSON failed to parse, and the browser got "Save failed (HTTP 400)" with no clue
    // why. The form only ever grows, so size it from content_len and loop - the same
    // discipline /api/config already uses.
    //
    // Buffer comes from PSRAM: this is several KB on the httpd task's stack
    // otherwise, and a >16 KB plain malloc would land in scarce internal RAM
    // (CLAUDE.md, SPIRAM_MALLOC_ALWAYSINTERNAL).
    int total = req->content_len;
    if (total <= 0)      { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    if (total > 65536)   { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large"); return ESP_FAIL; }

    char *buf = heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, (size_t)(total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;      // retry, do not give up
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body"); return ESP_FAIL; }
        got += r;
    }
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);            // cJSON_Parse copies what it keeps, so this is safe here
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    // MERGE, never replace: a browser sending one toggle must not reset the rest
    // to whatever its (possibly stale) form last read. Same principle as the
    // config-file import.
    const char *s;
    cJSON *it;

    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "my_callsign")))) settings_set_my_callsign(s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "my_grid"))))     settings_set_my_grid(s);

    cJSON *cq = cJSON_GetObjectItem(root, "cq");
    if (cJSON_IsObject(cq)) {
        cJSON *msgs = cJSON_GetObjectItem(cq, "msg");
        if (cJSON_IsArray(msgs)) {
            int n = cJSON_GetArraySize(msgs);
            for (int i = 0; i < n && i < 3; i++) {
                const char *m = cJSON_GetStringValue(cJSON_GetArrayItem(msgs, i));
                if (m) settings_set_cq_msg((uint8_t)i, m);
            }
        }
        if (cJSON_IsNumber(it = cJSON_GetObjectItem(cq, "sel")))
            settings_set_cq_sel((uint8_t)it->valuedouble);
        if (cJSON_IsNumber(it = cJSON_GetObjectItem(cq, "max_calls")))
            settings_set_cq_max_calls((uint8_t)it->valuedouble);
        if (cJSON_IsNumber(it = cJSON_GetObjectItem(cq, "listen_every")))
            settings_set_cq_listen_every((uint8_t)it->valuedouble);
    }

    // Filters are one NVS blob, so read-modify-write the whole struct.
    cJSON *f = cJSON_GetObjectItem(root, "filters");
    if (cJSON_IsObject(f)) {
        qmx_settings_t cur;
        settings_load_all(&cur);
        ft8_filters_t nf = cur.ft8_filters;
        const char *keys[2] = { "incl", "excl" };
        for (int k = 0; k < 2; k++) {
            cJSON *arr = cJSON_GetObjectItem(f, keys[k]);
            if (!cJSON_IsArray(arr)) continue;
            for (int i = 0; i < 2 && i < cJSON_GetArraySize(arr); i++) {
                cJSON *o = cJSON_GetArrayItem(arr, i);
                if (!cJSON_IsObject(o)) continue;
                bool *en  = k ? &nf.excl_en[i]   : &nf.incl_en[i];
                char *txt = k ? nf.excl_text[i]  : nf.incl_text[i];
                cJSON *e = cJSON_GetObjectItem(o, "en");
                if (cJSON_IsBool(e)) *en = cJSON_IsTrue(e);
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(o, "text"));
                if (t) snprintf(txt, FT8_FILTER_TEXT_LEN, "%s", t);
            }
        }
        #define BOOLF(name, field) do { cJSON *b = cJSON_GetObjectItem(f, name); \
            if (cJSON_IsBool(b)) nf.field = cJSON_IsTrue(b); } while (0)
        BOOLF("excl_worked_before", excl_worked_before);
        BOOLF("excl_plain_cq",      excl_plain_cq);
        BOOLF("incl_cq_only",       incl_cq_only);
        BOOLF("skip_tx1",           skip_tx1);
        BOOLF("auto_pileup",        auto_pileup);
        BOOLF("robot_en",           robot_en);
        #undef BOOLF
        if (cJSON_IsNumber(it = cJSON_GetObjectItem(f, "robot_priority")))
            nf.robot_priority = (uint8_t)it->valuedouble;
        settings_set_ft8_filters(&nf);
    }

    #define BOOLTOP(name, setter) do { cJSON *b = cJSON_GetObjectItem(root, name); \
        if (cJSON_IsBool(b)) setter(cJSON_IsTrue(b)); } while (0)
    BOOLTOP("spots_en",          settings_set_spots_en);
    BOOLTOP("rbn_en",            settings_set_rbn_en);
    BOOLTOP("cluster_en",        settings_set_cluster_en);
    BOOLTOP("sota_en",           settings_set_sota_en);
    BOOLTOP("spots_mode_filter", settings_set_spots_mode_filter);
    BOOLTOP("psk_rx_en",         settings_set_psk_rx_en);
    BOOLTOP("bt_mouse_en",       settings_set_bt_mouse_en);
    BOOLTOP("pskreporter_en",    settings_set_pskreporter_en);
    BOOLTOP("greylist_en",       settings_set_greylist_en);
    // Fox/Hound: 0 off, 1 guided, 2 automatic. A number rather than a bool
    // because it is a ladder, not a switch - see ft8_hound.h.
    {
        cJSON *hm = cJSON_GetObjectItem(root, "hound_mode");
        if (cJSON_IsNumber(hm)) settings_set_hound_mode((uint8_t)hm->valuedouble);
    }
    // Simulation mode over the web. Added so the Fox/Hound work could be tested
    // with the radio switched off and nobody at the Tab5, and it earns its keep
    // generally: this is the one setting that makes TX SAFER (ft8_tx.c's interlock
    // sends not one CAT byte while it is on), so remote practice needs no trust.
    BOOLTOP("sim_mode_en",       settings_set_sim_mode_en);
    BOOLTOP("distance_in_miles", settings_set_distance_in_miles);
    // Mirror it into the live UI too, or the pill only changes on the next boot.
    if (cJSON_IsBool(it = cJSON_GetObjectItem(root, "rit_pill_show"))) {
        bool v = cJSON_IsTrue(it);
        settings_set_rit_pill_show(v);
        ui_set_rit_pill_show(v);
    }
    // Spur suppression, like IQ balance below, is a live DSP path as well as a
    // stored value - set both or the control does nothing until the next boot.
    // 0=off 1=subtract 2=interpolate.
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "spur_mode"))) {
        int v = it->valueint;
        if (v < 0 || v > 2) v = 0;
        settings_set_spur_mode((uint8_t)v);
        spur_map_set_mode((spur_mode_t)v);
    }
    #undef BOOLTOP

    // IQ balance is a live DSP path as well as a stored flag - set both, exactly
    // as the drawer switch does, or the setting and the behaviour disagree until
    // the next boot.
    cJSON *iq = cJSON_GetObjectItem(root, "iq_enabled");
    if (cJSON_IsBool(iq)) { iq_balance_set_enabled(cJSON_IsTrue(iq)); settings_set_iq_enabled(cJSON_IsTrue(iq)); }

    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "qmx_vol_db"))) {
        uint8_t db = (uint8_t)it->valuedouble;
        if (db > CAT_AF_GAIN_DB_MAX) db = CAT_AF_GAIN_DB_MAX;
        settings_set_qmx_vol_db(db);
        // AG is in 0.25 dB steps - the slider's own conversion. Sending dB here
        // would set the radio to a quarter of what the browser asked for.
        cat_request_af_gain((uint16_t)(db * 4));
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "cw_tx_offset_hz")))
        settings_set_cw_tx_offset_hz((int16_t)it->valuedouble);   // clamps internally
    // RF gain is NOT stored on our side at all - it lives in the radio, per
    // band, and we only ever relay it. Storing a copy would let the browser
    // replay another band's value onto this one.
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "qmx_rf_gain_db"))) {
        int db = (int)it->valuedouble;
        if (db < 0) db = 0;
        if (db > CAT_RF_GAIN_DB_MAX) db = CAT_RF_GAIN_DB_MAX;
        cat_request_rf_gain((uint8_t)db);
        ui_flat_mode_reset();   // the noise floor just moved
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "bandplan_region")))
        settings_set_bandplan_region((uint8_t)it->valuedouble);

    // CW pitch and the IF trim: live setter AND NVS, same as the drawer's sliders -
    // ui_set_* do both, and ui_set_cw_pitch_hz also pushes the QMX's own CW centre.
    // Both clamp internally, so a hand-written value cannot put the display out of
    // step with the radio.
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "cw_pitch_hz"))) {
        ui_set_cw_pitch_hz((uint16_t)it->valuedouble);
        ui_flat_mode_reset();          // the CW carrier just moved in the passband
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "if_cal_hz")))
        ui_set_cw_cal_hz((int16_t)it->valuedouble);

    // Battery care. status.c re-reads the config on every poll, so storing it is
    // enough - there is nothing to apply live.
    { cJSON *b = cJSON_GetObjectItem(root, "charge_limit_en");
      if (cJSON_IsBool(b)) settings_set_charge_limit_en(cJSON_IsTrue(b)); }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "charge_limit_pct")))
        settings_set_charge_limit_pct((uint8_t)it->valuedouble);   // clamps 50..100

    // SWR protection limit. This was in the GET and in the browser's form but had
    // no case here, so the field looked editable, looked saved, and was dropped -
    // the worst possible failure for a control whose whole job is to stop a
    // transmit into a bad antenna. Clamped rather than trusted: 0 is off, and
    // anything else is held inside the range the Tab5's own dropdown offers, so a
    // hand-written value cannot disable protection by being absurd.
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "swr_limit_x10"))) {
        int v = (int)it->valuedouble;
        if (v != 0) {
            if (v < 20) v = 20;
            if (v > 40) v = 40;
        }
        settings_set_swr_limit_x10((uint8_t)v);
        ESP_LOGI(TAG, "web: SWR protection -> %d.%d:1", v / 10, v % 10);
    }

    // Display & waterfall: every write does what the drawer's own control does -
    // the LIVE call and the NVS setter together, or the screen and the stored
    // value disagree until the next boot.
    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (cJSON_IsObject(disp)) {
        cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "wf_black_db"))) {
            render_waterfall_set_black_level((float)v->valuedouble);
            settings_set_wf_black_db((float)v->valuedouble);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "wf_contrast_db"))) {
            render_waterfall_set_contrast_db((float)v->valuedouble);
            settings_set_wf_contrast_db((float)v->valuedouble);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "wf_floor_blend"))) {
            int pct = (int)v->valuedouble;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            render_waterfall_set_floor_blend((float)pct / 100.0f);
            settings_set_wf_floor_blend((uint8_t)pct);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "wf_window"))) {
            uint8_t idx = (uint8_t)v->valuedouble; if (idx > 2) idx = 0;
            dsp_set_window(idx);
            settings_set_wf_window(idx);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "colormap"))) {
            uint8_t idx = (uint8_t)v->valuedouble; if (idx > 3) idx = 0;
            render_waterfall_set_colormap(idx);
            settings_set_colormap_idx(idx);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "brightness"))) {
            int pct = (int)v->valuedouble;
            // Floor at 10: a remote hand setting 0 turns the screen off with the
            // operator maybe not at the device - the drawer's slider has the
            // same floor for the same reason.
            if (pct < 10) pct = 10;
            if (pct > 100) pct = 100;
            display_set_brightness(pct);
            settings_set_brightness_pct((uint8_t)pct);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "sleep_min")))
            settings_set_display_sleep_min((uint8_t)v->valuedouble);
        // 180-degree flip, for a Tab5 mounted upside down. Live + stored, like the
        // drawer's own toggle: LVGL rotation and the touch map follow together, so
        // it is reversible from either screen.
        if (cJSON_IsBool(v = cJSON_GetObjectItem(disp, "flip"))) {
            bool fl = cJSON_IsTrue(v);
            display_set_flipped(fl);
            settings_set_display_flip(fl);
            ESP_LOGI(TAG, "web: display flip %s", fl ? "on" : "off");
        }
        cJSON *jmin = cJSON_GetObjectItem(disp, "db_min");
        cJSON *jmax = cJSON_GetObjectItem(disp, "db_max");
        if (cJSON_IsNumber(jmin) || cJSON_IsNumber(jmax)) {
            float mn = cJSON_IsNumber(jmin) ? (float)jmin->valuedouble : c_cur_db_min();
            float mx = cJSON_IsNumber(jmax) ? (float)jmax->valuedouble : c_cur_db_max();
            if (mx > mn + 10.0f) {          // a 10 dB floor keeps the scale sane
                ui_set_db_range(mn, mx);
                settings_set_db_min(mn);
                settings_set_db_max(mx);
            }
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(disp, "ema_pct"))) {
            float a = (float)v->valuedouble / 100.0f;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            settings_set_ema_alpha(a);      // the render task reads it live
        }
    }

    // WiFi: SSID and password together, and only when a password is actually
    // supplied - the GET never returns one, so an empty field means "unchanged",
    // not "erase it". This is how a second network gets added from a laptop.
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "wifi_ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "wifi_pass"));
    if (ssid && ssid[0] && pass && pass[0]) panadapter_wifi_update_credentials(ssid, pass);

    cJSON_Delete(root);
    settings_flush();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/decodes - the FT8/FT4 decode list, as the Tab5 shows it.
//
// The browser has shown nothing about who is on frequency since FT8 landed: in
// FT8 mode it pauses the spectrum and offers only the TX banner. An operator in
// another room could see that their radio was transmitting but not who was
// answering.
//
// Ordering comes from ft8_screen_sort_rows() - the SAME function the Tab5's own
// list calls - so the two cannot disagree about which station is on top. The
// display filters below are the ones from the Tab5's Filter window, applied here
// too, because a browser list that quietly ignored them would be a different
// list wearing the same name.
//
// Read-only. Nothing here can key the radio.
// Propagation feedback: who has heard US lately. A GET here also nudges a
// refresh, but psk_rx enforces the collector's own 5-minute floor, so a
// browser polling this cannot turn into abuse of a free service.
static esp_err_t psk_rx_handler(httpd_req_t *req)
{
    psk_rx_request_refresh();

    psk_rx_report_t *rep = heap_caps_malloc(sizeof(psk_rx_report_t) * PSK_RX_MAX,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rep) return httpd_resp_send_500(req);
    int n = psk_rx_get(rep, PSK_RX_MAX);

    cJSON *root = cJSON_CreateObject();
    if (!root) { heap_caps_free(rep); return httpd_resp_send_500(req); }
    qmx_settings_t qs;
    settings_load_all(&qs);
    cJSON_AddBoolToObject(root,   "enabled",   qs.psk_rx_en);
    cJSON_AddNumberToObject(root, "age_s",     psk_rx_age_s());
    cJSON_AddNumberToObject(root, "receivers", psk_rx_unique_receivers());
    cJSON_AddNumberToObject(root, "max_km",    psk_rx_max_distance_km());

    cJSON *arr = cJSON_AddArrayToObject(root, "reports");
    for (int i = 0; i < n && arr; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "call", rep[i].rx_call);
        if (rep[i].rx_grid[0]) cJSON_AddStringToObject(o, "grid", rep[i].rx_grid);
        if (rep[i].rx_dxcc[0]) cJSON_AddStringToObject(o, "dxcc", rep[i].rx_dxcc);
        if (rep[i].mode[0])    cJSON_AddStringToObject(o, "mode", rep[i].mode);
        cJSON_AddNumberToObject(o, "f", rep[i].freq_hz);
        cJSON_AddNumberToObject(o, "t", (double)rep[i].heard_unix);
        // Absent SNR is omitted rather than sent as 0 - 0 dB is a real report.
        if (rep[i].snr_db != (int16_t)-32768) cJSON_AddNumberToObject(o, "snr", rep[i].snr_db);
        if (rep[i].distance_km >= 0) {
            cJSON_AddNumberToObject(o, "km",  rep[i].distance_km);
            cJSON_AddNumberToObject(o, "brg", rep[i].bearing_deg);
        }
        cJSON_AddItemToArray(arr, o);
    }
    heap_caps_free(rep);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return e;
}

static esp_err_t decodes_handler(httpd_req_t *req)
{
    // ~11 KB of rows: PSRAM, never the httpd task's stack.
    ft8_call_t *snap = heap_caps_malloc(sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) return httpd_resp_send_500(req);

    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);

    qmx_settings_t qs;
    settings_load_all(&qs);

    char pin[FT8_CALL_MAX_LEN] = {0};
    ft8_qso_get_pinned_call(pin, sizeof(pin));
    char me[FT8_CALL_MAX_LEN] = {0};
    for (size_t i = 0; i < sizeof(me) - 1 && qs.my_callsign[i]; i++)
        me[i] = (char)toupper((unsigned char)qs.my_callsign[i]);

    ft8_screen_sort_rows(snap, n, me, pin);

    // Same hides as the Tab5 list: other stations' CQs during our own CQ run (or
    // whenever "exclude plain CQ" is set), and the include/exclude filter terms.
    // The our-parity hide is deliberately NOT applied here - it exists because a
    // row frozen on screen for minutes is confusing on the Tab5's fixed-height
    // list; the browser shows an age column instead, which answers it honestly.
    bool hide_cq = ft8_qso_cq_filter_active() || qs.ft8_filters.excl_plain_cq;

    // Distance/bearing for the browser's own KM|BRG columns (Tony Abbey asked
    // for the distance the Tab5 already shows). Computed HERE, from the same
    // util/maidenhead.c the Tab5 list uses, rather than in JS from the grid: one
    // implementation means the two screens cannot disagree, and it picks up the
    // miles setting for free. Same shape as /api/psk_rx's km/brg fields.
    double my_lat = 0.0, my_lon = 0.0;
    bool have_me_loc = qs.my_grid[0] && maidenhead_to_latlon(qs.my_grid, &my_lat, &my_lon);

    cJSON *root = cJSON_CreateObject();
    if (!root) { heap_caps_free(snap); return httpd_resp_send_500(req); }
    cJSON_AddStringToObject(root, "mode", ft8_op_mode_get() == FT8_OP_MODE_FT4 ? "FT4" : "FT8");
    // So the browser can label the column MI or KM exactly as the Tab5 does.
    cJSON_AddBoolToObject(root, "miles", qs.distance_in_miles);
    cJSON_AddStringToObject(root, "working", pin);
    cJSON *arr = cJSON_AddArrayToObject(root, "rows");

    int64_t now = (int64_t)time(NULL);
    int slot_ms = ft8_op_mode_slot_ms();
    for (int i = 0; i < n; i++) {
        const ft8_call_t *r = &snap[i];
        if (hide_cq && strncmp(r->last_text, "CQ ", 3) == 0) continue;
        if (qs.ft8_filters.incl_cq_only && strncmp(r->last_text, "CQ ", 3) != 0) continue;
        if (!ft8_filter_match(r->last_text, &qs.ft8_filters)) continue;

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "call", r->call);
        cJSON_AddStringToObject(o, "text", r->last_text);
        cJSON_AddStringToObject(o, "grid", r->last_grid);
        cJSON_AddNumberToObject(o, "snr",  r->last_snr_db);
        cJSON_AddNumberToObject(o, "hz",   r->last_freq);
        cJSON_AddNumberToObject(o, "dt",   r->last_dt_ms);
        cJSON_AddNumberToObject(o, "age",  (double)(now - r->last_utc));
        cJSON_AddNumberToObject(o, "heard", r->heard_count);
        // km/brg omitted entirely when either grid is missing - the browser then
        // shows "--", the same as the Tab5. Never send a distance we cannot
        // stand behind (see the ADIF "never fabricate a value" rule).
        double rlat = 0.0, rlon = 0.0;
        if (have_me_loc && r->last_grid[0]
            && maidenhead_to_latlon(r->last_grid, &rlat, &rlon)) {
            double km = haversine_km(my_lat, my_lon, rlat, rlon);
            cJSON_AddNumberToObject(o, "km",
                (int)((qs.distance_in_miles ? km * 0.621371 : km) + 0.5));
            cJSON_AddNumberToObject(o, "brg",
                (int)(bearing_deg(my_lat, my_lon, rlat, rlon) + 0.5));
        }
        // Which 15 s (or 7.5 s) window it was heard in - the E/O column.
        if (slot_ms > 0) {
            int64_t sidx = (r->last_utc * 1000 + slot_ms / 2) / slot_ms;
            cJSON_AddStringToObject(o, "sl", (sidx % 2) == 0 ? "E" : "O");
        }
        cJSON_AddBoolToObject(o, "me",  me[0] && strstr(r->last_text, me) != NULL);
        cJSON_AddBoolToObject(o, "cq",  strncmp(r->last_text, "CQ ", 3) == 0);
        cJSON_AddBoolToObject(o, "pin", pin[0] && strcmp(r->call, pin) == 0);
        cJSON_AddItemToArray(arr, o);
    }
    heap_caps_free(snap);

    // The pileup: stations calling US while we are busy. Small (12 max) and
    // exactly what a remote operator wants to see between exchanges.
    {
        ft8_pileup_entry_t pe[FT8_PILEUP_MAX];
        int np = ft8_pileup_get_all(pe, FT8_PILEUP_MAX);
        cJSON *parr = cJSON_AddArrayToObject(root, "pileup");
        for (int i = 0; i < np; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "call", pe[i].call);
            cJSON_AddNumberToObject(o, "snr",  pe[i].snr_db);
            cJSON_AddNumberToObject(o, "hz",   pe[i].freq_hz);
            cJSON_AddNumberToObject(o, "age",  (double)(now - pe[i].last_seen_utc));
            cJSON_AddItemToArray(parr, o);
        }
    }

    // The grey-list: who the auto pickers are skipping. The toggle was already
    // in the web settings with no way to see WHO - "why is it ignoring that
    // station?" was undiagnosable from another room.
    {
        char gl[24][12];
        int ng = ft8_greylist_get_all(gl, 24);
        cJSON *garr = cJSON_AddArrayToObject(root, "greylist");
        for (int i = 0; i < ng; i++)
            cJSON_AddItemToArray(garr, cJSON_CreateString(gl[i]));
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

// GET /api/help - the guidance panel's rows, ranked by the device itself.
//
// Deliberately reuses help_triage_collect() and help_topic_get() rather than
// restating any of it in JavaScript: ui/help_topics.c is the ONE place that
// knows which chapter covers what, and the build fails if a page or heading it
// points at has gone (tools/pack_manual.py). A second copy in the web page could
// not be checked that way and would rot silently - which is the exact failure
// the table exists to prevent.
//
// The ranking reflects the TAB5's live state and current screen, which is the
// honest thing for a remote operator: the rows describe what their radio is
// doing, not what their browser is doing.
static esp_err_t help_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    // What the Tab5 would open if its own User Manual button were tapped now.
    const help_entry_t *ctx = help_topic_get(help_topic_for_current_context());
    if (ctx) {
        cJSON *c = cJSON_AddObjectToObject(root, "context");
        cJSON_AddStringToObject(c, "page",   ctx->page);
        cJSON_AddStringToObject(c, "anchor", ctx->anchor);
        cJSON_AddStringToObject(c, "label",  ctx->label);
    }

    help_triage_row_t rows[HELP_TRIAGE_MAX];
    int n = help_triage_collect(rows, HELP_TRIAGE_MAX);
    cJSON *arr = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < n; i++) {
        const help_entry_t *e = help_topic_get(rows[i].topic);
        if (!e) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "symptom", rows[i].symptom);
        cJSON_AddBoolToObject  (o, "flagged", rows[i].flagged);
        cJSON_AddStringToObject(o, "page",    e->page);
        cJSON_AddStringToObject(o, "anchor",  e->anchor);
        cJSON_AddStringToObject(o, "label",   e->label);
        cJSON_AddItemToArray(arr, o);
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

// GET /api/manual?page=guide/ft8-tx.md - one page of the built-in manual, as the
// raw markdown that built the site. "toc.json" is a valid page name, so the
// contents list comes from the same endpoint.
//
// Served from the firmware blob (net/manual_embed.c), so the browser gets help
// with NO internet: a POTA operator on a phone hotspot, or a LAN with no route
// out, still gets the whole manual - and it always matches the running firmware,
// which a link to tab5.lav.dk could not promise.
//
// The blob is memory-mapped rodata: no copy, no allocation, and the send is
// bounded by the page size (the largest is ~30 KB).
static esp_err_t manual_handler(httpd_req_t *req)
{
    char q[128] = {0};
    char raw[96] = {0};
    char page[96] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "page", raw, sizeof(raw));
    if (!raw[0]) snprintf(raw, sizeof(raw), "index.md");

    // URL-DECODE, or every page in a subdirectory 404s. httpd_query_key_value
    // hands back the value still percent-encoded, and the browser sends
    // encodeURIComponent("guide/panadapter.md") = "guide%2Fpanadapter.md", which
    // matches nothing in the blob's table of literal paths. That was 15 of the
    // manual's 19 pages unreachable from the web UI - everything except the four
    // at the top level. It read as "HTTP 404" in the reader and looked like a
    // missing page rather than a missing decode.
    {
        size_t o = 0;
        for (size_t i = 0; raw[i] && o + 1 < sizeof(page); i++) {
            if (raw[i] == '%' && isxdigit((unsigned char)raw[i+1]) &&
                                 isxdigit((unsigned char)raw[i+2])) {
                char hex[3] = { raw[i+1], raw[i+2], 0 };
                page[o++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else if (raw[i] == '+') {
                page[o++] = ' ';
            } else {
                page[o++] = raw[i];
            }
        }
        page[o] = '\0';
    }

    // The blob is a fixed table of known paths, so a lookup miss is the whole
    // guard - there is no filesystem to escape into. Still refuse "..", so a
    // crafted URL never even reaches the lookup looking like a traversal.
    if (strstr(page, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad page");
        return ESP_FAIL;
    }

    const char *data = NULL;
    size_t len = 0;
    if (!manual_embed_get(page, &data, &len) || !data) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such page in the built-in manual");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, strstr(page, ".json") ? "application/json"
                                                   : "text/markdown; charset=utf-8");
    // The manual only changes when the firmware does, so let the browser keep it.
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, data, len);
}

// once-per-second /api/status that undersamples dynamic signals and reads low.
// Same dsp call + params as status_handler and render.c's Tab5 S-meter, so the
// dBm value is identical — only the polling cadence differs.
static esp_err_t signal_handler(httpd_req_t *req)
{
    float peak_dbm = -999.0f;
    int vfo_bin = ((ui_get_if_bin_shift(DSP_FFT_SIZE) % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE;
    char buf[32];
    if (dsp_get_peak_dbm_around_vfo(vfo_bin, 64, &peak_dbm) == ESP_OK)
        snprintf(buf, sizeof(buf), "{\"dbm\":%.1f}", (double)peak_dbm);
    else
        snprintf(buf, sizeof(buf), "{\"dbm\":null}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t uri_signal = {
    .uri = "/api/signal", .method = HTTP_GET, .handler = signal_handler,
};

static const httpd_uri_t uri_tone_get = {
    .uri = "/api/tone", .method = HTTP_GET, .handler = tone_get_handler,
};

static const httpd_uri_t uri_tone_post = {
    .uri = "/api/tone", .method = HTTP_POST, .handler = tone_post_handler,
};

static const httpd_uri_t uri_memory_get = {
    .uri = "/api/memory", .method = HTTP_GET, .handler = memory_get_handler,
};

static const httpd_uri_t uri_memory_post = {
    .uri = "/api/memory", .method = HTTP_POST, .handler = memory_post_handler,
};

static const httpd_uri_t uri_settings_get = {
    .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler,
};

static const httpd_uri_t uri_settings_post = {
    .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler,
};

static const httpd_uri_t uri_decodes = {
    .uri = "/api/decodes", .method = HTTP_GET, .handler = decodes_handler,
};

static const httpd_uri_t uri_psk_rx = {
    .uri = "/api/psk_rx", .method = HTTP_GET, .handler = psk_rx_handler,
};

static const httpd_uri_t uri_help = {
    .uri = "/api/help", .method = HTTP_GET, .handler = help_handler,
};

static const httpd_uri_t uri_manual = {
    .uri = "/api/manual", .method = HTTP_GET, .handler = manual_handler,
};

// ---- QMX terminal (#147) -------------------------------------------------
//
// GET /api/term  -> {"open":bool,"seq":n,"rows":[24 strings],
//                    "rev":[[row,col,len],..],"col":[[row,col,len,fg],..]}
//
// The reverse-video runs are NOT decoration: reverse video is the only thing
// marking the SELECTED menu item, so a client that drops it leaves the operator
// unable to see where they are in the radio's menu. They are sent as runs
// because there are normally one or two per screen - a full 80x24 attribute
// grid would triple the payload for the same information.
static void term_json_row(cJSON *arr, const ansi_term_t *t, int r)
{
    char line[ANSI_COLS + 1];
    ansi_term_row_text(t, r, line);
    int e = ANSI_COLS;
    while (e > 0 && line[e - 1] == ' ') e--;   // trailing blanks carry nothing
    line[e] = '\0';
    cJSON_AddItemToArray(arr, cJSON_CreateString(line));
}

static esp_err_t term_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    const ansi_term_t *t = qmx_term_lock_screen();
    if (!t) {
        cJSON_AddBoolToObject(root, "open", false);
    } else {
        cJSON_AddBoolToObject(root, "open", true);
        cJSON_AddNumberToObject(root, "seq", (double)t->dirty_seq);
        cJSON_AddNumberToObject(root, "cursor", t->cursor_visible ? 1 : 0);
        /* Cursor POSITION, not just visibility. Added 2026-08-17 after I claimed
         * "the left arrow does nothing" in a text field having only checked the
         * visibility flag - which cannot show movement. Whether a key moves the
         * cursor is exactly the question when working out how the radio expects a
         * field to be edited, so the position has to be observable. */
        cJSON_AddNumberToObject(root, "cur_row", t->cur_r);
        cJSON_AddNumberToObject(root, "cur_col", t->cur_c);

        cJSON *rows = cJSON_AddArrayToObject(root, "rows");
        cJSON *rev  = cJSON_AddArrayToObject(root, "rev");
        cJSON *col  = cJSON_AddArrayToObject(root, "col");
        for (int r = 0; r < ANSI_ROWS; r++) {
            if (rows) term_json_row(rows, t, r);
            // Collapse each attribute into runs as we walk the row.
            for (int c = 0; c < ANSI_COLS; ) {
                if (t->cell[r][c].reverse) {
                    int s = c;
                    while (c < ANSI_COLS && t->cell[r][c].reverse) c++;
                    if (rev) {
                        cJSON *run = cJSON_CreateArray();
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(r));
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(s));
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(c - s));
                        cJSON_AddItemToArray(rev, run);
                    }
                } else c++;
            }
            for (int c = 0; c < ANSI_COLS; ) {
                uint8_t fg = t->cell[r][c].fg;
                if (fg) {
                    int s = c;
                    while (c < ANSI_COLS && t->cell[r][c].fg == fg) c++;
                    if (col) {
                        cJSON *run = cJSON_CreateArray();
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(r));
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(s));
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(c - s));
                        cJSON_AddItemToArray(run, cJSON_CreateNumber(fg));
                        cJSON_AddItemToArray(col, run);
                    }
                } else c++;
            }
        }
        qmx_term_unlock_screen();
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return e;
}

// POST /api/term  {"action":"open"|"close"|"key"|"text", "key":"down", "text":"12"}
static esp_err_t term_post_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    const cJSON *ja = cJSON_GetObjectItem(root, "action");
    const char *action = cJSON_IsString(ja) ? ja->valuestring : "";
    bool ok = false;
    const char *err = NULL;

    if (!strcmp(action, "open")) {
        ok = qmx_term_open();
        if (!ok) err = "The radio did not offer a second serial port. Set "
                       "System config > GPS & Ser. ports > USB serial ports to 2, "
                       "then power-cycle the QMX.";
    } else if (!strcmp(action, "close")) {
        qmx_term_close();
        ok = true;
    } else if (!strcmp(action, "key")) {
        const cJSON *jk = cJSON_GetObjectItem(root, "key");
        ok = cJSON_IsString(jk) && qmx_term_key(jk->valuestring);
        if (!ok) err = "no session";
    } else if (!strcmp(action, "text")) {
        // Typing a value into a menu field. One character at a time through the
        // same path as a key, so there is a single place that writes to the port.
        const cJSON *jt = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(jt)) {
            ok = true;
            for (const char *p = jt->valuestring; *p && ok; p++) {
                char one[2] = { *p, 0 };
                ok = qmx_term_key(one);
            }
        }
        if (!ok) err = "no session";
    } else {
        err = "unknown action";
    }
    cJSON_Delete(root);

    char body[256];
    if (ok) snprintf(body, sizeof(body), "{\"ok\":true}");
    else    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err ? err : "failed");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t uri_term_get = {
    .uri = "/api/term", .method = HTTP_GET, .handler = term_get_handler,
};

static const httpd_uri_t uri_term_post = {
    .uri = "/api/term", .method = HTTP_POST, .handler = term_post_handler,
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
        } else if (up.kind == UPLOAD_LOTW) {
            lotw_upload_result_t result;
            lotw_upload_pending(&result);
            xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
            s_last_upload.uploaded = result.uploaded;
            s_last_upload.failed = result.failed;
            strncpy(s_last_upload.error, result.error, sizeof(s_last_upload.error) - 1);
            s_last_upload.error[sizeof(s_last_upload.error) - 1] = '\0';
            s_last_upload.busy = false;
            xSemaphoreGive(s_upload_mutex);
        }
        // Stagger the resume - releasing the SD lock, the WS pause, and the
        // DSP quiet all at once (as this used to) means the SD archive
        // task's pent-up write (it was locked out for the whole upload,
        // typically with a fresh ADIF-dirty flag from the very QSO that
        // triggered the upload) fires at the *exact instant* the WS stream
        // and FFT/FT8 also slam back to full activity - a worse concurrent
        // SDMMC/WiFi burst than steady-state operation, right at the
        // release point. Field-reproduced: WiFi died 4-5s after the upload
        // popup, not during the upload itself, which is exactly the gap
        // between the old simultaneous release and the SD task's next
        // write actually landing. Release SD first and give it a moment to
        // do that write while WS/DSP are still quiet, then resume those.
        if (sd_locked) sd_archive_unlock();
        vTaskDelay(pdMS_TO_TICKS(1500));
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
    config.max_uri_handlers = 41;   // 34 API + WS + 5 file-browser + headroom
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
    httpd_register_uri_handler(s_server, &uri_adif_delete);
    httpd_register_uri_handler(s_server, &uri_qrz_key);
    httpd_register_uri_handler(s_server, &uri_qrz_upload);
    httpd_register_uri_handler(s_server, &uri_upload_status);
    httpd_register_uri_handler(s_server, &uri_signal);
    httpd_register_uri_handler(s_server, &uri_tone_get);
    httpd_register_uri_handler(s_server, &uri_tone_post);
    httpd_register_uri_handler(s_server, &uri_memory_get);
    httpd_register_uri_handler(s_server, &uri_memory_post);
    httpd_register_uri_handler(s_server, &uri_settings_get);
    httpd_register_uri_handler(s_server, &uri_settings_post);
    httpd_register_uri_handler(s_server, &uri_decodes);
    httpd_register_uri_handler(s_server, &uri_psk_rx);
    httpd_register_uri_handler(s_server, &uri_help);
    httpd_register_uri_handler(s_server, &uri_manual);
    httpd_register_uri_handler(s_server, &uri_eqsl_creds);
    httpd_register_uri_handler(s_server, &uri_eqsl_upload);
    httpd_register_uri_handler(s_server, &uri_lotw_cert);
    httpd_register_uri_handler(s_server, &uri_lotw_upload);
    httpd_register_uri_handler(s_server, &uri_lotw_tq8);
    httpd_register_uri_handler(s_server, &uri_forge_js);
    httpd_register_uri_handler(s_server, &uri_config_get);
    httpd_register_uri_handler(s_server, &uri_config_post);
    httpd_register_uri_handler(s_server, &uri_term_get);
    httpd_register_uri_handler(s_server, &uri_term_post);
    filebrowser_register(s_server);   // /files + /api/files + /api/file
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
