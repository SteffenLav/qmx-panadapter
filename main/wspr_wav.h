/* Canonical WAV header for a WSPR capture dump.
 *
 * WHY THIS IS ITS OWN FILE
 *   The 44-byte header is pure byte-layout arithmetic with four size fields
 *   that must agree with each other and with the payload. Getting one wrong
 *   does not crash anything - it produces a file that wsprd silently refuses
 *   or, worse, reads with the wrong length and decodes garbage. That is
 *   exactly the class of bug this project host-tests rather than discovers on
 *   hardware, and here there is an EXTERNAL vector available: WSJT-X's own
 *   reference recordings in test/wav_reference/, whose headers were written by
 *   somebody else's code. See test/wspr_wav_harness.c.
 *
 * No ESP dependencies on purpose, so the harness links the real function.
 */
#ifndef WSPR_WAV_H
#define WSPR_WAV_H

#include <stdint.h>
#include <stddef.h>

#define WSPR_WAV_HDR_BYTES 44

/* Fill `hdr` with a 44-byte canonical PCM RIFF/WAVE header for `nsamples`
 * mono 16-bit samples at `rate` Hz. Returns WSPR_WAV_HDR_BYTES, or 0 if hdr is
 * NULL or nsamples/rate is 0 - a zero-length WAV is never what the caller
 * meant, and writing one would look like a successful dump. */
size_t wspr_wav_header(uint8_t *hdr, uint32_t nsamples, uint32_t rate);

/* The filename wsprd and WSJT-X expect for a 2-minute WSPR window:
 * YYMMDD_HHMM.wav, built from a UTC epoch. Naming dumps this way means the PC
 * side needs no renaming step - wsprd can be pointed straight at the file.
 * Writes at most n bytes including the NUL; returns the length written. */
size_t wspr_wav_filename(char *out, size_t n, int64_t utc_epoch);

#endif /* WSPR_WAV_H */
