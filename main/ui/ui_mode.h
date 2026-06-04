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

typedef enum {
    UI_MODE_PANADAPTER = 0,
    UI_MODE_FT8        = 1,
} ui_mode_t;

ui_mode_t ui_mode_get(void);
void      ui_mode_set(ui_mode_t mode);
