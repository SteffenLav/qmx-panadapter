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
// Sweeps Max. PA voltage through PWRCAL_STEPS test points (settings.h -
// 45 as of 2026-09-15, 1.0-12.0 V averaging 0.25 V) on the CURRENT band,
// forcing DiGi mode and keying a real TX;/TA<freq>;/RX; tone at each point
// (the same primitives a WSPR/FT8 burst is built from - see the .c file's
// own header for why the first two designs, QMX SWR Tune and a bare CW
// TX;, both measured the wrong thing), and records the measured PC;
// output. Needs a DUMMY LOAD, not the antenna - the modal's own warning
// text says so, and the button lives right beside Antenna Tune for exactly
// the opposite reason that one does. Stops WSPR receive for the duration
// if it was running, and resumes it when the window closes.
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

// For a general (non-WSPR) "Output power" control - every DISTINCT
// wattage Calibrate Power actually measured on `band`, ascending, each
// paired with the lowest voltage that produces it. See the .c file's own
// header for why "distinct" and "lowest" both matter. Returns the count
// (0 = not calibrated); out_w_x100/out_v_x10 must each hold at least
// PWRCAL_STEPS entries, bounded by max_n.
int power_cal_list_watts(const char *band, uint16_t *out_w_x100, uint16_t *out_v_x10, int max_n);

#ifdef __cplusplus
}
#endif
