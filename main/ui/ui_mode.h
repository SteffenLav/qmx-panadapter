#pragma once

// Step 4b v0.10: high-level UI/operating mode.
//
// Read by fft_task (dsp.c) and ft8_task (ft8_test.c) to decide
// what to do with each block of audio samples:
//   - UI_MODE_PANADAPTER: DC blocker -> FFT -> spectrum push.
//   - UI_MODE_FT8: drain the ring (still single-consumer!) and
//     either feed the FT8 mixer/decimator when a capture is
//     armed, or discard between slots.
//
// Single volatile enum, no mutex - all writers are LVGL touch
// callbacks or boot code; all readers are tasks polling at
// FFT cadence or slower. No torn reads on a 4-byte enum.

#include <stdbool.h>

typedef enum {
    UI_MODE_PANADAPTER = 0,
    UI_MODE_FT8        = 1,
    /* 2 is deliberately skipped: the CW page branch (feat/cw-page) already
     * owns UI_MODE_CW = 2, and the two feature branches will eventually meet.
     * Taking 3 here costs nothing and avoids a merge that silently makes two
     * different screens the same number. */
    UI_MODE_WSPR       = 3,
} ui_mode_t;

/* True for the modes that need the FT8-style capture pipeline: the +12 kHz IF
 * mixed to DC, decimated /4 to 12 kHz mono, and appended to the continuous
 * pre-ring. WSPR wants exactly that chain - only the window length and what
 * decodes it differ - so this is shared rather than duplicated. */
static inline bool ui_mode_uses_iq_capture(ui_mode_t m)
{
    return m == UI_MODE_FT8 || m == UI_MODE_WSPR;
}

ui_mode_t ui_mode_get(void);
void      ui_mode_set(ui_mode_t mode);
