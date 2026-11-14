#pragma once
#include <stdbool.h>

// "Use the contacts on the card as your log history?"
//
// Raised when a qso-archive.adi is on the card that this log does not own -
// the operator merged their master ADIF on the PC and copied it across. See
// adoption_check() in main/storage/sd_archive.c for why this is an offer and
// never automatic: the same state is produced by another station's card and
// by a log the operator has just cleared.
//
// The window states the count and nothing about where the contacts came from.
// The firmware cannot know whether this radio logged them before a flash or a
// clear, so it must not say "never logged" (operator, 2026-11-14).

// Call from the 1 Hz LVGL-thread tick. Opens the question when the archive
// task raises the offer and nothing else is on screen; closes it once the
// offer is answered or withdrawn (the card was pulled).
void archive_adopt_modal_tick(void);

bool archive_adopt_modal_is_open(void);
