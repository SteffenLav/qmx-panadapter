// See cw_decode.h. The parser is deliberately free of ESP dependencies so
// test/cw_decode_harness.c can link the REAL function rather than a copy of it.

#include "cw_decode.h"

#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// The parser. Portable.
//
// Format, from the QMX CAT manual: TBtnns;
//   TB  the echoed command
//   t   0 = receiving; 1-9 = that many characters of a KY send still to go
//   nn  how many decoded characters follow, as two digits
//   s   the characters themselves
//   ;   terminator
//
// nn is trusted only as far as it agrees with what actually arrived: the count
// and the payload are two statements about the same thing, and a response that
// contradicts itself is not one to guess about. Decoded CW can legitimately
// contain punctuation the manual lists (? . , " ` ( ) + - : @ $ < ! >), so the
// payload is NOT filtered by character - only a NUL or the terminator ends it.
// ---------------------------------------------------------------------------
int cw_decode_parse_tb(const char *resp, char *out, size_t out_sz, int *tx_pending)
{
    if (!resp || !out || out_sz == 0) return -1;
    out[0] = '\0';
    if (tx_pending) *tx_pending = 0;

    if (resp[0] != 'T' || resp[1] != 'B') return -1;
    if (!isdigit((unsigned char)resp[2])) return -1;
    if (!isdigit((unsigned char)resp[3]) || !isdigit((unsigned char)resp[4])) return -1;

    int t  = resp[2] - '0';
    int nn = (resp[3] - '0') * 10 + (resp[4] - '0');
    if (nn > CW_DECODE_MAX_CHUNK) return -1;    // longer than the radio's own buffer

    // The payload must be exactly nn characters and then the terminator. A
    // short one means the response was truncated in transit; taking what
    // arrived would silently drop characters and, worse, could swallow the
    // start of whatever came next.
    const char *p = resp + 5;
    for (int i = 0; i < nn; i++) {
        if (p[i] == '\0' || p[i] == ';') return -1;
    }
    if (p[nn] != ';') return -1;

    size_t n = (size_t)nn;
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    if (tx_pending) *tx_pending = t;
    return (int)n;
}

// ---------------------------------------------------------------------------
// The scrollback (ESP side).
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// A CW QSO's worth of text and then some. PSRAM: nothing here is hot, and
// internal RAM is the scarce resource on this board.
#define CW_RING_CAP 4096

static char            *s_ring;
static size_t           s_head;      // next write position
static size_t           s_count;     // characters held (<= CW_RING_CAP)
static unsigned         s_total;     // ever decoded - the change detector
static SemaphoreHandle_t s_lock;

void cw_decode_init(void)
{
    if (s_ring) return;
    s_lock = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(CW_RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_head = s_count = 0;
    s_total = 0;
}

void cw_decode_feed(const char *resp)
{
    if (!s_ring) return;
    char chunk[CW_DECODE_MAX_CHUNK + 1];
    int n = cw_decode_parse_tb(resp, chunk, sizeof(chunk), NULL);
    if (n <= 0) return;   // malformed, or simply nothing decoded this poll

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        s_ring[s_head] = chunk[i];
        s_head = (s_head + 1) % CW_RING_CAP;
        if (s_count < CW_RING_CAP) s_count++;
    }
    s_total += (unsigned)n;
    xSemaphoreGive(s_lock);
}

// Copy the newest characters, oldest-first within the copied span.
size_t cw_decode_tail(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!s_ring) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t want = out_sz - 1;
    if (want > s_count) want = s_count;
    size_t start = (s_head + CW_RING_CAP - want) % CW_RING_CAP;
    for (size_t i = 0; i < want; i++) out[i] = s_ring[(start + i) % CW_RING_CAP];
    out[want] = '\0';
    xSemaphoreGive(s_lock);
    return want;
}

size_t cw_decode_snapshot(char *out, size_t out_sz)
{
    return cw_decode_tail(out, out_sz);   // the ring IS the scrollback
}

unsigned cw_decode_total(void)
{
    return s_total;
}

void cw_decode_clear(void)
{
    if (!s_ring) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_head = s_count = 0;
    xSemaphoreGive(s_lock);
}

#endif /* ESP_PLATFORM */
