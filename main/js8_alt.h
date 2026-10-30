#pragma once

/* T132 - decoding ONE alternate JS8 submode out of the primary slot's capture.
 *
 * ⛔ WHY ANY OF THIS EXISTS. The firmware decodes exactly one JS8 submode at a
 * time, so a station calling on Fast is invisible to a station on Normal.
 * Ryan KK6DZB lost JS8 entirely to that. JS8Call has no such problem - its
 * actionModeMultiDecoder defaults to true.
 *
 * ⛔ DECODING ALL FOUR EVERY SLOT DOES NOT FIT. Measured 2026-10-30 from
 * bench1's own slot log, whole series, 26140 slots: 118% of one core at the
 * median and 193% on a busy band (commit 6ace379c). Two laws out of
 * constants.h: the STFT work per minute is IDENTICAL for every submode (the
 * symbol period cancels in the waterfall-size arithmetic), so four submodes is
 * a flat x4 with no knob; the LDPC half scales with slots per minute at the
 * saturated 140-candidate cap, so x5.5.
 *
 * So one alternate submode, on a duty cycle, out of the audio the PRIMARY slot
 * already captured. There is no second capture: s_cap_scratch holds exactly
 * one slot and the next slot overwrites it.
 *
 * ⭐ THIS FILE IS PURE ARITHMETIC AND NOTHING ELSE - no ESP-IDF, no settings,
 * no monitors - so test/js8_alt_select_harness.c links the real functions
 * rather than a copy of them. The alignment is the part that can quietly be
 * wrong: feeding a monitor from the wrong offset produces no error of any
 * kind, it produces SILENCE, which is indistinguishable from a quiet band.
 * test/js8_alt_window_harness.c proved the window arithmetic against the real
 * decoder before any of it was written into the firmware.
 *
 * ⚠ RECEIVE ONLY. An alternate decode must never reach ft8_qso_advance(), the
 * robot or TX arming: the station is operating on the PRIMARY submode, and a
 * Fast decode advancing a Normal exchange could key the radio against the
 * wrong state. ft8_test.c enforces that with an explicit flag, not by hoping.
 *
 * ⚠ NOT PROVEN ON AIR. Fast, Turbo and Slow have never been exchanged with a
 * real JS8Call station - only our own encoder and decoder agree. Same
 * disclaimer js8_submode.h already carries, for the same reason.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ft8/constants.h"

/* Decoder sample rate. One number, and it is the decoder's, not the codec's. */
#define JS8_ALT_SR_HZ   12000

/* ⭐ ONE ALTERNATE WINDOW EVERY JS8_ALT_DUTY_EVERY PRIMARY SLOTS, and the two
 * reasons for 5 rather than 4 are a budget and a phase.
 *
 * THE BUDGET. From the measured per-slot medians (6ace379c): JS8 Normal costs
 * stft 1872 ms + dec 1858 ms per 15 s slot = 24.9% of one core today. One
 * Fast window inside a Normal slot costs stft 10/15 x 1872 = 1248 ms plus a
 * full saturated decode of 1858 ms = 3106 ms. Run on every slot that is
 * 3106/15000 = 20.7% added; at one slot in 5 it is 4.1% added, for about 29%
 * of one core in total. That is comfortably under the 48% the T132 row
 * promised - the row's figure costed the alternate at its OWN slots per
 * minute (840 Fast candidates), and we can only ever decode the windows that
 * fit inside a slot we captured, which is 4 per minute, not 6.
 *
 * ⛔ THE PHASE, which is why it may NOT be 4. A primary Normal slot is 15 s
 * and a minute holds exactly 4 of them, so `slot_number % 4` is the phase of
 * the minute and never moves. The alternate window offset depends on that
 * phase, so a duty of 1-in-4 would look at the SAME Fast window for ever and
 * three quarters of the Fast traffic would be permanently unreachable. 5 is
 * coprime with 4, so the visited phase walks 0,1,2,3,0,... and every reachable
 * window is covered in 20 primary slots (5 minutes).
 *
 * ⚠ Coverage is capped by arithmetic, not by the duty cycle. A Fast slot that
 * STRADDLES a Normal boundary is in neither capture: the Fast slots at :10 and
 * :40 can never be decoded this way, so Fast coverage is 4 of 6 per minute
 * whatever this number is. js8_alt_select_harness.c asserts exactly that, so
 * nobody re-derives it from intuition. */
#define JS8_ALT_DUTY_EVERY   5

/* Most complete alternate windows one primary slot can hold. Turbo (6 s) in a
 * Slow (30 s) primary is 5; Turbo in Normal is 2; Fast in Normal is 1. */
#define JS8_ALT_MAX_WINDOWS  6

/* How many complete `alt_s`-second slots lie inside the primary slot that
 * starts at UTC second `slot_sec`, and where.
 *
 * ⛔ PORTED FROM alt_slots_in_primary() IN test/js8_alt_window_harness.c, which
 * is the reference implementation and proved itself against the real decoder.
 * Do not reinvent it. Every submode period divides 60 s, but they do not line
 * up with each other inside a minute.
 *
 * ⚠ I first wrote the Fast expectation as 0 complete slots at t0=15 and t0=45,
 * reasoning that the Fast slots there are [10,20) and [20,30) and neither is
 * inside [15,30). The second half of that is false - [20,30) IS inside
 * [15,30) - so Fast fits in EVERY primary slot. The wrong version is the
 * intuitive one, which is why it is written down rather than quietly fixed.
 *
 * `offs_samples`, when given, receives each window's offset into the capture
 * buffer in SAMPLES at JS8_ALT_SR_HZ. Returns the count, which may exceed
 * `max_offs` (only `max_offs` offsets are written). */
int js8_alt_slots_in_primary(int64_t slot_sec, int primary_s, int alt_s,
                             int *offs_samples, int max_offs);

/* True when `alt` can be decoded out of a `primary` capture at all - it is a
 * different JS8 submode and at least one of its slots fits. Slow (30 s) inside
 * a 15 s primary is excluded HERE, by arithmetic, not deferred. */
bool js8_alt_fits(ftx_protocol_t primary, ftx_protocol_t alt);

/* The duty cycle: is the primary slot starting at `slot_sec` an alternate
 * slot? Pure function of the UTC slot number, so it survives a task restart
 * and a host test can ask it about any slot. */
bool js8_alt_slot_due(int64_t slot_sec, int primary_s);

/* Everything the capture task needs for one slot, in one call.
 *
 * `alt_submode` is the JS8Call NSUBMODE value the operator chose (0 Normal,
 * 1 Fast, 2 Turbo, 4 Slow), or JS8_ALT_OFF. Returns false - and writes
 * nothing - when the feature is off, the primary is not JS8, this is not a
 * duty slot, the choice equals the primary, or no complete window fits.
 *
 * On true, `*alt` is the alternate protocol and `offs_samples[0..n)` are the
 * windows to feed, n being the return of js8_alt_slots_in_primary(). */
#define JS8_ALT_OFF  255

bool js8_alt_select(ftx_protocol_t primary, uint8_t alt_submode,
                    int64_t slot_sec,
                    ftx_protocol_t *alt, int *offs_samples, int max_offs,
                    int *n_windows);

/* The one-character mark the JS8 page puts in front of a message that was
 * heard on something other than the submode we are operating. ' ' for the
 * primary, so the column is blank on an ordinary session. */
char js8_alt_mark(ftx_protocol_t proto, ftx_protocol_t primary);

/* Map an NSUBMODE value to a protocol. Shared with js8_submode.c's table by
 * value, not by index - see the note there about the numbering not being
 * dense. Returns FTX_PROTOCOL_JS8 for anything unknown. */
ftx_protocol_t js8_alt_proto_for_submode(uint8_t nsubmode);
