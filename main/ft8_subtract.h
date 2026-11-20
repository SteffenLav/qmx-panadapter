#pragma once

/* Remove a decoded FT8 signal from the slot audio, so a second decode pass
 * can find what was hidden under it.
 *
 * WHY. ft8_decode_bench's sweep (test/ft8_decode_bench.c) showed that
 * FT8_MAX_CANDIDATES, FT8_FIND_MIN_SCORE and FT8_LDPC_MAX_ITERS are all
 * NON-binding: 140 -> 600 candidates moves the decode count not at all while
 * costing 5x the CPU. The messages we miss are not reachable by turning a
 * knob - they are underneath stronger signals. WSJT-X decodes, subtracts what
 * it decoded, and decodes again; ft8_lib has no such pass.
 *
 * MEASURED WORTH, 22 ground-truth reference WAVs, test/ft8_subtract_bench.c:
 *   1 pass   264 unique, 260 of 362 ground truth,  404 ms
 *   2 passes 285 unique, 280 of 362            , 1994 ms   = +21, +8.0 %
 *   3 passes 285 unique, 280 of 362            , 2413 ms   (nothing more)
 * About 90 ms per slot, against FT8_DECODE_BUDGET_MS = 11000.
 *
 * [!] NOT WIRED INTO decode_slot() YET, AND DELIBERATELY SO. Doing that needs
 * three things this file does not provide, each of which is a hazard in the
 * hottest path in the firmware:
 *
 *   1. A PRIVATE COPY OF THE SLOT AUDIO. s_cap_scratch cannot be read during
 *      decode - the capture task owns it and is already filling the next
 *      slot. js8_alt.c's s_alt_audio solves exactly this, at ~720 KB of
 *      PSRAM, and the same copy would serve here.
 *   2. A PRIVATE MONITOR AND PRIVATE STFT SCRATCH for the second pass.
 *      monitor_process() is capture-task-only and strictly sequential because
 *      of s_shared_fft_work / s_shared_stft_time / s_shared_stft_freq (see
 *      ft8_test.c:795). A re-STFT on the decode task MUST NOT touch them.
 *      build_alt_monitor() is the pattern.
 *   3. Budget accounting. The second pass is another ~90 ms per slot and has
 *      to live inside FT8_DECODE_BUDGET_MS with the dual-core worker split.
 *
 * So the algorithm is here, proven on known-correct data by a host harness
 * that links THIS function; the integration is one call site plus those three
 * pieces, and it needs a bench to verify. Nothing half-finished is left in
 * decode_slot().
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Samples per FT8 symbol at 12 kHz: 0.16 s * 12000 = 1920. */
#define FT8_SUB_SPSYM 1920

/* [*] THE SUBTRACTION STARTS ONE SYMBOL BEFORE THE CANDIDATE'S TIME, and the
 * number was measured, not reasoned. ft8_lib's own monitor_resynth() hints at
 * it ("Starting offset is 3 subblocks due to analysis buffer loading"), but
 * the value came from sweeping the offset and watching how much energy the
 * fit takes: on the first decode of 191111_110615 it takes 0.695 of the burst
 * window at -1 symbol against 0.066 at +0, a factor of ten. Before this
 * correction the subtraction removed about 6 % of the signal and the second
 * pass found NOTHING - which reads exactly like "subtract-and-repeat is not
 * worth it". Callers get it right by using ft8_subtract_start_sample(). */
#define FT8_SUB_START_LEAD_SYM 1

/**
 * @brief Sample index at which a decoded signal's waveform begins.
 *
 * @param time_sec  (cand->time_offset + cand->time_sub / time_osr) * symbol_period,
 *                  i.e. ft8_lib's own conversion (demo/decode_ft8.c:154).
 * @param rate_hz   audio sample rate (12000 on this device).
 */
int ft8_subtract_start_sample(float time_sec, int rate_hz);

/**
 * @brief Subtract one decoded FT8 signal from the slot audio, in place.
 *
 * Re-encodes nothing itself: pass the 79 tones from ft8_encode() on the
 * decoded payload. Those symbols are known exactly, because the message
 * survived a 14-bit CRC - which is why re-encoding beats ft8_lib's
 * monitor_resynth(), which rebuilds whatever happened to be in those bins.
 *
 * The fit is in QUADRATURE and PER SYMBOL BLOCK. The synthesised phase will
 * not match the received signal's, and a real-valued scale cannot correct a
 * phase error; a single scale over the whole 12.64 s burst cannot track
 * fading. Both were needed to get the measured yield.
 *
 * Scale-free: the amplitude comes from the data, so it does not matter that
 * s_cap_scratch holds raw int16 magnitudes rather than a +/-1.0 signal.
 *
 * @param sig      slot audio, modified in place
 * @param n_sig    samples in sig
 * @param tones    79 FT8 symbols from ft8_encode()
 * @param n_sym    number of symbols (FT8_NN = 79)
 * @param f0_hz    the signal's audio frequency
 * @param start    from ft8_subtract_start_sample()
 * @param rate_hz  audio sample rate
 * @param scratch  caller-owned, at least ft8_subtract_scratch_floats() floats,
 *                 so this never allocates on the decode path
 *
 * @return the share of the burst window's energy the fitted model accounts
 *         for, 0..1. It is a diagnostic, not a success flag: a strong signal
 *         in a quiet slot takes a large share, a weak one in a crowded slot
 *         takes little even when the fit is perfect, because the denominator
 *         is every signal in the window. Near-zero across ALL decodes means
 *         the frequency or start time is wrong.
 *         [!] An earlier version returned removed/fitted energy, which for a
 *         least-squares fit are equal BY CONSTRUCTION - it printed 1.00 for
 *         every signal whether the alignment was right or hopelessly wrong.
 *         An instrument that cannot fail tells you nothing.
 */
double ft8_subtract_signal(float *sig, int n_sig, const uint8_t *tones,
                           int n_sym, float f0_hz, int start, int rate_hz,
                           float *scratch);

/** Floats the caller must provide as `scratch` for an n_sym-symbol signal. */
int ft8_subtract_scratch_floats(int n_sym);

#ifdef __cplusplus
}
#endif
