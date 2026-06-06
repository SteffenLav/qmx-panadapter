#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


// Reset all running estimates. Useful after disabling and re-enabling,
// or after a band/mode change where the QMX I/Q characteristics may shift.
void iq_balance_reset(void);

// Enable or disable correction. When disabled, apply() is a no-op
// and the running estimates freeze (do not update).
void iq_balance_set_enabled(bool on);
void iq_balance_init(bool enabled);
bool iq_balance_is_enabled(void);

// Process one I/Q sample pair in place. i_inout/q_inout are int16
// samples at the post-decode stage (after the QMX 24->16 bit shift in
// audio.c). When disabled, returns immediately without touching the
// samples or the running estimates.
void iq_balance_apply(int16_t *i_inout, int16_t *q_inout);

#ifdef __cplusplus
}
#endif