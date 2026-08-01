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
    // CW page (v1.4): the panadapter pipeline KEEPS RUNNING — every consumer
    // that gates on == UI_MODE_FT8 (dsp, render, ft8_task) treats CW exactly
    // like PANADAPTER, which is the intended behaviour: spectrum + waterfall
    // stay live as the CW page's backdrop, only LVGL overlays differ.
    UI_MODE_CW         = 2,
} ui_mode_t;

ui_mode_t ui_mode_get(void);
void      ui_mode_set(ui_mode_t mode);
