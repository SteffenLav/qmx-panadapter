#pragma once

#include <stdbool.h>
#include <stdint.h>

// Suppression of the QMX's own synthesizer spurs, by measurement rather than
// inference.
//
// WHAT THE SPURS ARE (measured on OZ1LAV's unit, 2026-08-13, BNC open):
// at some dial frequencies the radio puts a strong artifact into the IQ stream
// - up to 41 dB over the noise floor, with a 2nd harmonic and a mirror image.
// They are self-generated: present with no antenna, and they vanish rather than
// translate when the dial moves, so they are neither off-air nor anything our
// DSP creates.
//
// THE KEY PROPERTY, and the whole basis of this module: a spur's baseband
// offset moves at 16-50x the dial (16.0 and 23.0 measured solidly over several
// intervals). So a tiny nudge of the dial moves a spur by many FFT bins while a
// real signal stays in its own bin:
//
//     25 Hz nudge  ->  spur moves 400-1250 Hz  (8.5-27 bins)
//                  ->  real signal moves 25 Hz (0.53 bin)
//
// That is a physical discriminator with an order of magnitude of margin, not a
// statistical guess. An earlier attempt classified bins by how CONSTANT they
// were; a host harness rejected it for eating 30 dB of a QSB-fading carrier,
// which is the failure mode the operator explicitly refused.
//
// WHAT IS DONE WITH IT: the measured spur POWER is subtracted from the affected
// bins in the linear domain, so a real signal that happens to share a bin keeps
// its own power and loses only the spur's contribution. Nothing is blanked, so
// no signal of any size can be rejected.
//
// COST: the nudge is audible-ish (a 25 Hz shift for under a second). So results
// are CACHED per dial frequency - returning to a frequency already learned
// costs nothing at all, which matters because FT8 sits on one frequency for
// hours.
//
// NOT applied to the zoom-FFT path yet, and detection is skipped while RIT is
// engaged (cat_set_frequency clears RIT, so dithering would wipe the operator's
// offset) or while a TX is armed or running.

#ifdef __cplusplus
extern "C" {
#endif

// Each tooth of the comb is ~10 bins wide once its skirts are counted, and
// there are typically 4+ teeth (fundamental, 2nd harmonic, and both mirror
// images), so this has to be generous. 24 was not: the array filled with weak
// skirt bins at low indices and never reached the strongest spur, which sits at
// a NEGATIVE offset and therefore a high bin index.
// 96, not 64: a full comb is ~60 bins once every tooth's skirt is counted, and
// the DC cluster adds up to 17 more. At 64 the comb consumed every slot and the
// DC bins - added afterwards - silently got none, leaving the one spur the
// operator cares about most fully visible.
#define SPUR_MAP_MAX_ENTRIES 96

// Deep cancellation is an accuracy problem, not an averaging one: removing 3260
// of 3547 power units - an error of 0.37 dB - still leaves a residual only 10.9
// dB down. Roughly 2.4 dB of error buys 10 dB of cancellation, 0.9 dB buys 20,
// 0.14 dB buys 30. The spur's own level wobbles by a few tenths, so no amount of
// averaging at detection time reaches a deep null; the estimate has to be
// trimmed against the residual it actually leaves.
//
// The trim is clamped to +/-1 dB of the detected power, and that clamp IS the
// safety property: if a real signal ever lands on a mapped bin the servo will
// try to null that too, and the bound means the most it can lose is the spur's
// power plus 1 dB. The operator accepted that bound on the reasoning that
// tuning up and down a little makes any hidden signal reappear anyway.
// Measured 2026-08-13: with the bound temporarily opened to 6 dB the servo
// still settled at -0.11 dB of trim, so 1 dB costs nothing in practice - the
// residual is set by the spur's own level wobble, not by this clamp.
#define SPUR_SERVO_MAX_TRIM_DB 1.0f

// Start the detection task. Safe to call with the feature switched off - the
// task idles until it is enabled.
void spur_map_init(void);

// Subtract the currently published spur powers from a linear-power spectrum,
// in place. Called on the FFT hot path, so it is a no-op returning immediately
// when the feature is off or nothing has been learned for this frequency.
void spur_map_apply(float *mag2, int n_bins);

// 0 = off, 1 = subtract the measured power (never hides a real signal - one
// sharing a spur bin loses only the spur's power +/-1 dB, but the spur stays
// faintly visible because its own level wobbles ~0.5 dB and a constant cannot
// cancel a fluctuating quantity), 2 = interpolate the mapped bins away from
// their neighbours (spurs vanish completely; a real signal inside a mapped
// cluster is hidden while the dial sits still). The operator's argument for
// allowing 2: the spurs move 16-50x faster than the dial, so nudging the VFO
// slides the blind spots straight off anything real.
typedef enum {
    SPUR_MODE_OFF         = 0,
    SPUR_MODE_SUBTRACT    = 1,
    SPUR_MODE_INTERPOLATE = 2,
} spur_mode_t;

spur_mode_t spur_map_get_mode(void);
void spur_map_set_mode(spur_mode_t mode);
bool spur_map_is_enabled(void);

// Bins currently being suppressed, for showing the operator what is touched.
// Returns the number written to `bins` (at most `max`).
int spur_map_get_marks(uint16_t *bins, int max);

// True while a detection nudge is actually in progress - the UI can say so
// rather than leaving an unexplained wobble.
bool spur_map_is_measuring(void);

// Drop everything learned. For a band change, or an operator "re-learn".
void spur_map_forget_all(void);

#ifdef __cplusplus
}
#endif
