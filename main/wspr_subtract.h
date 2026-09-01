/* Remove a decoded WSPR transmission from a capture, so a second pass can find
 * what was hiding under it.
 *
 * WHY: measured on the three 2026-08-24 reference windows, 9 of the 32 stations
 * wsprd found sit within 6 Hz of another, and our comb search yields roughly
 * ONE candidate per 4-6 Hz cluster - so at most one of each is reachable, and
 * sometimes NEITHER is. At 19:10 `DK8AF` (-16 dB) and `DD3MS` (-13 dB) are 4 Hz
 * apart and we decode neither, because the single candidate between them sees a
 * mixture rather than either signal. Subtracting the one we CAN decode is what
 * makes its neighbour reachable, and it is what wsprd does.
 *
 * Portable, no ESP dependencies, so test/wspr_cap_sweep.c links the real thing.
 */
#ifndef WSPR_SUBTRACT_H
#define WSPR_SUBTRACT_H

#include <stdint.h>
#include "wspr_fano.h"     /* WSPR_NSYM */

/* Re-derive the 162 transmitted tones (0..3) from a decoded message. Returns 0
 * if the callsign/grid/power will not re-pack - which would mean the decode was
 * never self-consistent in the first place. */
int wspr_tones_from_message(const char *callsign, const char *grid,
                            int power_dbm, uint8_t tones_out[WSPR_NSYM]);

/* Subtract the transmission described by (f0_hz, dt_samples, tones) from
 * `samples` IN PLACE.
 *
 * f0_hz is the TONE-0 frequency, i.e. exactly what wspr_decode_result_t.freq_hz
 * holds - not the signal centre. (wsprd reports the centre, 1.5 tone spacings
 * higher; that difference is a convention, not an error, and mixing the two up
 * would subtract 2.2 Hz away from the signal.)
 *
 * dt_samples is in ORIGINAL 12 kHz samples, as the decode result reports it.
 *
 * The amplitude and phase of each symbol are fitted from the data rather than
 * assumed, so real fading within the transmission is removed as it actually
 * occurred. Symbols falling outside the capture are skipped.
 *
 * Returns the number of symbols actually subtracted. */
int wspr_subtract(int16_t *samples, long n, double f0_hz, long dt_samples,
                  const uint8_t tones[WSPR_NSYM]);

#endif /* WSPR_SUBTRACT_H */
