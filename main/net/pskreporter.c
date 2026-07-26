// PSK Reporter reception-report uploader - see pskreporter.h.
//
// Implemented from the published protocol description (pskreporter.info/
// pskdev.html), NOT ported from WSJT-X (GPL). The datagram is IPFIX
// (RFC 5101, simplified per the spec's "cookie cutter" section):
//
//   header:  00 0A len(2) time(4) seq(4) randid(4)
//   receiver descriptor (template 0x9992): receiverCallsign, receiverLocator,
//            decoderSoftware  (all variable-length strings)
//   sender descriptor (template 0x9993): senderCallsign, frequency(4),
//            sNR(1), mode, informationSource(1), senderLocator,
//            flowStartSeconds(4)
//   receiver record block (0x9992), then sender records block (0x9993),
//   each null-padded to a multiple of 4.
//
// All enterprise fields use PEN 30351 (0x768F). informationSource = 1
// ("automatically extracted"). The descriptor field-set matches what the
// server's generic IPFIX decoder accepts (same combination WSJT-X uses:
// SNR but no IMD, plus senderLocator when known).

#include "pskreporter.h"
#include "storage/settings.h"
#include "wifi/wifi.h"
#include "util/psram_task.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "pskrep";

#define PSK_HOST          "report.pskreporter.info"
#define PSK_PORT          4739
#define SEND_PERIOD_S     300      // spec: no more than one datagram per 5 min
#define SEND_JITTER_S     30       // de-synchronize senders (spec requirement)
#define DESC_EVERY_S      3600     // re-send descriptors hourly
#define BATCH_MAX         64       // spots per batch (spec: ~80-90 fit a datagram)
#define DGRAM_MAX         1400     // stay under a single MTU

typedef struct {
    char     call[12];
    char     grid[8];
    uint32_t freq_hz;
    int8_t   snr_db;
    char     mode[6];     // "FT8" / "FT4" (ADIF values)
    uint32_t utc_sec;
} spot_t;

static spot_t            s_batch[BATCH_MAX];
static int               s_batch_n = 0;
static SemaphoreHandle_t s_lock;
static bool              s_running = false;
static uint32_t          s_seq = 0;         // cumulative REPORT count (spec)
static uint32_t          s_rand_id = 0;     // constant per session
static int               s_sock = -1;       // kept open: same source port all session

// ---------------------------------------------------------------------------
// Datagram assembly helpers (all values network byte order)
// ---------------------------------------------------------------------------

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// Variable-length string field: 1-byte length + bytes.
static int put_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    if (n > 254) n = 254;
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return (int)(n + 1);
}

// Receiver descriptor: template 0x9992, 3 fields (callsign, locator, software).
static const uint8_t DESC_RX[] = {
    0x00,0x03, 0x00,0x24, 0x99,0x92, 0x00,0x03, 0x00,0x01,
    0x80,0x02, 0xFF,0xFF, 0x00,0x00,0x76,0x8F,
    0x80,0x04, 0xFF,0xFF, 0x00,0x00,0x76,0x8F,
    0x80,0x08, 0xFF,0xFF, 0x00,0x00,0x76,0x8F,
    0x00,0x00,
};

// Sender descriptor: template 0x9993, 7 fields — senderCallsign, frequency(4),
// sNR(1), mode, informationSource(1), senderLocator, flowStartSeconds(4).
static const uint8_t DESC_TX[] = {
    0x00,0x02, 0x00,0x3C, 0x99,0x93, 0x00,0x07,
    0x80,0x01, 0xFF,0xFF, 0x00,0x00,0x76,0x8F,   // senderCallsign (var)
    0x80,0x05, 0x00,0x04, 0x00,0x00,0x76,0x8F,   // frequency u32
    0x80,0x06, 0x00,0x01, 0x00,0x00,0x76,0x8F,   // sNR i8
    0x80,0x0A, 0xFF,0xFF, 0x00,0x00,0x76,0x8F,   // mode (var)
    0x80,0x0B, 0x00,0x01, 0x00,0x00,0x76,0x8F,   // informationSource u8
    0x80,0x03, 0xFF,0xFF, 0x00,0x00,0x76,0x8F,   // senderLocator (var)
    0x00,0x96, 0x00,0x04,                        // flowStartSeconds u32
};

// Shortest data record each template can produce: every fixed field at its
// stated width, every variable-length field at one length octet with an empty
// value. RFC 5101/7011 s3.3.1 requires any trailing padding in a Data Set to be
// SHORTER than this - otherwise the padding is byte-identical to a record of
// empty values and a collector may ingest a phantom record or reject the
// message. See the padding note in build_datagram().
#define PSK_MIN_REC_RX  3    // 0x9992: 3 variable-length fields
#define PSK_MIN_REC_TX  13   // 0x9993: 3 var (3) + frequency(4) + sNR(1)
                             //         + informationSource(1) + flowStart(4)

// Pad a just-written Data Set to a 4-byte boundary, but ONLY while the padding
// stays shorter than min_rec (see above). Alignment is a SHOULD in IPFIX - the
// set length field locates the next set either way - so when the two rules
// collide we keep the MUST and drop the alignment.
static int pad_data_set(uint8_t *buf, int off, int start, int min_rec)
{
    int pad = (4 - ((off - start) % 4)) % 4;
    if (pad < min_rec) while (pad--) buf[off++] = 0;
    return off;
}

// Build the full datagram into buf. Returns length, or 0 if nothing to send.
static int build_datagram(uint8_t *buf, const spot_t *spots, int n,
                          bool with_desc, const char *my_call,
                          const char *my_grid, const char *sw)
{
    int off = 16;   // header written last (needs total length)

    if (with_desc) {
        memcpy(buf + off, DESC_RX, sizeof(DESC_RX)); off += sizeof(DESC_RX);
        memcpy(buf + off, DESC_TX, sizeof(DESC_TX)); off += sizeof(DESC_TX);
    }

    // Receiver information record block (0x9992), padded to 4.
    {
        int start = off; off += 4;   // block header written after
        off += put_str(buf + off, my_call);
        off += put_str(buf + off, my_grid);
        off += put_str(buf + off, sw);
        // NOTE: this set is the one that can collide with the padding rule.
        // Its shortest record is only 3 octets, and whether the natural pad
        // reaches 3 depends purely on callsign + grid + version-string lengths
        // - so an unguarded pad here corrupts the datagram for SOME stations
        // and not others (measured: ~5 of 18 realistic call/grid combinations).
        off = pad_data_set(buf, off, start, PSK_MIN_REC_RX);
        put_u16(buf + start, 0x9992);
        put_u16(buf + start + 2, (uint16_t)(off - start));
    }

    // Sender records block (0x9993), padded to 4.
    {
        int start = off; off += 4;
        for (int i = 0; i < n; i++) {
            if (off > DGRAM_MAX - 64) break;   // safety: never overrun the MTU
            off += put_str(buf + off, spots[i].call);
            put_u32(buf + off, spots[i].freq_hz); off += 4;
            buf[off++] = (uint8_t)spots[i].snr_db;
            off += put_str(buf + off, spots[i].mode);
            buf[off++] = 1;                    // informationSource: auto-extracted
            off += put_str(buf + off, spots[i].grid);   // "" -> length 0 (unknown)
            put_u32(buf + off, spots[i].utc_sec); off += 4;
        }
        // A sender record is at least 13 octets, so the pad (<=3) is always
        // legal here; routed through the same helper so the rule holds if the
        // template ever changes.
        off = pad_data_set(buf, off, start, PSK_MIN_REC_TX);
        put_u16(buf + start, 0x9993);
        put_u16(buf + start + 2, (uint16_t)(off - start));
    }

    // Header: version 0x000A, length, export time, sequence, random id.
    put_u16(buf, 0x000A);
    put_u16(buf + 2, (uint16_t)off);
    put_u32(buf + 4, (uint32_t)time(NULL));
    put_u32(buf + 8, s_seq);
    put_u32(buf + 12, s_rand_id);
    return off;
}

static bool send_datagram(const uint8_t *buf, int len)
{
    if (s_sock < 0) {
        s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_sock < 0) { ESP_LOGW(TAG, "socket() failed"); return false; }
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(PSK_HOST, NULL, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS lookup for " PSK_HOST " failed");
        return false;
    }
    ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(PSK_PORT);
    int rc = sendto(s_sock, buf, len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != len) { ESP_LOGW(TAG, "sendto: %d (errno %d)", rc, errno); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Public feeder + sender task
// ---------------------------------------------------------------------------

// Is this a callsign we may publish to a public database?
//
// The decode path hands us whatever ft8_screen_extract_call() found, and an
// UNRESOLVED hashed callsign legitimately arrives as "..." (ft8_lib renders a
// 12/22-bit hash it cannot resolve as "<...>", which the extractor strips to
// "..."). That is harmless in the station table - it matches nothing - but it
// must never be reported as a reception report: the spot would be a bogus
// entry in a public database that other operators rely on. WSJT-X likewise
// reports nothing for a call it could not resolve.
//
// A real callsign always has at least one letter AND one digit, and never a
// '.'; '/' is allowed so compound calls ("PJ4/K1ABC") still spot.
static bool call_is_publishable(const char *c)
{
    bool has_alpha = false, has_digit = false;
    for (; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') { has_alpha = true; continue; }
        if (*c >= 'a' && *c <= 'z') { has_alpha = true; continue; }
        if (*c >= '0' && *c <= '9') { has_digit = true; continue; }
        if (*c != '/') return false;   // '.', '<', '>' or any other junk
    }
    return has_alpha && has_digit;
}

void pskreporter_spot(const char *call, const char *grid,
                      uint32_t freq_hz, int snr_db,
                      const char *mode, int64_t utc_sec)
{
    if (!s_running || !call || !call[0]) return;
    if (!call_is_publishable(call)) return;
    // Never publish a TRUNCATED call - that reports a different station, which
    // is worse than reporting none. 11 chars is the FT8 maximum, so this only
    // fires on something unexpected.
    if (strlen(call) >= sizeof(s_batch[0].call)) return;

    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.pskreporter_en) return;
    if (s.sim_mode_en) return;   // phantoms must never reach the real world

    if (snr_db < -128) snr_db = -128;
    if (snr_db > 127)  snr_db = 127;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // One report per callsign per batch (spec) - refresh in place if re-heard.
    int i;
    for (i = 0; i < s_batch_n; i++) {
        if (strcmp(s_batch[i].call, call) == 0) break;
    }
    if (i == s_batch_n) {
        if (s_batch_n >= BATCH_MAX) { xSemaphoreGive(s_lock); return; }
        s_batch_n++;
        memset(&s_batch[i], 0, sizeof(s_batch[i]));
        strncpy(s_batch[i].call, call, sizeof(s_batch[i].call) - 1);
    }
    if (grid && grid[0]) {
        strncpy(s_batch[i].grid, grid, sizeof(s_batch[i].grid) - 1);
        s_batch[i].grid[sizeof(s_batch[i].grid) - 1] = '\0';
    }
    s_batch[i].freq_hz = freq_hz;
    s_batch[i].snr_db  = (int8_t)snr_db;
    strncpy(s_batch[i].mode, mode ? mode : "FT8", sizeof(s_batch[i].mode) - 1);
    s_batch[i].utc_sec = (uint32_t)utc_sec;
    xSemaphoreGive(s_lock);
}

static void psk_task(void *arg)
{
    (void)arg;
    int sends = 0;
    int64_t last_desc_s = 0;

    while (1) {
        // Randomized 5-min cadence anchored to task start, per the spec.
        vTaskDelay(pdMS_TO_TICKS((SEND_PERIOD_S + (esp_random() % SEND_JITTER_S)) * 1000));

        qmx_settings_t s;
        settings_load_all(&s);
        if (!s.pskreporter_en || s.sim_mode_en) continue;
        if (!s.my_callsign[0] || !s.my_grid[0]) continue;   // receiver record needs both
        if (!wifi_is_connected()) continue;

        // Snapshot + clear the batch.
        spot_t local[BATCH_MAX];
        int n;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        n = s_batch_n;
        memcpy(local, s_batch, sizeof(spot_t) * n);
        s_batch_n = 0;
        xSemaphoreGive(s_lock);
        if (n == 0) continue;

        char sw[48];
        const esp_app_desc_t *app = esp_app_get_description();
        snprintf(sw, sizeof(sw), "QMX Panadapter %s", app ? app->version : "?");

        bool with_desc = (sends < 3) ||
                         (time(NULL) - last_desc_s >= DESC_EVERY_S);

        static uint8_t dgram[DGRAM_MAX];   // single task, no reentry
        int len = build_datagram(dgram, local, n, with_desc,
                                 s.my_callsign, s.my_grid, sw);
        s_seq += (uint32_t)n;   // sequence counts REPORTS, not packets

        if (send_datagram(dgram, len)) {
            sends++;
            if (with_desc) last_desc_s = time(NULL);
            ESP_LOGI(TAG, "sent %d spot(s), %d bytes, seq=%lu%s",
                     n, len, (unsigned long)s_seq, with_desc ? " (+descriptors)" : "");
        } else {
            ESP_LOGW(TAG, "send failed - %d spot(s) dropped", n);
        }
    }
}

// === BENCH SELF-TEST - OFF in shipping builds ==========================
// Flip to 1 to verify the datagram WITHOUT an antenna, a QMX or a live band:
// it feeds synthetic spots through the real pskreporter_spot() entry point,
// builds a real datagram, and hex-dumps it to the log. It deliberately does
// NOT transmit - nothing may reach the public collector from a bench test.
//
//   1. set to 1, build + flash
//   2. curl http://<ip>/api/log | grep SELFTEST
//   3. strip the "NNNN " offsets, concatenate the hex, unhexlify to a .bin
//   4. test/psk_harness.exe that.bin
//
// This is how the RFC 7011 s3.3.1 padding bug was found (2026-07-26): PSK
// Reporter never acknowledges anything, so a malformed datagram is silently
// discarded and the device log looks perfectly healthy. Keep this hook.
#define PSK_BENCH_SELFTEST 0
#if PSK_BENCH_SELFTEST
static void psk_bench_selftest(void)
{
    // Feed through the real public entry point so the callsign guard is
    // exercised too: two good calls, plus junk that must be dropped.
    pskreporter_spot("K1ABC",     "FN42", 14075500u, -12, "FT8", 1770000000);
    pskreporter_spot("PJ4/K9XYZ", "",     14074900u,  -3, "FT4", 1770000015);
    pskreporter_spot("...",       "",     14074000u, -10, "FT8", 1770000030);  // must be dropped
    pskreporter_spot("OZ",        "JO65", 14074000u, -10, "FT8", 1770000030);  // no digit -> dropped

    qmx_settings_t s;
    settings_load_all(&s);
    char sw[48];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(sw, sizeof(sw), "QMX Panadapter %s", app ? app->version : "?");

    static uint8_t dg[DGRAM_MAX];
    int len = build_datagram(dg, s_batch, s_batch_n, true,
                             s.my_callsign, s.my_grid, sw);
    ESP_LOGI(TAG, "SELFTEST spots_accepted=%d len=%d", s_batch_n, len);
    // 32 bytes per line keeps each line well inside the log's line buffer.
    for (int i = 0; i < len; i += 32) {
        char hex[80]; int p = 0;
        for (int j = i; j < i + 32 && j < len; j++)
            p += snprintf(hex + p, sizeof(hex) - p, "%02X", dg[j]);
        ESP_LOGI(TAG, "SELFTEST %04d %s", i, hex);
    }
    s_batch_n = 0;   // never let synthetic spots reach a real datagram
}
#endif
// === END TEMPORARY =====================================================

void pskreporter_init(void)
{
    if (s_running) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    s_rand_id = esp_random();
    if (psram_task_create(psk_task, "pskrep", 6144, NULL, 2, tskNO_AFFINITY)) {
        s_running = true;
        // Report the effective state at boot: it is the quickest way to tell,
        // from a user's diagnostic log, whether spotting is actually enabled
        // and why nothing is being reported (setting off, or no callsign/grid).
        qmx_settings_t s;
        settings_load_all(&s);
        ESP_LOGI(TAG, "PSK Reporter sender ready (id=0x%08lx) - spotting %s, call='%s' grid='%s'",
                 (unsigned long)s_rand_id, s.pskreporter_en ? "ENABLED" : "disabled",
                 s.my_callsign, s.my_grid);
#if PSK_BENCH_SELFTEST
        psk_bench_selftest();
#endif
    }
}
