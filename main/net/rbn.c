// Reverse Beacon Network telnet client. Contract and rationale in rbn.h.

#include "rbn.h"
#include "spots.h"
#include "wifi.h"
#include "cat.h"
#include "ui/ui.h"
#include "storage/settings.h"
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

static const char *TAG = "rbn";

#define RBN_HOST      "telnet.reversebeacon.net"
#define RBN_PORT_CW   7000        // CW/RTTY skimmers
#define RBN_PORT_DIGI 7001        // FT8/FT4

// A station stays in our picture for this long after it was last heard by any
// skimmer. RBN re-spots an active CQer every couple of minutes, so 10 minutes is
// generous without keeping stations that have gone away.
#define RBN_TTL_S     600
#define RBN_MAX       120         // deduplicated stations we track
#define PUBLISH_EVERY_MS 10000    // batch into the shared store, don't thrash it
#define LINE_MAX      256
#define RX_TIMEOUT_S  30          // no data for this long: assume the link died

// Bench override for bringing the feature up when there is no way to reach the
// settings toggle (the web UI behind a hotel subnet, nobody at the screen).
// Ships as 0 - the live socket path was verified with it at 1 on 2026-08-04:
// "connected as OZ1LAV" then "76 lines -> 16 stations held" per 10 s window.
#define RBN_FORCE_ON  0

typedef struct {
    // Tracks spot_t.call (see spots.h): a 12-character portable call needs 13
    // bytes, and the _Static_assert in publish() requires these to stay equal.
    char     call[16];
    uint32_t freq_hz;
    int      snr_db;
    int64_t  last_unix;
} rbn_entry_t;

typedef struct {
    rbn_entry_t tab[RBN_MAX];
    int         n;
    spot_t      pub[RBN_MAX];     // publish staging, never points into the store
    char        line[LINE_MAX];
    int         line_len;
    char        rx[512];
} rbn_state_t;

static rbn_state_t *s;            // PSRAM: ~7 KB, far too big for a task stack
static int64_t      s_last_line_us;
static volatile int s_pub_count;

int rbn_age_s(void)
{
    if (!s_last_line_us) return -1;
    return (int)((esp_timer_get_time() - s_last_line_us) / 1000000);
}

int rbn_spot_count(void) { return s_pub_count; }

// ---- parsing ---------------------------------------------------------------

// A callsign must have at least one letter and one digit. Same guard the PSK
// Reporter path uses, and for the same reason: an unresolved or malformed token
// must never be published as if it were a station.
static bool plausible_call(const char *c)
{
    int letters = 0, digits = 0;
    for (const char *p = c; *p; p++) {
        if (isalpha((unsigned char)*p)) letters++;
        else if (isdigit((unsigned char)*p)) digits++;
        else if (*p != '/') return false;
    }
    return letters > 0 && digits > 0 && strlen(c) >= 3 && strlen(c) <= 11;
}

// Feed lines look like:
//   DX de SM7IUN-#:   14018.0  OZ1LAV     CW    12 dB  22 WPM  CQ      1408Z
// Returns true and fills the outputs on a usable spot line. Anything else - the
// login banner, status chatter, a truncated line - is simply not a spot.
//
// Exposed (non-static) only so the self-test can drive it.
bool rbn_parse_line(const char *line, char *call_out, size_t call_cap,
                    uint32_t *freq_hz_out, int *snr_out)
{
    if (strncmp(line, "DX de ", 6) != 0) return false;

    const char *colon = strchr(line + 6, ':');
    if (!colon) return false;

    double khz = 0;
    char call[24] = {0}, mode[16] = {0};
    int snr = 0;
    // The spotter field is skipped deliberately: which skimmer heard it is not
    // something the lane can show in 36 px.
    if (sscanf(colon + 1, " %lf %23s %15s %d", &khz, call, mode, &snr) < 3) return false;

    if (khz < 1000.0 || khz > 60000.0) return false;      // not HF/6m
    if (!plausible_call(call)) return false;

    snprintf(call_out, call_cap, "%s", call);
    *freq_hz_out = (uint32_t)(khz * 1000.0);
    *snr_out = snr;
    return true;
}

// ---- dedupe ----------------------------------------------------------------

// RBN reports the same CQ from every skimmer that hears it - ten or more copies
// of one station is normal - so dedupe is not an optimisation here, it is what
// makes the feed usable at all. Keeps the strongest report per station.
static void note_spot(const char *call, uint32_t freq_hz, int snr, int64_t now)
{
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->tab[i].call, call) == 0) {
            s->tab[i].last_unix = now;
            if (snr > s->tab[i].snr_db) { s->tab[i].snr_db = snr; s->tab[i].freq_hz = freq_hz; }
            return;
        }
    }
    // Pick the slot to write: a free one, else the oldest. Evicting the oldest
    // rather than dropping the newcomer means a busy band cannot freeze the
    // picture at whatever it happened to hold first.
    int slot;
    if (s->n < RBN_MAX) {
        slot = s->n++;
    } else {
        slot = 0;
        for (int i = 1; i < RBN_MAX; i++)
            if (s->tab[i].last_unix < s->tab[slot].last_unix) slot = i;
    }
    snprintf(s->tab[slot].call, sizeof(s->tab[slot].call), "%s", call);
    s->tab[slot].freq_hz   = freq_hz;
    s->tab[slot].snr_db    = snr;
    s->tab[slot].last_unix = now;
}

static void expire(int64_t now)
{
    int keep = 0;
    for (int i = 0; i < s->n; i++)
        if (now - s->tab[i].last_unix <= RBN_TTL_S) s->tab[keep++] = s->tab[i];
    s->n = keep;
}

static void publish(int64_t now)
{
    expire(now);
    int n = 0;
    for (int i = 0; i < s->n && n < RBN_MAX; i++) {
        spot_t *sp = &s->pub[n++];
        memset(sp, 0, sizeof(*sp));
        // Sized copy, not snprintf: both fields live inside *s, so GCC cannot
        // prove they do not overlap and -Wrestrict (an error here) rejects it.
        // Both are char[16], so this is exact.
        _Static_assert(sizeof(sp->call) == sizeof(s->tab[i].call), "call field sizes must match");
        memcpy(sp->call, s->tab[i].call, sizeof(sp->call));
        sp->call[sizeof(sp->call) - 1] = '\0';
        sp->freq_hz    = s->tab[i].freq_hz;
        sp->source     = SPOT_SRC_RBN;
        sp->mode       = SPOT_MODE_CW;     // port 7000 is the CW/RTTY feed
        sp->heard_unix = s->tab[i].last_unix;
    }
    spots_publish(SPOT_SRC_RBN, s->pub, n);
    s_pub_count = n;
}

// ---- session ---------------------------------------------------------------

static int connect_feed(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", RBN_PORT_CW);
    if (getaddrinfo(RBN_HOST, port, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS failed for %s", RBN_HOST);
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

// The band we are currently looking at, or the whole spectrum if that cannot be
// determined (fail open - better a noisy table than an empty one).
static uint32_t s_band_lo, s_band_hi = 0xFFFFFFFFu;

static void refresh_band(void)
{
    uint32_t f = cat_get_frequency();
    uint32_t lo, hi;
    if (!f || !ui_validate_band_freq_hz(f, &lo, &hi)) return;
    if (lo == s_band_lo && hi == s_band_hi) return;
    // Band changed: everything held is about somewhere else now. Expiry would
    // get there in 10 minutes, but the lane would be wrong for those 10 minutes.
    s_band_lo = lo;
    s_band_hi = hi;
    s->n = 0;
    ESP_LOGI(TAG, "band now %lu-%lu Hz - table cleared",
             (unsigned long)lo, (unsigned long)hi);
}

static void handle_line(const char *line, int64_t now)
{
    char call[16];
    uint32_t hz;
    int snr;
    if (!rbn_parse_line(line, call, sizeof(call), &hz, &snr)) return;

    // Keep only what could actually appear on screen. RBN is a GLOBAL feed and
    // the table filled to its 120-station cap within a minute during the first
    // live run, pinned there permanently - so without this the slots go to
    // whichever bands happened to be busiest, and the band the operator is
    // actually on can end up unrepresented. One band at a time is all the lane
    // can show, so one band at a time is all we keep.
    if (hz < s_band_lo || hz > s_band_hi) return;

    note_spot(call, hz, snr, now);
}

static void session(int fd, const char *mycall)
{
    // The feed asks for a callsign before it sends anything. It is a login, not
    // authentication - but it is how RBN attributes load, so send the real one.
    char login[24];
    int ln = snprintf(login, sizeof(login), "%s\r\n", mycall);
    if (send(fd, login, ln, 0) != ln) { ESP_LOGW(TAG, "login send failed"); return; }
    ESP_LOGI(TAG, "connected as %s", mycall);

    s->line_len = 0;
    int64_t last_pub_us = esp_timer_get_time();
    uint32_t lines = 0, spots = 0;

    for (;;) {
        qmx_settings_t st;
        settings_load_all(&st);
        if ((!st.rbn_en && !RBN_FORCE_ON) || !wifi_is_connected()) { ESP_LOGI(TAG, "session ending (disabled or offline)"); return; }

        int r = recv(fd, s->rx, sizeof(s->rx), 0);
        if (r == 0) { ESP_LOGW(TAG, "feed closed by peer"); return; }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "no data for %ds - reconnecting", RX_TIMEOUT_S);
                return;
            }
            ESP_LOGW(TAG, "recv errno %d", errno);
            return;
        }
        s_last_line_us = esp_timer_get_time();

        int64_t now = (int64_t)time(NULL);
        refresh_band();
        for (int i = 0; i < r; i++) {
            char c = s->rx[i];
            if (c == '\n' || c == '\r') {
                if (s->line_len > 0) {
                    s->line[s->line_len] = '\0';
                    lines++;
                    int before = s->n;
                    handle_line(s->line, now);
                    if (s->n != before) spots++;
                    s->line_len = 0;
                }
            } else if (s->line_len < LINE_MAX - 1) {
                s->line[s->line_len++] = c;
            } else {
                s->line_len = 0;         // overlong: drop it, resync on the newline
            }
        }

        if (esp_timer_get_time() - last_pub_us >= PUBLISH_EVERY_MS * 1000LL) {
            last_pub_us = esp_timer_get_time();
            publish(now);
            ESP_LOGI(TAG, "%lu lines -> %d stations held (%lu new)",
                     (unsigned long)lines, s->n, (unsigned long)spots);
            lines = 0; spots = 0;
        }
    }
}

static void rbn_task(void *arg)
{
    (void)arg;
    int backoff_s = 5;
    for (;;) {
        qmx_settings_t st;
        settings_load_all(&st);

        if ((!st.rbn_en && !RBN_FORCE_ON) || !wifi_is_connected()) {
            // Drop anything we were showing: stale RBN spots are worse than none.
            if (s->n) { s->n = 0; spots_publish(SPOT_SRC_RBN, NULL, 0); s_pub_count = 0; }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (!st.my_callsign[0]) {
            ESP_LOGW(TAG, "no callsign set - RBN needs one to log in");
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        int fd = connect_feed();
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            if (backoff_s < 300) backoff_s *= 2;      // be a polite client
            continue;
        }
        backoff_s = 5;
        session(fd, st.my_callsign);
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ---- self-test -------------------------------------------------------------

#define T_CHECK(cond, ...) do { if (!(cond)) { ESP_LOGE(TAG, "SELFTEST FAIL: " __VA_ARGS__); fails++; } } while (0)

void rbn_selftest(void)
{
    int fails = 0;
    char call[16];
    uint32_t hz;
    int snr;

    // These vectors are VERBATIM lines captured from telnet.reversebeacon.net
    // on 2026-08-04, not lines written from memory. That distinction matters:
    // vectors invented alongside the parser only prove the parser agrees with
    // its author's assumption, which is exactly how the PSK Reporter padding bug
    // survived a code review and had to be caught by decoding real bytes.
    T_CHECK(rbn_parse_line("DX de DO4DXA-#: 14011.30  EU1TN          CW    15 dB  23 WPM  CQ      1755Z",
                           call, sizeof(call), &hz, &snr), "standard CW line rejected");
    T_CHECK(strcmp(call, "EU1TN") == 0, "call parsed as '%s'", call);
    T_CHECK(hz == 14011300, "freq parsed as %lu, want 14011300", (unsigned long)hz);
    T_CHECK(snr == 15, "snr parsed as %d, want 15", snr);

    // Spotter callsigns really do carry a hyphen AND a digit suffix, and the
    // column padding varies line to line - both seen in the same capture.
    T_CHECK(rbn_parse_line("DX de EA2RCF-4-#: 10113.00  DL5MCK         CW     6 dB  26 WPM  CQ      1755Z",
                           call, sizeof(call), &hz, &snr), "hyphenated spotter rejected");
    T_CHECK(strcmp(call, "DL5MCK") == 0, "call after hyphenated spotter -> '%s'", call);
    T_CHECK(hz == 10113000, "30m freq -> %lu, want 10113000", (unsigned long)hz);
    T_CHECK(rbn_parse_line("DX de DK3UA-#:   7029.00  YU7RQ          CW     7 dB  28 WPM  CQ      1755Z",
                           call, sizeof(call), &hz, &snr), "wider padding rejected");
    T_CHECK(hz == 7029000, "40m freq -> %lu, want 7029000", (unsigned long)hz);

    // Fractional kHz must land on the right 100 Hz, not be truncated.
    T_CHECK(rbn_parse_line("DX de GX0FRE-#: 14065.30  EU1LL          CW     3 dB  22 WPM  CQ      1755Z",
                           call, sizeof(call), &hz, &snr), "fractional kHz line rejected");
    T_CHECK(hz == 14065300, "14065.30 kHz -> %lu, want 14065300", (unsigned long)hz);

    // Compound calls are real and must survive intact.
    T_CHECK(rbn_parse_line("DX de W3LPL-#: 21023.00  PJ4/K1ABC      CW    18 dB  28 WPM  CQ      1200Z",
                           call, sizeof(call), &hz, &snr), "compound call rejected");
    T_CHECK(strcmp(call, "PJ4/K1ABC") == 0, "compound call -> '%s'", call);

    // Every non-spot line from the real session banner must stay a non-spot.
    T_CHECK(!rbn_parse_line("Please enter your call: Hello, OZ1LAV! Connected.",
                            call, sizeof(call), &hz, &snr), "login prompt accepted as a spot");
    T_CHECK(!rbn_parse_line("Local users: 488", call, sizeof(call), &hz, &snr), "user count accepted");
    T_CHECK(!rbn_parse_line("Spot rate: 5/s (16,757/h)", call, sizeof(call), &hz, &snr), "spot rate accepted");
    T_CHECK(!rbn_parse_line("OZ1LAV de RELAY 04-Aug-2026 17:55Z >", call, sizeof(call), &hz, &snr),
            "relay prompt accepted as a spot");
    T_CHECK(!rbn_parse_line("", call, sizeof(call), &hz, &snr), "empty line accepted");
    T_CHECK(!rbn_parse_line("DX de ", call, sizeof(call), &hz, &snr), "truncated header accepted");
    // Out-of-range frequency (a VHF skimmer) must be dropped, not wrapped.
    T_CHECK(!rbn_parse_line("DX de OH6BG-#: 144300.00  OH2XYZ        CW    10 dB  20 WPM  CQ      1200Z",
                            call, sizeof(call), &hz, &snr), "VHF spot accepted");
    // A token with no digit is not a callsign.
    T_CHECK(!rbn_parse_line("DX de W3LPL-#: 14025.00  CQCQCQ         CW    18 dB  28 WPM  CQ      1200Z",
                            call, sizeof(call), &hz, &snr), "digitless token accepted as a call");
    // An over-long token must be DROPPED, never truncated into a different and
    // perfectly valid-looking callsign - the PSK Reporter lesson again.
    T_CHECK(!rbn_parse_line("DX de W3LPL-#: 14025.00  OZ1LAV0123456  CW    18 dB  28 WPM  CQ      1200Z",
                            call, sizeof(call), &hz, &snr), "over-long call accepted (would truncate)");

    // Dedupe: the same station from four skimmers is ONE station, at its best SNR.
    if (s) {
        int save_n = s->n;
        s->n = 0;
        note_spot("OZ1LAV", 14018000, 12, 1000);
        note_spot("OZ1LAV", 14018000, 25, 1001);
        note_spot("OZ1LAV", 14018000,  7, 1002);
        note_spot("K1ABC",  14020000, 15, 2000);
        T_CHECK(s->n == 2, "4 lines for 2 stations -> %d entries", s->n);
        int idx = (strcmp(s->tab[0].call, "OZ1LAV") == 0) ? 0 : 1;
        T_CHECK(s->tab[idx].snr_db == 25, "kept snr %d, want the best (25)", s->tab[idx].snr_db);
        // A re-spot must REFRESH the entry, not just update its SNR - that is
        // what keeps an active CQer on screen. OZ1LAV's last_unix is therefore
        // 1002 (the third line), not 1000.
        T_CHECK(s->tab[idx].last_unix == 1002, "re-spot left last_unix at %lld, want 1002",
                (long long)s->tab[idx].last_unix);
        // Expiry: past OZ1LAV's TTL but well inside K1ABC's.
        expire(1002 + RBN_TTL_S + 1);
        T_CHECK(s->n == 1, "expiry left %d entries, want 1 (OZ1LAV out, K1ABC in)", s->n);
        if (s->n == 1) T_CHECK(strcmp(s->tab[0].call, "K1ABC") == 0,
                               "expiry kept '%s', want K1ABC", s->tab[0].call);
        s->n = save_n;
    }

    if (fails == 0) ESP_LOGI(TAG, "RBN parser self-test: PASS");
    else            ESP_LOGE(TAG, "RBN parser self-test: %d FAILURE(S)", fails);
}

void rbn_init(void)
{
    if (s) return;
    s = heap_caps_calloc(1, sizeof(rbn_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) { ESP_LOGE(TAG, "no PSRAM for state (%u B)", (unsigned)sizeof(rbn_state_t)); return; }
    rbn_selftest();
    psram_task_create(rbn_task, "rbn", 5120, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "RBN client started (opt-in; state %u B)", (unsigned)sizeof(rbn_state_t));
}
