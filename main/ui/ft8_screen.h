#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Step 4c.2 v0.10: FT8 decode aggregation - data layer.
//
// 4c.1: data layer built. Each decoded message is parsed for its
//       remote callsign (2nd token by FT8 message convention) and
//       merged into a fixed-size table, keyed by that callsign.
// 4c.2: LVGL list widget (ft8_screen_view) snapshots this table
//       at slot end. A mutex now guards table access (writer =
//       ft8_task on core 1, readers = LVGL timer on core 0).

#define FT8_CALL_MAX_LEN          16
#define FT8_TEXT_MAX_LEN          40   // FTX_MAX_MESSAGE_LENGTH is 35
#define FT8_CALL_TABLE_SIZE       128

#define FT8_GRID_MAX_LEN          7   // "JO45ab" + NUL

typedef struct {
    char     call[FT8_CALL_MAX_LEN];
    char     last_text[FT8_TEXT_MAX_LEN];
    char     last_grid[FT8_GRID_MAX_LEN];  // remote's grid (empty if unknown)
    int64_t  last_utc;
    int16_t  last_score;
    int16_t  last_snr_db;  // approx SNR = score - 20 (rough calibration)
    int16_t  last_freq;
    uint16_t heard_count;
    bool     occupied;
} ft8_call_t;

void ft8_screen_init(void);

// Called from ft8_task once per successfully decoded message.
void ft8_screen_record_decode(const char *text,
                              int score, int freq_off,
                              int64_t utc_sec);

// Snapshot occupied rows into out[0..max). count_out receives
// number written. Takes mutex internally. Insertion order;
// caller may sort.
void ft8_screen_get_all(ft8_call_t *out, int max, int *count_out);
