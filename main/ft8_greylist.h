#pragma once
#include <stdbool.h>

// FT8 grey-list: stations that repeatedly failed to complete a pounce
// (no response before the QSO timeout) are temporarily set aside so the
// automatic pickers (robot auto-answer, auto-work pileup) stop burning
// cycles re-calling a station that can't hear us (Roy KI0ER field report:
// the robot re-tried the same deaf station all night). RAM-only - the list
// resets on reboot, which is the "temporary" in temporary black-list.
//
// The feature is gated by the "Allow grey-listing" setting (greylist_en,
// Filter modal); the callers gate on it - this module just keeps the table.
// A grey-listed station's decode-list rows render in a distinct colour, and
// tapping one offers "Clear from grey-list" instead of the TX modal.

// A pounce at `call` timed out. Two timeouts (each already = several
// re-sent transmissions) grey-lists the station.
void ft8_greylist_note_timeout(const char *call);

// True if the station is currently grey-listed.
bool ft8_greylist_contains(const char *call);

// Remove one station (the operator's "Clear from grey-list" action) /
// wipe the whole list.
void ft8_greylist_clear(const char *call);
void ft8_greylist_clear_all(void);
