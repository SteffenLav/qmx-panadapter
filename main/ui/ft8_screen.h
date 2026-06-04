#pragma once

#include <stdint.h>

// Step 4c.1 v0.10: FT8 decode aggregation.
//
// 4c.1: data layer only. Each decoded message is parsed for its
//       remote callsign (2nd token by FT8 message convention) and
//       merged into a fixed-size table, keyed by that callsign.
//       Repeated decodes of the same station update last_utc /
//       last_score / last_freq / heard_count rather than appending.
//       For now the only output is ESP_LOGI on table update.
//
// 4c.2: LVGL list widget reading the same table.
//
// Single producer (ft8_task on core 1), no readers yet -> no mutex
// for now. The 4c.2 patch will add a mutex when the UI starts
// reading.

void ft8_screen_init(void);

// Called from ft8_task once per successfully decoded message.
//   text     : full decoded message string, e.g. "CQ R8LDJ"
//   score    : decoder score (raw ftx_candidate_t.score)
//   freq_off : audio Hz offset within FT8 passband (0-3000 typ)
//   utc_sec  : slot start time (Unix epoch seconds)
void ft8_screen_record_decode(const char *text,
                              int score, int freq_off,
                              int64_t utc_sec);
