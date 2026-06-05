// ft8_test.h - Synthetic FT8 round-trip self-test.
// One-shot diagnostic: encode -> synthesize 8-FSK audio -> monitor -> decode.
// Logs PASS/FAIL plus per-stage timing. Called once from app_main at boot.

#ifndef FT8_TEST_H
#define FT8_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

void ft8_self_test(void);

#ifdef __cplusplus
}
#endif

#endif // FT8_TEST_H
