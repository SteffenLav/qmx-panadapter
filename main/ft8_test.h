// ft8_test.h - Synthetic FT8 round-trip self-test.
// One-shot diagnostic: encode -> synthesize 8-FSK audio -> monitor -> decode.
// Logs PASS/FAIL plus per-stage timing. Called once from app_main at boot.

#ifndef FT8_TEST_H
#define FT8_TEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ft8_self_test(void);

// Returns the clock timing error (ms) measured from the strongest successfully
// decoded FT8 candidate in the last slot that decoded anything. Positive = system
// clock is fast. Returns false if no valid measurement is available yet.
bool ft8_get_last_timing_ms(int *out_ms);

// Increments each time ft8_get_last_timing_ms's value is refreshed from a new
// decode. UI code can poll this to detect a new sync event (e.g. to flash the
// SS box) without needing a callback/notification mechanism.
uint32_t ft8_get_timing_seq(void);

#ifdef __cplusplus
}
#endif

#endif // FT8_TEST_H
