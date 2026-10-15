#pragma once

/* webaudio_codec - the wire format for demodulated audio sent to the browser.
 *
 * Portable C, no ESP-IDF: the firmware and test/webaudio_codec_harness.c link
 * THIS file, so what the harness measures is what the radio sends. See
 * tools/run_harnesses.py for why a harness that mirrors the logic tests the
 * mirror.
 *
 * Two pieces, both small and both able to fail silently, which is why they are
 * measured rather than reviewed:
 *   1. a 4:1 decimator from the 48 kHz output stream down to 12 kHz, and
 *   2. G.711 mu-law, 1 byte per sample.
 */

#include <stdbool.h>
#include <stdint.h>

#define WEBAUD_DECIM    4
/* ⛔ 63 TAPS BECAUSE 31 MEASURED SHORT, not because longer felt safer. At 31
 * the harness read -29.7 dB at 7 kHz and -1.89 dB at 4 kHz: both ends of the
 * same fault, a transition band too wide for the gap between the audio
 * (<= 4 kHz) and the first frequency that aliases (6 kHz). Doubling the length
 * halves the transition. Cost is 63 multiply-accumulates per OUTPUT sample -
 * 12,000 of those a second, not 48,000, because the filter only runs on the
 * samples that are kept. */
#define WEBAUD_FIR_LEN  63
/* The -6 dB point. Sits above the 4 kHz top of the audio and below the 6 kHz
 * Nyquist of the decimated stream; moving it is a trade between the two, and
 * the harness measures both ends. */
#define WEBAUD_FC_HZ    5000

/* ⛔ THE STOPBAND IS LOAD-BEARING. AM at a 20 kHz width carries audio to
 * 10 kHz, well past the 6 kHz Nyquist of the decimated stream - without this
 * filter that content folds back into the passband as a mirror image, and it
 * sounds like interference rather than like a filter fault. Every other mode is
 * already inside 5 kHz by its own passband filter and is unaffected. */
typedef struct {
    float fir[WEBAUD_FIR_LEN];
    float hist[WEBAUD_FIR_LEN];
    int   hi;      /* next write slot = oldest sample */
    int   phase;   /* 0..WEBAUD_DECIM-1 */
} webaud_decim_t;

/* Hamming-windowed sinc, normalised to unity at DC. fc_hz is the -6 dB point. */
void webaud_decim_init(webaud_decim_t *st, int fs_in_hz);

/* Clears the history and the phase, keeping the coefficients. Call when a
 * listener attaches: the tail of the last session is not their audio. */
void webaud_decim_reset(webaud_decim_t *st);

/* Feed one input-rate sample. Returns true on every WEBAUD_DECIM'th call and
 * writes the filtered, clamped output sample to *out. */
bool webaud_decim_push(webaud_decim_t *st, float mono, int16_t *out);

/* G.711 mu-law. Zero-state by construction: a lost frame costs its own
 * duration and corrupts nothing after it - which is the property that matters
 * on this link, measured LOSSY rather than narrow (see webserver_ws.c). */
uint8_t webaud_mulaw_encode(int16_t pcm);
int16_t webaud_mulaw_decode(uint8_t u);
