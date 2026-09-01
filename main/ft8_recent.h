// ft8_recent - "did we work this station on this band, recently?"
//
// Gyula HA3HZ, testing auto-answer CQ on v1.9.6 (2026-08-28):
//
//   "When I finish a QSO and his callsign turns gray, he calls again shortly
//    after - as if there was no previous completed QSO."
//
// The decode list greys a callsign whenever adif_log_contains_call_on_band()
// says it is already logged - with no condition attached. Every ENGINE picker,
// though, skipped that station only `if (excl_worked_before)`. So on the default
// the screen said "worked" in dim grey and the machine called it again. BD4AHS
// reported the identical behaviour on 2026-08-06 and was told to tick the box;
// two independent reports of one behaviour is evidence about the DEFAULT, not
// about two operators misconfiguring the same thing.
//
// The fix is bounded in TIME rather than absolute, because re-working a station
// later in the day is perfectly legitimate: the unattended pickers refuse a
// station worked on this band within FT8_RECENT_GRACE_SEC whatever the checkbox
// says, and beyond that window the checkbox decides exactly as before.
//
// ⭐ There is a second, harder reason it has to be unconditional. ft8_qso.c
// REFUSES TO LOG a repeat contact with the same call on the same band inside
// QSO_DUP_LOG_WINDOW_SEC. So without this the machine transmits a complete
// exchange - about 12.6 s of key-down per message - and then discards the
// result. Working a contact we have already decided not to log is worse than
// declining to work it.
//
// ⚠ RAM-only, deliberately. It answers "recently, in this session", which is
// exactly the reported symptom; everything older is still covered by the ADIF
// log via the checkbox. A reboot clearing it is harmless when the window is
// minutes.
//
// Portable: no ESP-IDF dependencies and the clock is a PARAMETER, so
// test/ft8_recent_harness.c links these very functions instead of a copy, and
// can step time without waiting for it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// 16 entries because one slot is useless the moment another station is worked in
// between - which is why ft8_qso.c's single-entry duplicate guard could not be
// reused for this.
#define FT8_RECENT_MAX        16
#define FT8_RECENT_GRACE_SEC  (30 * 60)

// Below this the system clock has not been set, and a stored timestamp of 0
// would otherwise read as "worked 56 years ago" - or, with a wrong sign, as
// "worked in the future", which would refuse every station for ever.
#define FT8_RECENT_EPOCH_MIN  1700000000LL

void ft8_recent_reset(void);

// Record a completed contact. Re-noting a call/band that is already held
// REFRESHES it rather than consuming a second slot.
void ft8_recent_note(const char *call, const char *band, int64_t now);

// True only for a call+band held with an age in [0, FT8_RECENT_GRACE_SEC).
// False whenever `now` is not a plausible epoch, so an unset clock can never
// turn this into a permanent refusal.
bool ft8_recent_worked(const char *call, const char *band, int64_t now);
