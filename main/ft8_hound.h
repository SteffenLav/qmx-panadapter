#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ui/ft8_screen.h"   // ft8_call_t

// FT8 Fox/Hound (DXpedition) mode - the HOUND side.
//
// WHY ONLY HOUND. A Fox transmits up to FIVE signals at once. We emit one CAT
// "TA<freq>;" per symbol and the QMX synthesises that single tone, so a
// composite multi-tone waveform is not something this radio can be asked for.
// Fox is a DXpedition role anyway. It is not "unimplemented", it is impossible
// here - don't come back to it.
//
// WHAT A HOUND DOES, and where each step lives:
//
//   1. Call the Fox from ABOVE 1000 Hz, well clear of the Fox's own region.
//      hound_pick_tx_tone() chooses a free slot there.
//   2. The Fox answers "<us> <fox> <report>". Ordinary FT8, so the existing
//      WAIT_RPT handler in ft8_qso.c already recognises it.
//   3. QSY DOWN onto the Fox's own frequency for "<fox> <us> R<report>". This is
//      the one genuinely new mechanic, and the reason F/H exists: the Fox listens
//      only to its own narrow slice, so a hound that stays up-band is never heard
//      again.
//   4. The Fox's RR73 ENDS the QSO. A hound does not send 73 - the Fox's
//      frequency is precious and a 73 there is pure clutter.
//   5. Return to the hound tone for the next Fox.
//
// THREE OF OUR POLITENESS RULES MUST STAND DOWN, and this is the real reason
// Hound is a mode rather than a tweak (see ft8_qso.c for where each is gated):
//
//   * the busy-station hold - a Fox is ALWAYS working somebody, so holding TX
//     until it is free means never calling at all;
//   * the final re-send - the Fox never asks again, and re-sending into a
//     pileup is exactly the noise F/H was invented to avoid;
//   * the grey-list - being ignored by a Fox for many slots is the normal
//     experience of a pileup, not a station that never answers.
//
// Everything here is pure decision logic over a decode snapshot: no LVGL, no
// CAT, no state of its own. That keeps it testable and keeps the protocol rules
// in one file instead of smeared across the QSO machine.

typedef enum {
    FT8_HOUND_OFF = 0,
    FT8_HOUND_GUIDED,      // hound rules apply; the operator starts each contact
    FT8_HOUND_AUTO,        // hound rules apply; the machine may start one itself
} ft8_hound_mode_t;

// Current mode, read from settings (cheap - settings_load_all is a staged copy).
ft8_hound_mode_t ft8_hound_mode(void);

// True when Hound is enabled at all (guided or automatic).
static inline bool ft8_hound_enabled(ft8_hound_mode_t m) { return m != FT8_HOUND_OFF; }

// The Fox region: a Fox transmits below this, hounds above it. 1000 Hz is the
// WSJT-X convention and DXpeditions publish it, so it is not ours to choose.
#define FT8_HOUND_FOX_MAX_HZ   1000
// Lowest tone we will call a Fox from. Above the Fox region with a margin, so a
// hound never lands on the Fox's own slice by rounding.
#define FT8_HOUND_TX_MIN_HZ    1100

// Feed the tick's decode snapshot in so the queue history stays current. Must be
// called before ft8_hound_looks_like_fox() can answer true for anybody - the
// decode table keeps only each station's last message, so "has it worked several
// different stations lately" has to be accumulated over time, and that lives in
// ft8_hound.c.
void ft8_hound_observe(const ft8_call_t *list, int n, int64_t now);

// Does this decoded station look like a Fox? Conservative on purpose: a false
// positive means transmitting at an ordinary station as though it were a
// DXpedition, and QSY'ing onto its frequency to do it.
//
// Three things together, and the third is the one that matters: it sits in the
// Fox region, its message is Fox-shaped (a CQ from down there or a report to
// somebody), and it has been seen WORKING A QUEUE - several different callsigns
// inside a few minutes. Being low in the passband and calling CQ is NOT enough;
// that rule identified an ordinary phantom at 700 Hz as a Fox on the bench and
// called it.
bool ft8_hound_looks_like_fox(const ft8_call_t *c);

// Scan a decode snapshot for the most promising Fox, or NULL. `n` entries.
// Strongest first, since in a real pileup the loudest Fox in the Fox region is
// the one being called.
//
// slot_sec != 0 restricts the answer to Foxes heard in THAT slot. Callers about
// to transmit must pass it: our reply goes in the following slot, so a Fox last
// heard two slots ago would have us calling in the same window it transmits in -
// which is both useless and rude, since it is deaf while it transmits.
const ft8_call_t *ft8_hound_find_fox(const ft8_call_t *list, int n, int64_t slot_sec);

// Once per RX slot, from the decode task (the same place and thread
// ft8_robot_tick() starts contacts from). No-op when the mode is off or a contact
// is already running.
//
//   GUIDED    says a Fox is there and what tapping it will do, and stops. Every
//             transmission stays the operator's decision.
//   AUTOMATIC does the same and then calls it.
//
// Never calls a Fox already worked on this band - a DXpedition dupe is worth
// nothing to either station, and without that check automatic mode would work the
// same Fox for ever.
void ft8_hound_tick(int64_t slot_sec);

// Pick our calling tone: a free 50 Hz slot at or above FT8_HOUND_TX_MIN_HZ,
// chosen from the same live occupancy mask the ordinary clear-slot picker uses -
// which beats WSJT-X's random pick, since random is how hounds end up stacked on
// each other. Falls back to a spread-out default if nothing is known yet.
int ft8_hound_pick_tx_tone(void);
