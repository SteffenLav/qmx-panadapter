#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the Calibrate Power modal at boot - same fragmentation-cliff
// rationale as tune_modal_init(). Hidden until power_cal_modal_show().
void power_cal_modal_init(void);

// Show the Calibrate Power modal as a full-screen overlay. Caller (the
// drawer's "Calibrate Power" button) closes the settings drawer first.
//
// Sweeps Max. PA voltage through 5 fixed test points (6.0/7.5/9.0/10.5/12.0 V)
// on the CURRENT band, keying QMX SWR Tune mode (MD8;, 1_04+ firmware only -
// same requirement as Antenna Tune) briefly at each point, and records the
// measured PC; output. Needs a DUMMY LOAD, not the antenna - the modal's own
// warning text says so, and the button lives right beside Antenna Tune for
// exactly the opposite reason that one does.
void power_cal_modal_show(void);

#ifdef __cplusplus
}
#endif
