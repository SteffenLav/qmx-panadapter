#include "ft8_screen.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "ft8_screen";

#define FT8_CALL_MAX_LEN          16
#define FT8_TEXT_MAX_LEN          40     // FTX_MAX_MESSAGE_LENGTH from ft8_lib is 35
#define FT8_CALL_TABLE_SIZE       128

typedef struct {
    char     call[FT8_CALL_MAX_LEN];
    char     last_text[FT8_TEXT_MAX_LEN];
    int64_t  last_utc;
    int16_t  last_score;
    int16_t  last_freq;
    uint16_t heard_count;
    bool     occupied;
} ft8_call_t;

static ft8_call_t s_table[FT8_CALL_TABLE_SIZE];

// Extract the 2nd whitespace-separated token from msg into out_call.
// FT8 message conventions (per ft8_lib message.c):
//   "CQ X"           -> X is the calling station
//   "CQ DX X"        -> X is the calling station
//   "X Y [grid|73|R+nn]" -> X is destination, Y is source (transmitter)
// We always want the *transmitter*, which is the 2nd token in the
// CQ case and the 2nd token in the QSO case too. So: just take the
// 2nd token. "CQ DX X" is a rare DX-CQ variant; we'll mis-attribute
// it to "DX" - acceptable for 4c.1, fix in 4c.2 if it shows up.
// Returns true on success, false if message has fewer than 2 tokens.
static bool extract_remote_call(const char *msg, char *out_call, size_t cap)
{
    // skip leading whitespace
    while (*msg == ' ') msg++;
    // skip first token
    while (*msg && *msg != ' ') msg++;
    // skip whitespace between tokens
    while (*msg == ' ') msg++;
    if (!*msg) return false;
    // copy second token
    size_t i = 0;
    while (msg[i] && msg[i] != ' ' && i + 1 < cap) {
        out_call[i] = msg[i];
        i++;
    }
    out_call[i] = '\0';
    return i > 0;
}

// Find existing entry for call, or evict oldest to make room.
// Returns pointer to a slot the caller may write into (occupied may
// be false on first insert).
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
    // Table full: evict the LRU entry.
    memset(&s_table[oldest_idx], 0, sizeof(ft8_call_t));
    return &s_table[oldest_idx];
}

void ft8_screen_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    ESP_LOGI(TAG, "FT8 screen data layer ready (%d slots, ~%u B)",
             FT8_CALL_TABLE_SIZE, (unsigned)sizeof(s_table));
}

void ft8_screen_record_decode(const char *text,
                              int score, int freq_off,
                              int64_t utc_sec)
{
    char call[FT8_CALL_MAX_LEN];
    if (!extract_remote_call(text, call, sizeof(call))) {
        return;  // malformed; just skip
    }
    ft8_call_t *e = find_or_evict(call);
    bool first_time = !e->occupied;
    if (first_time) {
        strncpy(e->call, call, FT8_CALL_MAX_LEN - 1);
        e->call[FT8_CALL_MAX_LEN - 1] = '\0';
        e->heard_count = 0;
        e->occupied = true;
    }
    // memcpy + manual NUL (Werror=stringop-truncation)
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
}
