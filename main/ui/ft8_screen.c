#include "ft8_screen.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_screen";

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

void ft8_screen_record_decode(const char *text,
                              int score, int freq_off,
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
    e->last_freq  = (int16_t)freq_off;
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
    for (int i = 0; i < FT8_CALL_TABLE_SIZE && n < max; i++) {
        if (s_table[i].occupied) {
            out[n++] = s_table[i];
        }
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    if (count_out) *count_out = n;
}
