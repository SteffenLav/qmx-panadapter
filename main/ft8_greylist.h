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

/* The operator skipped this station BY HAND - the "Skip exchange" button
 * (Randy N4OPI, 2026-10-21: a station calling "CQ DX" that was never looking
 * for us, or one that keeps calling CQ while we call it because it cannot hear
 * our QRP signal). Lists it AT ONCE rather than after GREY_FAIL_LIMIT
 * timeouts: the operator has already decided.
 *
 * ⛔ It is marked operator-forced, and the pickers honour a forced entry EVEN
 * WHEN "Allow grey-listing" IS OFF. A hand skip is an instruction, not a
 * heuristic - without this, skipping with the setting off would abandon the
 * exchange and then let the robot re-answer the same CQ on the next slot,
 * which is the whole complaint.
 *
 * Still RAM-only and still cleared by "Clear from grey-list", so it is
 * reversible and never outlives a reboot. */
void ft8_greylist_skip_now(const char *call);

// True if the station was skipped by hand. NOT subject to greylist_en - see
// ft8_greylist_skip_now().
bool ft8_greylist_skipped_by_hand(const char *call);

// Remove one station (the operator's "Clear from grey-list" action) /
// wipe the whole list.
void ft8_greylist_clear(const char *call);
void ft8_greylist_clear_all(void);

// Snapshot the calls currently being skipped (fail count at the limit), for the
// web viewer. Returns the number written to out[0..max).
int ft8_greylist_get_all(char out[][12], int max);
