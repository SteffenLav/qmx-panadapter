#include "config_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdbool.h>

#include "esp_log.h"
#include "settings.h"
#include "mem_channels.h"

static const char *TAG = "config_io";

#define CFG_BUF_BYTES 8192   // settings + 32 memories fit comfortably

static const char *yn(bool b) { return b ? "true" : "false"; }

// ---- Export -------------------------------------------------------------
char *config_io_export(size_t *out_len)
{
    qmx_settings_t c;
    settings_load_all(&c);

    char *buf = malloc(CFG_BUF_BYTES);
    if (!buf) return NULL;
    int n = 0;
    int cap = CFG_BUF_BYTES;
    #define APP(...) do { if (n < cap) n += snprintf(buf + n, cap - n, __VA_ARGS__); } while (0)

    APP("# QMX Panadapter config (M5Stack Tab5).\n");
    APP("# Edit values and re-upload, or share a single section (e.g. [memories]).\n");
    APP("# Lines starting with # are ignored; unknown keys are ignored. Upload\n");
    APP("# MERGES: only keys present here change. Most settings apply on restart.\n");
    APP("# NOTE: wifi_pass / qrz_key / eqsl_pass are stored here in clear text.\n\n");

    APP("[settings]\n");
    APP("callsign           = %s\n", c.my_callsign);
    APP("grid               = %s\n", c.my_grid);
    APP("wifi_ssid          = %s\n", c.wifi_ssid);
    APP("wifi_pass          = %s\n", c.wifi_pass);
    APP("wifi_enabled       = %s\n", yn(c.wifi_enabled));
    APP("cw_pitch_hz        = %u\n", (unsigned)c.cw_pitch_hz);
    APP("if_cal_hz          = %d\n", (int)c.cw_cal_hz);
    APP("iq_balance         = %s\n", yn(c.iq_enabled));
    APP("flat_spectrum      = %s\n", yn(c.flat_mode));
    APP("zoom               = %.2f\n", (double)c.zoom_factor);
    APP("colormap           = %u\n", (unsigned)c.colormap_idx);
    APP("brightness         = %u\n", (unsigned)c.brightness_pct);
    APP("ema_alpha          = %.2f\n", (double)c.ema_alpha);
    APP("db_min             = %.0f\n", (double)c.db_min);
    APP("db_max             = %.0f\n", (double)c.db_max);
    APP("diag_log           = %s\n", yn(c.diag_log));
    APP("qmx_gps            = %s\n", yn(c.qmx_gps));
    APP("freq_keypad_10key  = %s\n", yn(c.freq_kp_calc));
    APP("onboarded          = %s\n", yn(c.onboarded));
    APP("qrz_key            = %s\n", c.qrz_api_key);
    APP("eqsl_user          = %s\n", c.eqsl_user);
    APP("eqsl_pass          = %s\n", c.eqsl_pswd);

    APP("\n[cq]\n");
    APP("active = %u\n", (unsigned)(c.cq_sel + 1));   // 1-based for the user
    APP("1 = %s\n", c.cq_msg[0]);
    APP("2 = %s\n", c.cq_msg[1]);
    APP("3 = %s\n", c.cq_msg[2]);

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

    if (n >= cap) n = cap - 1;
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

typedef enum { SEC_NONE, SEC_SETTINGS, SEC_CQ, SEC_FILTERS, SEC_MEM } section_t;

int config_io_import(char *text)
{
    if (!text) return 0;

    // ft8_filters are merged into the current value and written once at the end.
    qmx_settings_t cur;
    settings_load_all(&cur);
    ft8_filters_t filt = cur.ft8_filters;
    bool filt_touched = false;

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
            else if (!strcasecmp(key, "cw_pitch_hz"))       settings_set_cw_pitch_hz((uint16_t)atoi(val));
            else if (!strcasecmp(key, "if_cal_hz"))         settings_set_cw_cal_hz((int16_t)atoi(val));
            else if (!strcasecmp(key, "iq_balance"))        settings_set_iq_enabled(to_bool(val));
            else if (!strcasecmp(key, "flat_spectrum"))     settings_set_flat_mode(to_bool(val));
            else if (!strcasecmp(key, "zoom"))              settings_set_zoom_factor((float)atof(val));
            else if (!strcasecmp(key, "colormap"))          settings_set_colormap_idx((uint8_t)atoi(val));
            else if (!strcasecmp(key, "brightness"))        settings_set_brightness_pct((uint8_t)atoi(val));
            else if (!strcasecmp(key, "ema_alpha"))         settings_set_ema_alpha((float)atof(val));
            else if (!strcasecmp(key, "db_min"))            settings_set_db_min((float)atof(val));
            else if (!strcasecmp(key, "db_max"))            settings_set_db_max((float)atof(val));
            else if (!strcasecmp(key, "diag_log"))          settings_set_diag_log(to_bool(val));
            else if (!strcasecmp(key, "qmx_gps"))           settings_set_qmx_gps(to_bool(val));
            else if (!strcasecmp(key, "freq_keypad_10key")) settings_set_freq_kp_calc(to_bool(val));
            else if (!strcasecmp(key, "onboarded"))         settings_set_onboarded(to_bool(val));
            else if (!strcasecmp(key, "qrz_key"))           settings_set_qrz_api_key(val);
            else if (!strcasecmp(key, "eqsl_user"))         settings_set_eqsl_user(val);
            else if (!strcasecmp(key, "eqsl_pass"))         settings_set_eqsl_pswd(val);
            else break;   // unknown key: ignore, don't count
            applied++;
            break;

        case SEC_CQ:
            if (!strcasecmp(key, "active")) {
                int s = atoi(val) - 1;          // file is 1-based
                if (s >= 0 && s <= 2) { settings_set_cq_sel((uint8_t)s); applied++; }
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

        default: break;
        }
    }

    if (filt_touched) settings_set_ft8_filters(&filt);
    settings_flush();
    ESP_LOGI(TAG, "imported config: %d keys/slots applied", applied);
    return applied;
}
