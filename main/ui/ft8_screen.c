#include "ft8_screen.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_screen";

// The decode list is a live picture of who is transmitting *now*, not a log.
// A station that hasn't been re-decoded within this many seconds is expired
// (you can't work a signal that's already gone). ~1 minute keeps stations that
// are actively calling/working (they transmit every 15–30 s) while dropping
// one-shot decodes and stations that have left. Tunable.
#define FT8_ROW_STALE_SEC   60

static ft8_call_t s_table[FT8_CALL_TABLE_SIZE];
static SemaphoreHandle_t s_mutex = NULL;

// Extract the transmitter callsign from an FT8 message.
//
// FT8 message forms:
//   "CQ K1ABC"             -> K1ABC  (plain CQ, 2nd token)
//   "CQ DX K1ABC"          -> K1ABC  (directional CQ, 3rd token)
//   "CQ NA K1ABC"          -> K1ABC  (continent-directional CQ)
//   "CQ POTA K1ABC"        -> K1ABC  (arbitrary directional)
//   "K9XYZ K1ABC FN42"     -> K1ABC  (QSO reply, source = 2nd token)
//   "K9XYZ K1ABC -10"      -> K1ABC  (signal report)
//
// Rule (matches WSJT-X heuristic in packjt77.f90): if the 1st token
// is exactly "CQ" and the 2nd token contains no digit, treat it as
// a directional qualifier and take the 3rd token. Otherwise take
// the 2nd token. Real callsigns nearly always contain a digit;
// directional tokens (DX, NA, EU, AS, AF, SA, OC, POTA, etc.) do not.
static bool token_has_digit(const char *t, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (t[i] >= '0' && t[i] <= '9') return true;
    }
    return false;
}

static bool extract_remote_call(const char *msg, char *out_call, size_t cap)
{
    // Locate token starts/lengths for up to 3 tokens.
    const char *t[3] = {NULL, NULL, NULL};
    size_t      len[3] = {0, 0, 0};
    int n = 0;
    while (*msg == ' ') msg++;
    while (*msg && n < 3) {
        t[n] = msg;
        size_t l = 0;
        while (msg[l] && msg[l] != ' ') l++;
        len[n] = l;
        n++;
        msg += l;
        while (*msg == ' ') msg++;
    }
    if (n < 2) return false;

    // Pick source token: 3rd if 1st is "CQ" and 2nd has no digit.
    int pick = 1;  // default 2nd token
    bool first_is_cq = (len[0] == 2 && t[0][0] == 'C' && t[0][1] == 'Q');
    if (first_is_cq && n >= 3 && !token_has_digit(t[1], len[1])) {
        pick = 2;
    }

    size_t out_len = len[pick];
    if (out_len >= cap) out_len = cap - 1;
    memcpy(out_call, t[pick], out_len);
    out_call[out_len] = '\0';
    return out_len > 0;
}

// Caller must hold s_mutex.
static ft8_call_t *find_or_evict(const char *call)
{
    int free_idx   = -1;
    int oldest_idx = 0;
    int64_t oldest_utc = s_table[0].last_utc;
    for (int i = 0; i < FT8_CALL_TABLE_SIZE; i++) {
        if (s_table[i].occupied) {
            if (strncmp(s_table[i].call, call, FT8_CALL_MAX_LEN) == 0) {
                return &s_table[i];
            }
            if (s_table[i].last_utc < oldest_utc) {
                oldest_utc = s_table[i].last_utc;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) return &s_table[free_idx];
    memset(&s_table[oldest_idx], 0, sizeof(ft8_call_t));
    return &s_table[oldest_idx];
}

void ft8_screen_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "FT8 screen data layer ready (%d slots, ~%u B, mutex=%p)",
             FT8_CALL_TABLE_SIZE, (unsigned)sizeof(s_table), s_mutex);
}

// Parse a Maidenhead grid out of an FT8 message. Grids appear as the
// last whitespace-separated token of a CQ message ("CQ K1ABC FN42")
// or the 3rd token of an exchange ("K1ABC K9XYZ FN42"). They are 4 or
// 6 chars: 2 letters + 2 digits + (optional) 2 letters. Anything that
// doesn't match (e.g. signal reports "-10", "R+05", "73", "RR73") is
// ignored. Writes "" to out_grid when no grid is found.
static bool token_looks_like_grid(const char *t, size_t len)
{
    if (len != 4 && len != 6) return false;
    // "RR73" is a reserved FT8 roger/73 token, not a grid square, even
    // though it syntactically matches AA00..RR99 (R is a valid field letter).
    if (len == 4 && t[0] == 'R' && t[1] == 'R' && t[2] == '7' && t[3] == '3') return false;
    if (t[0] < 'A' || t[0] > 'R') return false;
    if (t[1] < 'A' || t[1] > 'R') return false;
    if (t[2] < '0' || t[2] > '9') return false;
    if (t[3] < '0' || t[3] > '9') return false;
    if (len == 6) {
        // Subsquare letters can be upper or lower a..x; we'll see them upper
        // from the wire, but be tolerant.
        char c4 = t[4]; if (c4 >= 'a' && c4 <= 'z') c4 = (char)(c4 - 32);
        char c5 = t[5]; if (c5 >= 'a' && c5 <= 'z') c5 = (char)(c5 - 32);
        if (c4 < 'A' || c4 > 'X') return false;
        if (c5 < 'A' || c5 > 'X') return false;
    }
    return true;
}

static void extract_grid_from_text(const char *msg, char *out_grid, size_t cap)
{
    out_grid[0] = '\0';
    // Scan to last token.
    const char *p = msg;
    const char *last_tok = NULL;
    size_t last_len = 0;
    while (*p == ' ') p++;
    while (*p) {
        const char *tok = p;
        size_t l = 0;
        while (p[l] && p[l] != ' ') l++;
        last_tok = tok;
        last_len = l;
        p += l;
        while (*p == ' ') p++;
    }
    if (last_tok && token_looks_like_grid(last_tok, last_len)) {
        size_t n = last_len;
        if (n >= cap) n = cap - 1;
        memcpy(out_grid, last_tok, n);
        out_grid[n] = '\0';
    }
}

void ft8_screen_record_decode(const char *text,
                              int score, int snr_db, int freq_off,
                              int64_t utc_sec)
{
    char call[FT8_CALL_MAX_LEN];
    if (!extract_remote_call(text, call, sizeof(call))) {
        return;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "record_decode: mutex timeout, dropping '%s'", text);
        return;
    }
    ft8_call_t *e = find_or_evict(call);
    bool first_time = !e->occupied;
    if (first_time) {
        strncpy(e->call, call, FT8_CALL_MAX_LEN - 1);
        e->call[FT8_CALL_MAX_LEN - 1] = '\0';
        e->heard_count = 0;
        e->occupied = true;
    }
    size_t n = strlen(text);
    if (n >= FT8_TEXT_MAX_LEN) n = FT8_TEXT_MAX_LEN - 1;
    memcpy(e->last_text, text, n);
    e->last_text[n] = '\0';
    e->last_utc   = utc_sec;
    e->last_score = (int16_t)score;
    e->last_snr_db = (int16_t)snr_db;
    e->last_freq  = (int16_t)freq_off;
    // Update grid if this message contains one (don't clobber prior grid
    // with empty when later messages omit it).
    char new_grid[FT8_GRID_MAX_LEN];
    extract_grid_from_text(text, e->last_grid, FT8_GRID_MAX_LEN);
    extract_grid_from_text(text, new_grid, FT8_GRID_MAX_LEN);
    if (new_grid[0]) {
        memcpy(e->last_grid, new_grid, FT8_GRID_MAX_LEN);
    }
    e->heard_count++;
    ESP_LOGI(TAG, "%s call=%-10s heard=%u score=%d freq=%d text='%s'",
             first_time ? "NEW " : "UPD ",
             e->call, e->heard_count, e->last_score, e->last_freq,
             e->last_text);
    if (s_mutex) xSemaphoreGive(s_mutex);
}

void ft8_screen_get_all(ft8_call_t *out, int max, int *count_out)
{
    int n = 0;
    if (count_out) *count_out = 0;
    if (!out || max <= 0) return;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "get_all: mutex timeout");
        return;
    }
    // Expire stale stations as we snapshot: anything not re-decoded within
    // FT8_ROW_STALE_SEC is freed here, so it vanishes from the view, the active
    // count, and the CQ clear-frequency scan in one place. last_utc and now are
    // both UTC seconds (slot start vs wall clock); valid because FT8 mode gates
    // on SNTP sync. Scan the whole table (not limited by max) so purging is
    // complete even if the caller's buffer fills.
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < FT8_CALL_TABLE_SIZE; i++) {
        if (!s_table[i].occupied) continue;
        if (now - s_table[i].last_utc > FT8_ROW_STALE_SEC) {
            s_table[i].occupied = false;   // station went quiet — drop it
            continue;
        }
        if (n < max) out[n++] = s_table[i];
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    if (count_out) *count_out = n;
}

void ft8_screen_clear(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "clear: mutex timeout");
        return;
    }
    memset(s_table, 0, sizeof(s_table));
    ESP_LOGI(TAG, "decode list cleared");
    if (s_mutex) xSemaphoreGive(s_mutex);
}
