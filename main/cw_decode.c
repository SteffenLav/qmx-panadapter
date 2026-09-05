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
// Noise squelch. Portable.
// ---------------------------------------------------------------------------
/* '*' is the decoder's marker for a symbol it could not resolve, not a
 * character: it appears nowhere in the CAT manual's list of what the QMX
 * decodes (? . , " ` ( ) + - : @ $ < ! > and the prosigns), and it cannot be
 * part of a callsign. Measured on the bench it was the SINGLE most common thing
 * arriving - 23.6% of 864 characters over 20 minutes on 20 m - which is what
 * "the line fills with rubbish" actually consisted of. E and T together were
 * only 8%, so the first version of this squelch was filtering the wrong thing.
 *
 * So it is dropped outright rather than held for a run verdict. There is no
 * reading of "the decoder could not tell what that was" that is worth a column,
 * and unlike E or T it can never be real text. */
static int is_unresolved(char c) { return c == '*'; }

/* E and T are the two SHORTEST Morse symbols - one dit, one dah - so a random
 * threshold crossing lands on them more often than on anything longer. Unlike
 * '*' they ARE real letters, so they are only dropped as part of a long run. */
static int is_noise_char(char c) { return c == 'E' || c == 'T'; }

int cw_squelch_push(cw_squelch_t *st, char c, char *out, size_t out_sz)
{
    if (!st || !out || out_sz == 0) return 0;

    // A space neither starts nor breaks a run: noise arrives as "T T E T" just
    // as often as "TTET", and letting a space end the run would release every
    // noise character one at a time.
    /* Unresolved symbols never reach the screen. They also COUNT toward the run,
     * so "T*T*E*T" is recognised as one stretch of the decoder struggling
     * rather than as text interrupted by noise. */
    if (is_unresolved(c)) {
        st->run++;
        return 0;
    }

    if (c == ' ' || is_noise_char(c)) {
        if (is_noise_char(c)) st->run++;
        if (st->n_pend < CW_SQUELCH_HOLD) st->pend[st->n_pend++] = c;
        return 0;
    }

    // Anything else settles it.
    size_t n = 0;
    if (st->run < CW_SQUELCH_RUN) {
        // Short run - this was real text with some E/T in it. Release it.
        for (int i = 0; i < st->n_pend && n < out_sz - 1; i++) out[n++] = st->pend[i];
    } else {
        // A long run was the decoder chewing on noise, so it goes. But if the
        // operator stopped sending, the noise ran, and then sending resumed,
        // there WAS a gap - and dropping it wholesale welds the two words
        // together ("CQ TTTTTTTT DE" came out as "CQDE"). One space stands in
        // for whatever was thrown away. Not at the very start, where it would
        // just indent the first word.
        int had_space = 0;
        for (int i = 0; i < st->n_pend; i++) if (st->pend[i] == ' ') had_space = 1;
        if (had_space && st->emitted && n < out_sz - 1) out[n++] = ' ';
    }
    if (n < out_sz - 1) out[n++] = c;
    out[n] = '\0';
    if (n > 0) st->emitted = 1;
    st->n_pend = 0;
    st->run = 0;
    return (int)n;
}

// ---------------------------------------------------------------------------
// Speed. Portable.
// ---------------------------------------------------------------------------
int cw_wpm_estimate(int chars, unsigned elapsed_ms)
{
    if (chars <= 0 || elapsed_ms < 1000) return 0;   // too little to say anything
    // PARIS: five characters to a word.
    unsigned wpm = ((unsigned)chars * 60000u) / (5u * elapsed_ms);
    if (wpm > 99) wpm = 99;     // beyond any real sending speed - do not print it
    return (int)wpm;
}

// ---------------------------------------------------------------------------
// The scrollback (ESP side).
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// A CW QSO's worth of text and then some. PSRAM: nothing here is hot, and
// internal RAM is the scarce resource on this board.
#define CW_RING_CAP 4096

// Speed estimate: arrival times of accepted characters. 30 s is long enough to
// ride out the gaps between words, short enough to follow an operator who
// changes speed. MIN_CHARS stops a couple of stray characters producing a
// confident-looking number.
#define CW_STAMP_CAP       256
#define CW_WPM_WINDOW_MS   30000u
#define CW_WPM_MIN_CHARS   12

static cw_squelch_t     s_squelch;
static uint32_t        *s_stamp;
static int              s_stamp_head, s_stamp_count;
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
    s_stamp = heap_caps_malloc(CW_STAMP_CAP * sizeof(uint32_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_head = s_count = 0;
    s_stamp_head = s_stamp_count = 0;
    s_total = 0;
    memset(&s_squelch, 0, sizeof(s_squelch));
}

void cw_decode_feed(const char *resp)
{
    if (!s_ring) return;
    char chunk[CW_DECODE_MAX_CHUNK + 1];
    int n = cw_decode_parse_tb(resp, chunk, sizeof(chunk), NULL);
    if (n <= 0) return;   // malformed, or simply nothing decoded this poll

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        char rel[CW_SQUELCH_HOLD + 2];
        int m = cw_squelch_push(&s_squelch, chunk[i], rel, sizeof(rel));
        for (int k = 0; k < m; k++) {
            s_ring[s_head] = rel[k];
            s_head = (s_head + 1) % CW_RING_CAP;
            if (s_count < CW_RING_CAP) s_count++;
            // Timestamp every accepted character for the speed estimate. Noise
            // never reaches here, so it cannot drag the figure around.
            s_stamp[s_stamp_head] = (uint32_t)(esp_timer_get_time() / 1000);
            s_stamp_head = (s_stamp_head + 1) % CW_STAMP_CAP;
            if (s_stamp_count < CW_STAMP_CAP) s_stamp_count++;
        }
        s_total += (unsigned)m;
    }
    xSemaphoreGive(s_lock);
}

// Speed over the characters still inside CW_WPM_WINDOW_MS. Anything older is
// ignored, so the figure fades to "nothing to say" on a quiet band instead of
// standing at whatever the last burst measured.
int cw_decode_wpm(void)
{
    if (!s_ring) return 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int      in_window = 0;
    uint32_t oldest    = now;
    for (int i = 0; i < s_stamp_count; i++) {
        int idx = (s_stamp_head + CW_STAMP_CAP - 1 - i) % CW_STAMP_CAP;
        uint32_t t = s_stamp[idx];
        if (now - t > CW_WPM_WINDOW_MS) break;
        in_window++;
        oldest = t;
    }
    xSemaphoreGive(s_lock);

    if (in_window < CW_WPM_MIN_CHARS) return 0;
    return cw_wpm_estimate(in_window, now - oldest);
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
    s_stamp_head = s_stamp_count = 0;
    memset(&s_squelch, 0, sizeof(s_squelch));
    xSemaphoreGive(s_lock);
}

#endif /* ESP_PLATFORM */
