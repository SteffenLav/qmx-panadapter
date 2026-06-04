#include "ft8_screen.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_screen";

static ft8_call_t s_table[FT8_CALL_TABLE_SIZE];
static SemaphoreHandle_t s_mutex = NULL;

// Extract the 2nd whitespace-separated token from msg into out_call.
// FT8 message convention: 2nd token is the transmitter in both CQ
// and QSO forms. "CQ DX X" mis-attributes to "DX" (rare edge case).
static bool extract_remote_call(const char *msg, char *out_call, size_t cap)
{
    while (*msg == ' ') msg++;
    while (*msg && *msg != ' ') msg++;
    while (*msg == ' ') msg++;
    if (!*msg) return false;
    size_t i = 0;
    while (msg[i] && msg[i] != ' ' && i + 1 < cap) {
        out_call[i] = msg[i];
        i++;
    }
    out_call[i] = '\0';
    return i > 0;
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
