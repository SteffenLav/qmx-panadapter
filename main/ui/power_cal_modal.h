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

// Runtime lookup against a PAST sweep's persisted results (settings.h
// pwr_cal), independent of whether this modal has ever been opened this
// session. Returns false (out_v_x10 untouched) if `band` was never
// calibrated, or nothing measured on it came within 3 dB of target_dbm -
// same "never fabricate" rule the modal's own results table follows, so a
// caller wiring this into the WSPR drawer's "Declared power" dropdown can
// never show a different answer than Calibrate Power's own table would.
#include <stdint.h>
#include <stdbool.h>
bool power_cal_voltage_for_dbm(const char *band, int8_t target_dbm, uint16_t *out_v_x10);

#ifdef __cplusplus
}
#endif
