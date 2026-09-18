#include "config_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "settings.h"
#include "mem_channels.h"
#include "adif/lotw_upload.h"   // lotw_read/store_cert/key_b64 for full backup

static const char *TAG = "config_io";

/* ⛔ SIZED FOR THE WORST CASE, NOT THE USUAL ONE, and the 2026-09-18 audit is
 * why it had to grow. A fully calibrated radio adds a lot of text: 16 bands x
 * 45 "v:w" points is ~7 KB on its own, on top of ~4 KB of settings, up to ~4 KB
 * of LoTW cert+key base64, 32 memories and 6 remembered networks - about 19 KB,
 * past the old 16384.
 *
 * APP() clamps rather than overruns, so the failure mode was never a crash: it
 * was a config file that simply STOPPED, mid-section, with no indication. A
 * truncated backup that looks complete is the worst shape this file can take,
 * which is why the export now says so in the log as well. */
#define CFG_BUF_BYTES 32768

static const char *yn(bool b) { return b ? "true" : "false"; }

// ---- Export -------------------------------------------------------------
char *config_io_export(size_t *out_len)
{
    qmx_settings_t c;
    settings_load_all(&c);

    // Plain malloc() of 8 KB would be forced into internal RAM (below IDF's
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL threshold), which is the one scarce
    // resource on this device — this buffer is just text, no DMA needed.
    char *buf = heap_caps_malloc(CFG_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    int n = 0;
    int cap = CFG_BUF_BYTES;
    #define APP(...) do { if (n < cap) n += snprintf(buf + n, cap - n, __VA_ARGS__); } while (0)

    APP("# QMX Panadapter config (M5Stack Tab5).\n");
    APP("# Edit values and re-upload, or share a single section (e.g. [memories]).\n");
    APP("# Lines starting with # are ignored; unknown keys are ignored. Upload\n");
    APP("# MERGES: only keys present here change. Most settings apply on restart.\n");
    APP("# NOTE: wifi_pass / qrz_key / eqsl_pass / lotw_key are stored here in clear\n");
    APP("# text - this file is a complete backup; keep it private.\n\n");

    APP("[settings]\n");
    APP("callsign           = %s\n", c.my_callsign);
    APP("grid               = %s\n", c.my_grid);
    APP("wifi_ssid          = %s\n", c.wifi_ssid);
    APP("wifi_pass          = %s\n", c.wifi_pass);
    /* Static IP. Empty wifi_ip means DHCP - and an empty value here restores
       DHCP on import, which is what clearing the field is meant to do. */
    APP("wifi_ip            = %s\n", c.wifi_ip);
    APP("wifi_mask          = %s\n", c.wifi_mask);
    APP("wifi_gw            = %s\n", c.wifi_gw);
    APP("wifi_dns           = %s\n", c.wifi_dns);
    APP("wifi_enabled       = %s\n", yn(c.wifi_enabled));
    APP("cw_pitch_hz        = %u\n", (unsigned)c.cw_pitch_hz);
    APP("if_cal_hz          = %d\n", (int)c.cw_cal_hz);
    APP("iq_balance         = %s\n", yn(c.iq_enabled));
    APP("flat_spectrum      = %s\n", yn(c.flat_mode));
    APP("still_spectrum     = %s\n", yn(c.still_view));
    APP("spots              = %s\n", yn(c.spots_en));
    APP("spots_rbn          = %s\n", yn(c.rbn_en));
    APP("spot_map           = %s\n", yn(c.spotmap_en));
    APP("spots_sota         = %s\n", yn(c.sota_en));
    APP("wspr_enabled       = %s\n", yn(c.wspr_en));   /* the WSPR page master switch - default off */
    APP("zoom               = %.2f\n", (double)c.zoom_factor);
    APP("colormap           = %u\n", (unsigned)c.colormap_idx);
    APP("brightness         = %u\n", (unsigned)c.brightness_pct);
    APP("ema_alpha          = %.2f\n", (double)c.ema_alpha);
    APP("db_min             = %.0f\n", (double)c.db_min);
    APP("db_max             = %.0f\n", (double)c.db_max);
    APP("wf_black_db        = %.0f\n", (double)c.wf_black_db);
    APP("wf_contrast_db     = %.0f\n", (double)c.wf_contrast_db);
    APP("wf_floor_blend     = %u\n", (unsigned)c.wf_floor_blend);
    APP("wf_window          = %u\n", (unsigned)c.wf_window);
    APP("wf_speed_mult      = %u\n", (unsigned)c.wf_speed_mult);
    APP("display_flip       = %s\n", yn(c.display_flip));
    APP("qmx_vol_db         = %u\n", (unsigned)c.qmx_vol_db);
    APP("cw_tx_offset_hz    = %d\n", (int)c.cw_tx_offset_hz);   // 0 = off (CW only)
    APP("swr_limit_x10      = %u\n", (unsigned)c.swr_limit_x10); // 0 = off, else x10 (30 = 3.0:1)
    APP("psk_rx_en          = %d\n", c.psk_rx_en ? 1 : 0);       // propagation feedback (who hears me)
    APP("bt_mouse_en        = %d\n", c.bt_mouse_en ? 1 : 0);     // BLE mouse
    APP("cluster_en         = %d\n", c.cluster_en ? 1 : 0);      // DX cluster feed (phone spots)
    APP("cw_audio_vol       = %u\n", (unsigned)c.cw_audio_vol);
    APP("charge_limit       = %s\n", yn(c.charge_limit_en));
    APP("charge_limit_pct   = %u\n", (unsigned)c.charge_limit_pct);
    APP("display_sleep_min  = %u\n", (unsigned)c.display_sleep_min);
    APP("relay_pin          = %u\n", (unsigned)c.gpio_relay_pin);
    APP("relay_active_high  = %s\n", yn(c.gpio_relay_level));
    APP("relay_ms           = %u\n", (unsigned)c.gpio_relay_ms);
    /* #302: 0 = 14.074.000, 1 = 14,074,000 */
    APP("freq_sep_style     = %u\n", (unsigned)c.freq_sep_style);
    APP("qmx_gps            = %s\n", yn(c.qmx_gps));
    APP("freq_keypad_10key  = %s\n", yn(c.freq_kp_calc));
    APP("onboarded          = %s\n", yn(c.onboarded));
    APP("qrz_key            = %s\n", c.qrz_api_key);
    APP("qrz_lookup_user    = %s\n", c.qrz_lookup_user);
    APP("qrz_lookup_pass    = %s\n", c.qrz_lookup_pass);
    APP("eqsl_user          = %s\n", c.eqsl_user);
    APP("eqsl_pass          = %s\n", c.eqsl_pswd);
    APP("cloudlog_url       = %s\n", c.cloudlog_url);
    APP("cloudlog_key       = %s\n", c.cloudlog_key);
    APP("cloudlog_station   = %s\n", c.cloudlog_station);
    APP("lotw_dxcc          = %s\n", c.lotw_dxcc);
    APP("lotw_cqz           = %s\n", c.lotw_cqz);
    APP("lotw_ituz          = %s\n", c.lotw_ituz);
    APP("lotw_state         = %s\n", c.lotw_state);
    APP("lotw_county        = %s\n", c.lotw_county);
    /* ⛔ EVERYTHING BELOW WAS MISSING FROM THE BACKUP UNTIL 2026-09-18.
     *
     * Found by auditing qmx_settings_t against this file field by field, after
     * Bruce N9JCV lost his WSPR power calibration across an update and asked
     * whether that was intentional. It was not - and the audit turned up 30
     * more settings in the same state, several of them a year old. A setting
     * that is not here is one the operator can only get back by remembering it.
     *
     * ⚠ tx_tone_hz and tx_tone_hold were WORSE than missing: the importer has
     * always accepted them and the exporter never wrote them, so a
     * save-and-restore silently reverted both. Add the two halves in the SAME
     * commit - the import and export key sets must be equal, and a diff of the
     * two is the cheapest test there is. */
    APP("bandplan_region    = %u\n", (unsigned)c.bandplan_region);   // 0=auto 1=R1 2=R2 3=R3
    APP("tune_snap_hz       = %u\n", (unsigned)c.tune_snap_hz);   // 0=off, else 250/500/1000
    APP("rit_pill           = %s\n", yn(c.rit_pill_show));
    APP("spur_mode          = %u\n", (unsigned)c.spur_mode);   // 0=off 1=subtract 2=interpolate
    APP("cw_decode          = %s\n", yn(c.cw_decode_en));
    APP("spots_mode_filter  = %s\n", yn(c.spots_mode_filter));
    APP("drawer_expert      = %s\n", yn(c.drawer_expert));
    APP("distance_in_miles  = %s\n", yn(c.distance_in_miles));
    APP("freq_keypad_small  = %s\n", yn(c.freq_kp_small));
    APP("freq_keypad_dx     = %d\n", (int)c.freq_kp_dx);
    APP("freq_keypad_dy     = %d\n", (int)c.freq_kp_dy);

    APP("ft8_mode           = %u\n", (unsigned)c.ft8_op_mode);   // 0=FT8 1=FT4
    APP("ft8_early_decode   = %s\n", yn(c.ft8_early_decode));
    APP("ft8_greylist       = %s\n", yn(c.greylist_en));
    APP("pskreporter        = %s\n", yn(c.pskreporter_en));
    APP("tx_tone_hz         = %u\n", (unsigned)c.tx_tone_hz);
    APP("tx_tone_hold       = %s\n", yn(c.tx_tone_hold));
    /* Exported because this file is a BACKUP, not a recommendation. Restoring
       it ON means the red bezel is on screen and the radio is not keyed, which
       is visible and safe; dropping it silently would mean a restored unit
       behaves differently from the one that was saved. */
    APP("sim_mode           = %s\n", yn(c.sim_mode_en));
    APP("field_day          = %s\n", yn(c.field_day_en));
    APP("fd_class           = %s\n", c.fd_class);
    APP("fd_section         = %s\n", c.fd_section);
    APP("activation_type    = %u\n", (unsigned)c.act_type);   // 0=none 1=POTA 2=SOTA
    APP("activation_ref     = %s\n", c.act_ref);

    APP("wspr_dial_hz       = %lu\n", (unsigned long)c.wspr_dial_hz);
    APP("wspr_tx            = %s\n", yn(c.wspr_tx_en));
    APP("wspr_tx_dbm        = %d\n", (int)c.wspr_tx_dbm);
    APP("wspr_tx_cycles     = %u\n", (unsigned)c.wspr_tx_cycles);   // 0 = receive only
    APP("wspr_rx_cycles     = %u\n", (unsigned)c.wspr_rx_cycles);
    APP("wspr_band_hop      = %s\n", yn(c.wspr_hop_en));
    APP("wspr_hop_mask      = %u\n", (unsigned)c.wspr_hop_mask);
    APP("wspr_publish       = %s\n", yn(c.wspr_net_en));

    // LoTW callsign cert + private key, single-line base64 DER (full-backup
    // decision: the config file already carries wifi/qrz/eqsl secrets in
    // clear, and this makes a restore complete). Omitted when not imported.
    {
        char *cb = lotw_read_cert_b64();
        char *kb = lotw_read_key_b64();
        if (cb) APP("lotw_cert          = %s\n", cb);
        if (kb) APP("lotw_key           = %s\n", kb);
        free(cb);
        free(kb);
    }

    APP("\n[cq]\n");
    APP("active = %u\n", (unsigned)(c.cq_sel + 1));   // 1-based for the user
    APP("1 = %s\n", c.cq_msg[0]);
    APP("2 = %s\n", c.cq_msg[1]);
    APP("3 = %s\n", c.cq_msg[2]);
    APP("stop_after = %u\n", (unsigned)c.cq_max_calls);   // 0 = keep calling
    APP("hound_mode = %u\n", (unsigned)c.hound_mode);     // 0 off, 1 guided, 2 auto
    APP("listen_every = %u\n", (unsigned)c.cq_listen_every);  // 0 = never pause to listen

    APP("\n[ft8_filters]\n");
    APP("include1_on = %s\n", yn(c.ft8_filters.incl_en[0]));
    APP("include1    = %s\n", c.ft8_filters.incl_text[0]);
    APP("include2_on = %s\n", yn(c.ft8_filters.incl_en[1]));
    APP("include2    = %s\n", c.ft8_filters.incl_text[1]);
    APP("exclude1_on = %s\n", yn(c.ft8_filters.excl_en[0]));
    APP("exclude1    = %s\n", c.ft8_filters.excl_text[0]);
    APP("exclude2_on = %s\n", yn(c.ft8_filters.excl_en[1]));
    APP("exclude2    = %s\n", c.ft8_filters.excl_text[1]);
    APP("exclude_worked_before = %s\n", yn(c.ft8_filters.excl_worked_before));
    APP("exclude_plain_cq      = %s\n", yn(c.ft8_filters.excl_plain_cq));
    APP("only_cq               = %s\n", yn(c.ft8_filters.incl_cq_only));

    /* ⭐ THE POWER CALIBRATION - the reason this whole audit happened.
     *
     * It is MEASURED DATA: an hour at a dummy load, per band. It lived only in
     * NVS, so losing it meant doing the measurement again, and nothing else in
     * this file costs that much to recreate. Bruce N9JCV lost his across an
     * update and there was no way to put it back.
     *
     * One line per band, and only the points actually reached: "v:w" pairs in
     * tenths of a volt and hundredths of a watt - the units the table itself
     * stores, so the file is exact rather than rounded through a decimal.
     *
     * ⛔ Written as a LIST, not a fixed-width row, and that is deliberate.
     * PWRCAL_STEPS has already changed once (23 -> 45, inside v1.14.0), which
     * makes the NVS blob unreadable across that change by design - see the
     * discard in settings_load_all(). A file of v:w pairs survives it, so this
     * export is also the migration path the blob cannot have.
     *
     * ⚠ cal_unix_time is deliberately NOT carried. It records when the
     * measurement was taken on THIS radio, and a restore is not a measurement.
     */
    APP("\n[power_cal]\n");
    APP("# An empty value removes that band. Names are case-insensitive.\n");
    APP("# band = volt_x10:watts_x100, ...   (measured at a dummy load)\n");
    for (int i = 0; i < PWRCAL_MAX_BANDS; i++) {
        const pwr_cal_band_t *r = &c.pwr_cal.bands[i];
        if (!r->band[0]) continue;
        bool any = false;
        for (int k = 0; k < PWRCAL_STEPS; k++) if (r->voltage_x10[k]) { any = true; break; }
        if (!any) continue;
        APP("%s =", r->band);
        bool first = true;
        for (int k = 0; k < PWRCAL_STEPS; k++) {
            if (!r->voltage_x10[k]) continue;
            APP("%s%u:%u", first ? " " : ", ",
                (unsigned)r->voltage_x10[k], (unsigned)r->watts_x100[k]);
            first = false;
        }
        APP("\n");
    }

    /* The operator's own "I want N watts on this band" - a PREFERENCE, kept
       separate from the measurement above on purpose (see the type's comment
       in settings.h: it survives a recalibration or a different radio). */
    APP("\n[power_target]\n");
    APP("# An empty value removes that band. Names are case-insensitive.\n");
    APP("# band = watts   (what you asked for, not what was measured)\n");
    for (int i = 0; i < PWRCAL_MAX_BANDS; i++) {
        const pwr_target_band_t *t = &c.pwr_target.bands[i];
        if (!t->band[0] || !t->target_w_x100) continue;
        APP("%s = %u.%02u\n", t->band,
            (unsigned)(t->target_w_x100 / 100), (unsigned)(t->target_w_x100 % 100));
    }

    APP("\n[memories]\n");
    APP("# slot = freq_hz, mode, label   (mode e.g. USB/LSB/CW/DiGi)\n");
    for (int i = 0; i < MEM_SLOTS; i++) {
        mem_slot_t s;
        if (mem_channels_get(i, &s) && s.occupied) {
            APP("%d = %lu, %s, %s\n", i + 1, (unsigned long)s.freq_hz,
                s.mode[0] ? s.mode : "USB", s.label);
        }
    }
    #undef APP

    if (n >= cap) {
        ESP_LOGE(TAG, "config export TRUNCATED at %d bytes - raise CFG_BUF_BYTES. "
                      "This file is incomplete; do not keep it as a backup.", cap);
        n = cap - 1;
    }
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    ESP_LOGI(TAG, "exported config (%d bytes)", n);
    return buf;
}

// ---- Import -------------------------------------------------------------
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

static bool to_bool(const char *v)
{
    return strcasecmp(v, "true") == 0 || strcasecmp(v, "1") == 0 ||
           strcasecmp(v, "yes") == 0  || strcasecmp(v, "on") == 0;
}

typedef enum { SEC_NONE, SEC_SETTINGS, SEC_CQ, SEC_FILTERS, SEC_MEM, SEC_WIFI_KNOWN,
               SEC_PWR_CAL, SEC_PWR_TARGET } section_t;

int config_io_import(char *text)
{
    if (!text) return 0;

    // ft8_filters are merged into the current value and written once at the end.
    qmx_settings_t cur;
    settings_load_all(&cur);
    ft8_filters_t filt = cur.ft8_filters;
    bool filt_touched = false;

    // Static IP: the four fields are one setting, so they are collected here
    // and applied once at the end. Seeded from the CURRENT values so a file
    // carrying only some of them cannot half-erase the rest.
    char sip[16], smask[16], sgw[16], sdns[16];
    snprintf(sip,   sizeof sip,   "%s", cur.wifi_ip);
    snprintf(smask, sizeof smask, "%s", cur.wifi_mask);
    snprintf(sgw,   sizeof sgw,   "%s", cur.wifi_gw);
    snprintf(sdns,  sizeof sdns,  "%s", cur.wifi_dns);
    bool sip_touched = false;

    // Remembered networks, collected across the [wifi_known] section and applied
    // in one go at the end so the file's order is preserved.
    // STATIC for the same reason as everywhere else this array appears: ~590
    // bytes is too much to put on a task stack on this board. Import runs from a
    // single web request at a time, so a file-local scratch is safe.
    static wifi_known_t known[WIFI_KNOWN_MAX];
    memset(known, 0, sizeof(known));
    int  known_n = 0;
    bool known_touched = false;
    /* Buffered pairs: both of these are set through a single call that takes
       every component at once, so a file carrying only one of them must not
       zero the other. Same shape as the static-IP quartet above. */
    int16_t kp_dx = cur.freq_kp_dx, kp_dy = cur.freq_kp_dy;
    bool    kp_touched = false;
    uint8_t act_type = cur.act_type;
    char    act_ref[16];
    bool    act_touched = false;
    snprintf(act_ref, sizeof act_ref, "%s", cur.act_ref);

    section_t sec = SEC_NONE;
    int applied = 0;

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) *end = '\0';
            char *name = trim(p + 1);
            if      (strcasecmp(name, "settings") == 0)    sec = SEC_SETTINGS;
            else if (strcasecmp(name, "cq") == 0)          sec = SEC_CQ;
            else if (strcasecmp(name, "ft8_filters") == 0) sec = SEC_FILTERS;
            else if (strcasecmp(name, "memories") == 0)    sec = SEC_MEM;
            else if (strcasecmp(name, "wifi_known") == 0)  sec = SEC_WIFI_KNOWN;
            else if (strcasecmp(name, "power_cal") == 0)    sec = SEC_PWR_CAL;
            else if (strcasecmp(name, "power_target") == 0) sec = SEC_PWR_TARGET;
            else sec = SEC_NONE;
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        switch (sec) {
        case SEC_SETTINGS:
            if      (!strcasecmp(key, "callsign"))          settings_set_my_callsign(val);
            else if (!strcasecmp(key, "grid"))              settings_set_my_grid(val);
            else if (!strcasecmp(key, "wifi_ssid"))         settings_set_wifi_ssid(val);
            else if (!strcasecmp(key, "wifi_pass"))         settings_set_wifi_pass(val);
            else if (!strcasecmp(key, "wifi_enabled"))      settings_set_wifi_enabled(to_bool(val));
            /* All four are one setting - collected, applied once at the end. */
            else if (!strcasecmp(key, "wifi_ip"))   { snprintf(sip,   sizeof sip,   "%s", val); sip_touched = true; }
            else if (!strcasecmp(key, "wifi_mask")) { snprintf(smask, sizeof smask, "%s", val); sip_touched = true; }
            else if (!strcasecmp(key, "wifi_gw"))   { snprintf(sgw,   sizeof sgw,   "%s", val); sip_touched = true; }
            else if (!strcasecmp(key, "wifi_dns"))  { snprintf(sdns,  sizeof sdns,  "%s", val); sip_touched = true; }
            else if (!strcasecmp(key, "cw_pitch_hz"))       settings_set_cw_pitch_hz((uint16_t)atoi(val));
            else if (!strcasecmp(key, "if_cal_hz"))         settings_set_cw_cal_hz((int16_t)atoi(val));
            else if (!strcasecmp(key, "iq_balance"))        settings_set_iq_enabled(to_bool(val));
            else if (!strcasecmp(key, "flat_spectrum"))     settings_set_flat_mode(to_bool(val));
            else if (!strcasecmp(key, "still_spectrum"))    { settings_set_still_view(to_bool(val));
                                                              settings_set_still_notice_done(true); }
            else if (!strcasecmp(key, "spots"))             settings_set_spots_en(to_bool(val));
            else if (!strcasecmp(key, "spots_rbn"))         settings_set_rbn_en(to_bool(val));
            else if (!strcasecmp(key, "spot_map"))          settings_set_spotmap_en(to_bool(val));
            else if (!strcasecmp(key, "spots_sota"))        settings_set_sota_en(to_bool(val));
            else if (!strcasecmp(key, "wspr_enabled"))      settings_set_wspr_en(to_bool(val));
            /* ⛔ DEAD KEY, kept only so an old config file still imports without
               a surprise. The quiet auto-download was removed in v1.14.4 and
               settings_set_ota_autodl() now changes nothing that runs. It has
               no APP() on purpose - do NOT "fix" the asymmetry by adding one,
               which would write a removed feature back into every backup. */
            else if (!strcasecmp(key, "ota_autodownload")) settings_set_ota_autodl(to_bool(val));
            else if (!strcasecmp(key, "zoom"))              settings_set_zoom_factor((float)atof(val));
            else if (!strcasecmp(key, "colormap"))          settings_set_colormap_idx((uint8_t)atoi(val));
            else if (!strcasecmp(key, "brightness"))        settings_set_brightness_pct((uint8_t)atoi(val));
            else if (!strcasecmp(key, "ema_alpha"))         settings_set_ema_alpha((float)atof(val));
            else if (!strcasecmp(key, "db_min"))            settings_set_db_min((float)atof(val));
            else if (!strcasecmp(key, "db_max"))            settings_set_db_max((float)atof(val));
            else if (!strcasecmp(key, "wf_black_db"))       settings_set_wf_black_db((float)atof(val));
            else if (!strcasecmp(key, "wf_contrast_db"))    settings_set_wf_contrast_db((float)atof(val));
            else if (!strcasecmp(key, "wf_floor_blend"))    settings_set_wf_floor_blend((uint8_t)atoi(val));
            else if (!strcasecmp(key, "wf_window"))         settings_set_wf_window((uint8_t)atoi(val));
            else if (!strcasecmp(key, "wf_speed_mult"))     settings_set_wf_speed_mult((uint8_t)atoi(val));
            else if (!strcasecmp(key, "display_flip"))      settings_set_display_flip(to_bool(val));
            else if (!strcasecmp(key, "qmx_vol_db"))        settings_set_qmx_vol_db((uint8_t)atoi(val));
            else if (!strcasecmp(key, "cw_tx_offset_hz"))   settings_set_cw_tx_offset_hz((int16_t)atoi(val));
            else if (!strcasecmp(key, "swr_limit_x10"))     settings_set_swr_limit_x10((uint8_t)atoi(val));
            else if (!strcasecmp(key, "psk_rx_en"))         settings_set_psk_rx_en(atoi(val) != 0);
            else if (!strcasecmp(key, "bt_mouse_en"))       settings_set_bt_mouse_en(atoi(val) != 0);
            else if (!strcasecmp(key, "cluster_en"))        settings_set_cluster_en(atoi(val) != 0);
            else if (!strcasecmp(key, "cw_audio_vol"))      settings_set_cw_audio_vol((uint8_t)atoi(val));
            else if (!strcasecmp(key, "charge_limit"))      settings_set_charge_limit_en(to_bool(val));
            else if (!strcasecmp(key, "charge_limit_pct"))  settings_set_charge_limit_pct((uint8_t)atoi(val));
            else if (!strcasecmp(key, "tx_tone_hz"))        settings_set_tx_tone_hz((uint16_t)atoi(val));
            else if (!strcasecmp(key, "tx_tone_hold"))      settings_set_tx_tone_hold(to_bool(val));
            else if (!strcasecmp(key, "display_sleep_min")) settings_set_display_sleep_min((uint8_t)atoi(val));
            // The relay's three fields share one setter (they share a dirty
            // bit), but an INI is read a line at a time and the keys may
            // arrive in any order or singly - so each one re-reads the other
            // two and writes the trio back.
            else if (!strcasecmp(key, "relay_pin") || !strcasecmp(key, "relay_active_high") ||
                     !strcasecmp(key, "relay_ms")) {
                uint8_t rp; bool rl; uint16_t rms;
                settings_get_gpio_relay(&rp, &rl, &rms);
                if (!strcasecmp(key, "relay_pin"))              rp  = (uint8_t)atoi(val);
                else if (!strcasecmp(key, "relay_active_high")) rl  = to_bool(val);
                else                                            rms = (uint16_t)atoi(val);
                settings_set_gpio_relay(rp, rl, rms);
            }
            else if (!strcasecmp(key, "freq_sep_style"))    settings_set_freq_sep_style((uint8_t)atoi(val));
            else if (!strcasecmp(key, "qmx_gps"))           settings_set_qmx_gps(to_bool(val));
            else if (!strcasecmp(key, "freq_keypad_10key")) settings_set_freq_kp_calc(to_bool(val));
            else if (!strcasecmp(key, "onboarded"))         settings_set_onboarded(to_bool(val));
            else if (!strcasecmp(key, "qrz_key"))           settings_set_qrz_api_key(val);
            else if (!strcasecmp(key, "qrz_lookup_user"))   settings_set_qrz_lookup_user(val);
            else if (!strcasecmp(key, "qrz_lookup_pass"))   settings_set_qrz_lookup_pass(val);
            else if (!strcasecmp(key, "eqsl_user"))         settings_set_eqsl_user(val);
            else if (!strcasecmp(key, "eqsl_pass"))         settings_set_eqsl_pswd(val);
            else if (!strcasecmp(key, "cloudlog_url"))      settings_set_cloudlog_url(val);
            else if (!strcasecmp(key, "cloudlog_key"))      settings_set_cloudlog_key(val);
            else if (!strcasecmp(key, "cloudlog_station"))  settings_set_cloudlog_station(val);
            else if (!strcasecmp(key, "lotw_dxcc"))         settings_set_lotw_dxcc(val);
            else if (!strcasecmp(key, "lotw_cqz"))          settings_set_lotw_cqz(val);
            else if (!strcasecmp(key, "lotw_ituz"))         settings_set_lotw_ituz(val);
            else if (!strcasecmp(key, "lotw_state"))        settings_set_lotw_state(val);
            else if (!strcasecmp(key, "lotw_county"))       settings_set_lotw_county(val);
            else if (!strcasecmp(key, "lotw_cert"))         lotw_store_cert_b64(val);
            else if (!strcasecmp(key, "lotw_key"))          lotw_store_key_b64(val);
            /* The 2026-09-18 audit. Every one of these has a matching APP() in
               the export - keep it that way; see the note beside them there. */
            else if (!strcasecmp(key, "bandplan_region"))   settings_set_bandplan_region((uint8_t)atoi(val));
            else if (!strcasecmp(key, "tune_snap_hz"))      settings_set_tune_snap_hz((uint16_t)atoi(val));
            else if (!strcasecmp(key, "rit_pill"))          settings_set_rit_pill_show(to_bool(val));
            else if (!strcasecmp(key, "spur_mode"))         settings_set_spur_mode((uint8_t)atoi(val));
            else if (!strcasecmp(key, "cw_decode"))         settings_set_cw_decode_en(to_bool(val));
            else if (!strcasecmp(key, "spots_mode_filter")) settings_set_spots_mode_filter(to_bool(val));
            else if (!strcasecmp(key, "drawer_expert"))     settings_set_drawer_expert(to_bool(val));
            else if (!strcasecmp(key, "distance_in_miles")) settings_set_distance_in_miles(to_bool(val));
            else if (!strcasecmp(key, "freq_keypad_small")) settings_set_freq_kp_small(to_bool(val));
            /* dx/dy are one call, so the pair is buffered and applied at the
               end - the same shape the static-IP quartet already uses. */
            else if (!strcasecmp(key, "freq_keypad_dx"))  { kp_dx = (int16_t)atoi(val); kp_touched = true; }
            else if (!strcasecmp(key, "freq_keypad_dy"))  { kp_dy = (int16_t)atoi(val); kp_touched = true; }
            else if (!strcasecmp(key, "ft8_mode"))          settings_set_ft8_op_mode((uint8_t)atoi(val));
            else if (!strcasecmp(key, "ft8_early_decode"))  settings_set_ft8_early_decode(to_bool(val));
            else if (!strcasecmp(key, "ft8_greylist"))      settings_set_greylist_en(to_bool(val));
            else if (!strcasecmp(key, "pskreporter"))       settings_set_pskreporter_en(to_bool(val));
            else if (!strcasecmp(key, "sim_mode"))          settings_set_sim_mode_en(to_bool(val));
            else if (!strcasecmp(key, "field_day"))         settings_set_field_day_en(to_bool(val));
            else if (!strcasecmp(key, "fd_class"))          settings_set_fd_class(val);
            else if (!strcasecmp(key, "fd_section"))        settings_set_fd_section(val);
            /* type and ref are one call too, and a type with no ref is
               meaningless - buffered and applied together at the end. */
            else if (!strcasecmp(key, "activation_type"))  { act_type = (uint8_t)atoi(val); act_touched = true; }
            else if (!strcasecmp(key, "activation_ref"))   { snprintf(act_ref, sizeof act_ref, "%s", val); act_touched = true; }
            else if (!strcasecmp(key, "wspr_dial_hz"))      settings_set_wspr_dial_hz((uint32_t)strtoul(val, NULL, 10));
            else if (!strcasecmp(key, "wspr_tx"))           settings_set_wspr_tx_en(to_bool(val));
            else if (!strcasecmp(key, "wspr_tx_dbm"))       settings_set_wspr_tx_dbm((int8_t)atoi(val));
            else if (!strcasecmp(key, "wspr_tx_cycles"))    settings_set_wspr_tx_cycles((uint8_t)atoi(val));
            else if (!strcasecmp(key, "wspr_rx_cycles"))    settings_set_wspr_rx_cycles((uint8_t)atoi(val));
            else if (!strcasecmp(key, "wspr_band_hop"))     settings_set_wspr_hop_en(to_bool(val));
            else if (!strcasecmp(key, "wspr_hop_mask"))     settings_set_wspr_hop_mask((uint16_t)atoi(val));
            else if (!strcasecmp(key, "wspr_publish"))      settings_set_wspr_net_en(to_bool(val));
            else break;   // unknown key: ignore, don't count
            applied++;
            break;

        case SEC_CQ:
            if (!strcasecmp(key, "active")) {
                int s = atoi(val) - 1;          // file is 1-based
                if (s >= 0 && s <= 2) { settings_set_cq_sel((uint8_t)s); applied++; }
            } else if (!strcasecmp(key, "stop_after")) {
                int n = atoi(val);
                if (n >= 0 && n <= 255) { settings_set_cq_max_calls((uint8_t)n); applied++; }
            }
            else if (!strcasecmp(key, "hound_mode")) {
                int n = atoi(val);
                if (n >= 0 && n <= 2) { settings_set_hound_mode((uint8_t)n); applied++; }
            } else if (!strcasecmp(key, "listen_every")) {
                int n = atoi(val);
                if (n >= 0 && n <= 255) { settings_set_cq_listen_every((uint8_t)n); applied++; }
            } else {
                int idx = atoi(key) - 1;        // "1".."3"
                if (idx >= 0 && idx <= 2) { settings_set_cq_msg((uint8_t)idx, val); applied++; }
            }
            break;

        case SEC_FILTERS: {
            bool b = to_bool(val);
            if      (!strcasecmp(key, "include1_on")) { filt.incl_en[0] = b; filt_touched = true; }
            else if (!strcasecmp(key, "include2_on")) { filt.incl_en[1] = b; filt_touched = true; }
            else if (!strcasecmp(key, "exclude1_on")) { filt.excl_en[0] = b; filt_touched = true; }
            else if (!strcasecmp(key, "exclude2_on")) { filt.excl_en[1] = b; filt_touched = true; }
            else if (!strcasecmp(key, "exclude_worked_before")) { filt.excl_worked_before = b; filt_touched = true; }
            else if (!strcasecmp(key, "exclude_plain_cq"))      { filt.excl_plain_cq = b; filt_touched = true; }
            else if (!strcasecmp(key, "only_cq"))               { filt.incl_cq_only = b; filt_touched = true; }
            else if (!strcasecmp(key, "include1")) { strncpy(filt.incl_text[0], val, FT8_FILTER_TEXT_LEN - 1); filt.incl_text[0][FT8_FILTER_TEXT_LEN-1]='\0'; filt_touched = true; }
            else if (!strcasecmp(key, "include2")) { strncpy(filt.incl_text[1], val, FT8_FILTER_TEXT_LEN - 1); filt.incl_text[1][FT8_FILTER_TEXT_LEN-1]='\0'; filt_touched = true; }
            else if (!strcasecmp(key, "exclude1")) { strncpy(filt.excl_text[0], val, FT8_FILTER_TEXT_LEN - 1); filt.excl_text[0][FT8_FILTER_TEXT_LEN-1]='\0'; filt_touched = true; }
            else if (!strcasecmp(key, "exclude2")) { strncpy(filt.excl_text[1], val, FT8_FILTER_TEXT_LEN - 1); filt.excl_text[1][FT8_FILTER_TEXT_LEN-1]='\0'; filt_touched = true; }
            else break;
            applied++;
            break;
        }

        case SEC_MEM: {
            int slot = atoi(key);               // 1-based slot number
            if (slot < 1 || slot > MEM_SLOTS) break;
            // value: "freq_hz, mode, label"
            mem_slot_t s = {0};
            char *c1 = strchr(val, ',');
            if (!c1) break;
            *c1 = '\0';
            char *freq_s = trim(val);
            char *rest = c1 + 1;
            char *c2 = strchr(rest, ',');
            char *mode_s = rest, *label_s = (char *)"";
            if (c2) { *c2 = '\0'; mode_s = trim(rest); label_s = trim(c2 + 1); }
            else    { mode_s = trim(rest); }
            uint32_t hz = (uint32_t)strtoul(freq_s, NULL, 10);
            if (hz == 0) break;                 // skip garbage / treat as no-op
            s.freq_hz  = hz;
            s.occupied = 1;
            strncpy(s.mode,  mode_s[0] ? mode_s : "USB", sizeof(s.mode) - 1);
            strncpy(s.label, label_s, sizeof(s.label) - 1);
            mem_channels_set(slot - 1, &s);
            applied++;
            break;
        }

        case SEC_WIFI_KNOWN: {
            // ssidN / passN pairs, buffered so the list can be applied in FILE
            // order at the end. Going straight through remember() would reverse
            // it (remember() promotes to the front), and the file is written
            // most-recent-first.
            int idx = atoi(key + 4) - 1;                 // "ssid3"/"pass3" -> 2
            if (idx < 0 || idx >= WIFI_KNOWN_MAX) break;
            if (strncasecmp(key, "ssid", 4) == 0) {
                snprintf(known[idx].ssid, sizeof(known[idx].ssid), "%s", val);
            } else if (strncasecmp(key, "pass", 4) == 0) {
                snprintf(known[idx].pass, sizeof(known[idx].pass), "%s", val);
            } else break;
            if (idx + 1 > known_n) known_n = idx + 1;
            known_touched = true;
            applied++;
            break;
        }

        case SEC_PWR_CAL: {
            /* "band = v:w, v:w, ..." - see the export for why it is a list.
               Points arrive in whatever order the file has them and land in
               the row's own step order, so a file written by a firmware with a
               different PWRCAL_STEPS still restores every point that fits. */
            uint8_t  volts[PWRCAL_STEPS] = {0};
            uint16_t watts[PWRCAL_STEPS] = {0};
            int      k = 0;
            for (char *tok = strtok(val, ","); tok && k < PWRCAL_STEPS; tok = strtok(NULL, ",")) {
                char *t = trim(tok);
                char *colon = strchr(t, ':');
                if (!colon) continue;
                *colon = '\0';
                int v = atoi(trim(t));
                int w = atoi(trim(colon + 1));
                /* A voltage of 0 is the "unset" marker in the row itself, so a
                   zero here is not a measurement - drop it rather than store a
                   point that reads as absent. Same "never fabricate" rule the
                   blob discard follows. */
                if (v <= 0 || v > 255 || w < 0 || w > 65535) continue;
                volts[k] = (uint8_t)v;
                watts[k] = (uint16_t)w;
                k++;
            }
            /* Called even with k == 0: an empty value ("20m =") is how a band
               is REMOVED from the file, and the setter reads an all-zero sweep
               as exactly that. */
            settings_set_pwr_cal_band(key, volts, watts);
            applied++;
            break;
        }

        case SEC_PWR_TARGET: {
            /* "band = 1.50" watts. Parsed as hundredths so the stored unit and
               the printed one round-trip exactly. */
            double w = atof(val);
            if (w >= 0.0 && w < 655.0) {
                settings_set_pwr_target_watts(key, (uint16_t)(w * 100.0 + 0.5));
                applied++;
            }
            break;
        }

        default: break;
        }
    }

    if (filt_touched) settings_set_ft8_filters(&filt);
    if (sip_touched)  settings_set_wifi_static(sip, smask, sgw, sdns);
    // Applied wholesale, in file order: see the buffer's declaration.
    if (known_touched) settings_wifi_known_set_all(known, known_n);
    if (kp_touched)  settings_set_freq_kp_pos(kp_dx, kp_dy);
    if (act_touched) settings_set_activation(act_type, act_ref);
    settings_flush();
    ESP_LOGI(TAG, "imported config: %d keys/slots applied", applied);
    return applied;
}
