// ft8_test.h - Synthetic FT8 round-trip self-test.
// One-shot diagnostic: encode -> synthesize 8-FSK audio -> monitor -> decode.
// Logs PASS/FAIL plus per-stage timing. Called once from app_main at boot.

#ifndef FT8_TEST_H
#define FT8_TEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ft8_self_test(void);

// Returns the clock timing error (ms) measured from the strongest FT8 candidate
// in the last successfully decoded slot. Positive = system clock is fast.
// Returns false if no valid measurement is available yet.
bool ft8_get_last_timing_ms(int *out_ms);

#ifdef __cplusplus
}
#endif

#endif // FT8_TEST_H
