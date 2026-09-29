#pragma once
#include <stdbool.h>

// "Is today's date right?" - asked when the date could not be verified
// (time_sync_date_verified() is false: the Tab5 RTC ran down while it was off,
// and there is no internet). Don WB0LQW, 2026-09-13: his POTA log came out two
// days behind because the Tab5 had been off for two days.
//
// Never blocks logging: an unanswered question costs a possibly wrong date,
// which is what happened before; a blocked log would cost the QSO.

// Call from a 1 Hz LVGL-thread timer. Opens the question once per boot when the
// date is unverified, the unit has been up long enough for SNTP to have had its
// chance, and nothing else is on screen. Closes it by itself if SNTP arrives.
void date_confirm_modal_tick(void);

// Open it now (the Set the Clock window's date line). LVGL thread only.
void date_confirm_modal_show(void);

bool date_confirm_modal_is_open(void);
