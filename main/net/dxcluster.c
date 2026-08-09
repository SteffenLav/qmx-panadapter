// DX cluster spot feed - see dxcluster.h.
//
// Line format, captured from dxfun.com:8000 on 2026-08-09 (these exact lines
// are the self-test vectors at the bottom):
//
//   DX de KE2ELI:    10111.0  DL800PCH     cqing cqota.com                0630Z
//   DX de IZ8STJ:     7061.0  IZ1UIA       LSB CQ CQ                      0641Z
//   DX de EA1BKO:     3705.0  EA2EZ/P      ETE-0128 POTA ES-2081          0638Z
//   DX de PD2WL:      7074.0  M7GFJ        FT8 IO80 Decodium              0640Z
//
// Differs from RBN in three ways that matter: the comment is FREE TEXT (no
// "12 dB", no "22 WPM"), there is usually NO mode field, and a reference may be
// buried anywhere in the comment. So mode is inferred - from a keyword if the
// spotter typed one, otherwise from the band plan segment the frequency falls
// in, which is what a human would do reading the same line.
//
// Node choice matters more than expected: nc7j gave 2 spots in 4 minutes,
// dxfun 75 in 15. A quiet node looks identical to a broken client.

#include "dxcluster.h"
#include "spots.h"
#include "storage/settings.h"
#include "wifi/wifi.h"
#include "util/bandplan.h"
#include "cat/cat.h"
#include "ui.h"
#include "util/psram_task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

static const char *TAG = "dxc";

#define DXC_HOST     "dxfun.com"
#define DXC_PORT     8000
#define DXC_TTL_S    900          // a human spot stays useful longer than a skimmer's
#define DXC_MAX      96
#define LINE_MAX     256
#define RX_TIMEOUT_S 60           // humans are bursty; 30 s of quiet is normal here

typedef struct {
    char     call[12];
    char     ref[10];
    uint32_t freq_hz;
    int      mode;
    int64_t  last_unix;
} dxc_entry_t;

typedef struct {
    dxc_entry_t tab[DXC_MAX];
    int         n;
    spot_t      pub[DXC_MAX];
    char        line[LINE_MAX];
    int         line_len;
    char        rx[512];
} dxc_state_t;

static dxc_state_t *s;            // PSRAM - far too big for a task stack
static int64_t      s_last_line_us;
static volatile int s_pub_count;

int dxcluster_age_s(void)
{
    if (!s_last_line_us) return -1;
    return (int)((esp_timer_get_time() - s_last_line_us) / 1000000);
}
int dxcluster_spot_count(void) { return s_pub_count; }

// ---- parsing ---------------------------------------------------------------

static bool plausible_call(const char *c)
{
    // Same rule the PSK Reporter encoder uses: a callsign has at least one
    // letter AND one digit. Cheap, and it rejects the node chatter that would
    // otherwise sail through ("please", "de", node names).
    bool letter = false, digit = false;
    int n = 0;
    for (const char *p = c; *p; p++, n++) {
        if (isalpha((unsigned char)*p)) letter = true;
        else if (isdigit((unsigned char)*p)) digit = true;
        else if (*p != '/' && *p != '-') return false;
    }
    return letter && digit && n >= 3 && n <= 11;
}

// POTA "ES-2081" / "DL-0123", SOTA "OE/TI-123", WWFF "DLFF-0123". Returned as
// typed; the store only needs something the operator can recognise.
static bool find_reference(const char *comment, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = comment;
    while (*p) {
        while (*p == ' ') p++;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - tok);
        if (len >= 4 && len < cap) {
            // Must contain a '-' with digits after it, and start alphanumeric.
            const char *dash = memchr(tok, '-', len);
            if (dash && dash > tok && isdigit((unsigned char)dash[1]) &&
                isalpha((unsigned char)tok[0])) {
                // Reject a bare callsign-with-suffix by requiring the part
                // before the dash to be letters or a '/'.
                bool ok = true;
                for (const char *q = tok; q < dash; q++)
                    if (!isalpha((unsigned char)*q) && *q != '/') { ok = false; break; }
                if (ok) {
                    memcpy(out, tok, len);
                    out[len] = '\0';
                    for (char *u = out; *u; u++) *u = (char)toupper((unsigned char)*u);
                    return true;
                }
            }
        }
    }
    return false;
}

// 0 other, 1 CW, 2 SSB, 3 digital - matches spot_mode_t.
static int mode_from_comment(const char *comment)
{
    static const struct { const char *kw; int mode; } K[] = {
        { "CW",   1 }, { "LSB",  2 }, { "USB",  2 }, { "SSB",  2 },
        { "PHONE",2 }, { "FT8",  3 }, { "FT4",  3 }, { "RTTY", 3 },
        { "PSK",  3 }, { "JS8",  3 }, { "DIGI", 3 }, { "SSTV", 3 },
    };
    char up[64];
    size_t n = 0;
    for (const char *p = comment; *p && n < sizeof(up) - 1; p++)
        up[n++] = (char)toupper((unsigned char)*p);
    up[n] = '\0';
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        const char *hit = strstr(up, K[i].kw);
        if (!hit) continue;
        // Whole-word only: "CW" must not match inside "CWOPS" or a callsign.
        char before = (hit == up) ? ' ' : hit[-1];
        char after  = hit[strlen(K[i].kw)];
        if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after))
            return K[i].mode;
    }
    return 0;
}

// Fall back to the band plan - the same inference a human makes when a spot
// carries no mode: 14.020 is CW, 14.285 is phone.
static int mode_from_bandplan(uint32_t freq_hz)
{
    qmx_settings_t cfg;
    settings_load_all(&cfg);
    bandplan_region_t reg =
        bandplan_effective_region((bandplan_region_t)cfg.bandplan_region, cfg.my_grid);
    const bp_seg_t *segs = NULL;
    int n = bandplan_get_segments(freq_hz, reg, &segs);
    for (int i = 0; i < n; i++) {
        if (freq_hz < segs[i].lo_hz || freq_hz > segs[i].hi_hz) continue;
        switch (segs[i].type) {
            case BP_CW:    return 1;
            case BP_DIGI:  return 3;
            case BP_PHONE: return 2;
        }
    }
    return 0;
}

bool dxcluster_parse_line(const char *line,
                          char *call_out, size_t call_cap,
                          uint32_t *freq_hz,
                          char *ref_out, size_t ref_cap,
                          int *mode_out)
{
    if (call_out && call_cap) call_out[0] = '\0';
    if (ref_out && ref_cap)   ref_out[0]  = '\0';
    if (mode_out) *mode_out = 0;
    if (!line || strncmp(line, "DX de ", 6) != 0) return false;

    const char *p = strchr(line + 6, ':');
    if (!p) return false;
    // A spotter ending in "-#" is a SKIMMER relayed onto the cluster. Those are
    // RBN's job and would double every entry in the lane, so skip them here.
    if (p - line >= 2 && p[-1] == '#' && p[-2] == '-') return false;
    p++;

    while (*p == ' ') p++;
    char *end = NULL;
    double khz = strtod(p, &end);
    if (!end || end == p || khz < 1000.0 || khz > 60000.0) return false;
    p = end;

    while (*p == ' ') p++;
    const char *call = p;
    while (*p && *p != ' ') p++;
    size_t clen = (size_t)(p - call);
    if (clen == 0 || clen >= call_cap) return false;
    char c[12];
    if (clen >= sizeof(c)) return false;
    memcpy(c, call, clen);
    c[clen] = '\0';
    for (char *u = c; *u; u++) *u = (char)toupper((unsigned char)*u);
    if (!plausible_call(c)) return false;

    // Remainder is the free-text comment, with the HHMMZ stamp at the end.
    while (*p == ' ') p++;
    char comment[96];
    size_t n = 0;
    for (const char *q = p; *q && n < sizeof(comment) - 1; q++) comment[n++] = *q;
    comment[n] = '\0';
    // Trim the trailing "1408Z" so it cannot be mistaken for a reference.
    size_t cl = strlen(comment);
    while (cl > 0 && (comment[cl - 1] == ' ' || comment[cl - 1] == '\r')) comment[--cl] = '\0';
    if (cl >= 5 && (comment[cl - 1] == 'Z' || comment[cl - 1] == 'z')) {
        size_t d = cl - 1;
        int digits = 0;
        while (d > 0 && isdigit((unsigned char)comment[d - 1])) { d--; digits++; }
        if (digits == 4) { comment[d] = '\0'; cl = d; }
    }
    while (cl > 0 && comment[cl - 1] == ' ') comment[--cl] = '\0';

    snprintf(call_out, call_cap, "%s", c);
    if (freq_hz) *freq_hz = (uint32_t)(khz * 1000.0 + 0.5);
    if (ref_out && ref_cap) find_reference(comment, ref_out, ref_cap);
    if (mode_out) {
        int m = mode_from_comment(comment);
        if (m == 0 && freq_hz) m = mode_from_bandplan(*freq_hz);
        *mode_out = m;
    }
    return true;
}

// ---- table -----------------------------------------------------------------

static void note_spot(const char *call, uint32_t freq_hz, const char *ref,
                      int mode, int64_t now)
{
    for (int i = 0; i < s->n; i++) {
        if (strcasecmp(s->tab[i].call, call) != 0) continue;
        s->tab[i].freq_hz   = freq_hz;
        s->tab[i].mode      = mode;
        s->tab[i].last_unix = now;
        if (ref && ref[0]) snprintf(s->tab[i].ref, sizeof(s->tab[i].ref), "%s", ref);
        return;
    }
    if (s->n >= DXC_MAX) {
        // Full: replace the oldest rather than dropping the newest, so a busy
        // band converges on what is happening NOW.
        int oldest = 0;
        for (int i = 1; i < s->n; i++)
            if (s->tab[i].last_unix < s->tab[oldest].last_unix) oldest = i;
        s->n = oldest;   // overwrite in place below
    }
    dxc_entry_t *e = &s->tab[s->n < DXC_MAX ? s->n : 0];
    memset(e, 0, sizeof(*e));
    snprintf(e->call, sizeof(e->call), "%s", call);
    if (ref && ref[0]) snprintf(e->ref, sizeof(e->ref), "%s", ref);
    e->freq_hz   = freq_hz;
    e->mode      = mode;
    e->last_unix = now;
    if (s->n < DXC_MAX) s->n++;
}

static void expire(int64_t now)
{
    int w = 0;
    for (int i = 0; i < s->n; i++)
        if (now - s->tab[i].last_unix <= DXC_TTL_S) s->tab[w++] = s->tab[i];
    s->n = w;
}

static void publish(int64_t now)
{
    expire(now);
    int n = 0;
    for (int i = 0; i < s->n && n < DXC_MAX; i++) {
        spot_t *o = &s->pub[n++];
        memset(o, 0, sizeof(*o));
        // strncpy, not snprintf: both fields live inside the same PSRAM
        // struct, so GCC cannot prove they do not overlap and -Werror=restrict
        // rejects the snprintf. They never do overlap - pub[] and tab[] are
        // separate arrays - but a copy that does not need proving is simpler
        // than an annotation that does.
        strncpy(o->call, s->tab[i].call, sizeof(o->call) - 1);
        strncpy(o->ref,  s->tab[i].ref,  sizeof(o->ref) - 1);
        o->freq_hz    = s->tab[i].freq_hz;
        o->mode       = (spot_mode_t)s->tab[i].mode;
        o->source     = SPOT_SRC_CLUSTER;
        o->heard_unix = s->tab[i].last_unix;
    }
    spots_publish(SPOT_SRC_CLUSTER, s->pub, n);
    s_pub_count = n;
}

// ---- session ---------------------------------------------------------------

static uint32_t s_band_lo, s_band_hi = 0xFFFFFFFFu;

static void refresh_band(void)
{
    uint32_t f = cat_get_frequency();
    uint32_t lo, hi;
    if (!f || !ui_validate_band_freq_hz(f, &lo, &hi)) return;
    if (lo == s_band_lo && hi == s_band_hi) return;
    s_band_lo = lo;
    s_band_hi = hi;
    s->n = 0;
    ESP_LOGI(TAG, "band now %lu-%lu Hz - table cleared",
             (unsigned long)lo, (unsigned long)hi);
}

static int connect_feed(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", DXC_PORT);
    if (getaddrinfo(DXC_HOST, port, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS failed for %s", DXC_HOST);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { .tv_sec = RX_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval tvs = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tvs, sizeof(tvs));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect failed (errno %d)", errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void handle_line(const char *line, int64_t now)
{
    char call[12], ref[10];
    uint32_t hz = 0;
    int mode = 0;
    if (!dxcluster_parse_line(line, call, sizeof(call), &hz, ref, sizeof(ref), &mode))
        return;
    // Band filter at INGEST, like RBN: holding spots for bands we are not on
    // wastes the table and puts stale entries one band-change away from being
    // drawn.
    if (hz < s_band_lo || hz > s_band_hi) return;
    s_last_line_us = esp_timer_get_time();
    note_spot(call, hz, ref, mode, now);
}

static void session(int fd, const char *mycall)
{
    // The node asks for a callsign before it sends anything. It is a login, not
    // authentication - the same string RBN wants.
    char login[24];
    int ln = snprintf(login, sizeof(login), "%s\r\n", mycall);
    bool sent = false;
    int64_t last_pub = 0;
    s->line_len = 0;

    for (;;) {
        int r = recv(fd, s->rx, sizeof(s->rx) - 1, 0);
        if (r <= 0) {
            ESP_LOGW(TAG, "feed closed (r=%d errno=%d)", r, errno);
            return;
        }
        s->rx[r] = '\0';
        if (!sent && (strstr(s->rx, "call") || strstr(s->rx, "login") ||
                      strstr(s->rx, "Please enter"))) {
            if (send(fd, login, ln, 0) != ln) { ESP_LOGW(TAG, "login send failed"); return; }
            sent = true;
            ESP_LOGI(TAG, "connected as %s", mycall);
        }
        int64_t now = (int64_t)time(NULL);
        for (int i = 0; i < r; i++) {
            char ch = s->rx[i];
            if (ch == '\n' || ch == '\r') {
                if (s->line_len) {
                    s->line[s->line_len] = '\0';
                    handle_line(s->line, now);
                    s->line_len = 0;
                }
            } else if (s->line_len < LINE_MAX - 1) {
                s->line[s->line_len++] = ch;
            } else {
                s->line_len = 0;    // overlong: drop it rather than split a spot
            }
        }
        if (now - last_pub >= 10) {
            refresh_band();
            publish(now);
            last_pub = now;
            ESP_LOGI(TAG, "%d station(s) held", s_pub_count);
        }
        qmx_settings_t cfg;
        settings_load_all(&cfg);
        if (!cfg.cluster_en) { ESP_LOGI(TAG, "disabled - closing feed"); return; }
    }
}

static void dxc_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        qmx_settings_t cfg;
        settings_load_all(&cfg);
        if (!cfg.cluster_en || !cfg.my_callsign[0] || !wifi_is_connected()) {
            if (s->n) { s->n = 0; spots_publish(SPOT_SRC_CLUSTER, NULL, 0); s_pub_count = 0; }
            continue;
        }
        refresh_band();
        int fd = connect_feed();
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(30000)); continue; }
        session(fd, cfg.my_callsign);
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void dxcluster_init(void)
{
    if (s) return;
    s = heap_caps_calloc(1, sizeof(dxc_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) { ESP_LOGE(TAG, "no PSRAM for state"); return; }
    psram_task_create(dxc_task, "dxcluster", 5120, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "DX cluster client started (opt-in; state %u B)",
             (unsigned)sizeof(dxc_state_t));
}

// ---- self-test -------------------------------------------------------------

#define T_CHECK(cond, ...) do { if (!(cond)) { ESP_LOGE(TAG, "selftest: " __VA_ARGS__); ok = false; } } while (0)

void dxcluster_selftest(void)
{
    bool ok = true;
    char call[12], ref[10];
    uint32_t hz;
    int mode;

    // Every vector below is a line captured verbatim from dxfun.com:8000 on
    // 2026-08-09 - not invented alongside the parser, which would only prove
    // the parser agrees with itself.
    T_CHECK(dxcluster_parse_line("DX de IZ8STJ:     7061.0  IZ1UIA       LSB CQ CQ                      0641Z",
                                 call, sizeof(call), &hz, ref, sizeof(ref), &mode),
            "rejected a valid phone spot");
    T_CHECK(strcmp(call, "IZ1UIA") == 0, "call '%s'", call);
    T_CHECK(hz == 7061000, "freq %lu", (unsigned long)hz);
    T_CHECK(mode == 2, "LSB should read as SSB, got %d", mode);

    T_CHECK(dxcluster_parse_line("DX de EA1BKO:     3705.0  EA2EZ/P      ETE-0128 POTA ES-2081          0638Z",
                                 call, sizeof(call), &hz, ref, sizeof(ref), &mode),
            "rejected a POTA spot");
    T_CHECK(strcmp(call, "EA2EZ/P") == 0, "call '%s'", call);
    T_CHECK(ref[0] != '\0', "no reference extracted from a POTA comment");

    T_CHECK(dxcluster_parse_line("DX de PD2WL:      7074.0  M7GFJ        FT8 IO80 Decodium              0640Z",
                                 call, sizeof(call), &hz, ref, sizeof(ref), &mode),
            "rejected an FT8 spot");
    T_CHECK(mode == 3, "FT8 should read as digital, got %d", mode);

    // A skimmer relayed onto the cluster must be skipped - RBN already has it,
    // and keeping both would double every entry in the lane.
    T_CHECK(!dxcluster_parse_line("DX de DO4DXA-#: 14011.30  EU1TN          CW    15 dB  23 WPM  CQ      1755Z",
                                  call, sizeof(call), &hz, ref, sizeof(ref), &mode),
            "accepted a -# skimmer line");

    // Node chatter must not become a spot.
    T_CHECK(!dxcluster_parse_line("OZ1LAV de NC7J 09-Aug 0630Z arc6>",
                                  call, sizeof(call), &hz, ref, sizeof(ref), &mode),
            "accepted a node prompt");
    T_CHECK(!dxcluster_parse_line("DX de KE2ELI:    junk  DL800PCH  x  0630Z",
                                  call, sizeof(call), &hz, ref, sizeof(ref), &mode),
            "accepted a line with no parsable frequency");

    ESP_LOGI(TAG, "DX cluster parser self-test: %s", ok ? "PASS" : "FAIL");
}
