#ifndef _INCLUDE_MONITOR_H_
#define _INCLUDE_MONITOR_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <ft8/decode.h>
#include <fft/kiss_fftr.h>

/// Configuration options for FT4/FT8 monitor
typedef struct
{
    float f_min;             ///< Lower frequency bound for analysis
    float f_max;             ///< Upper frequency bound for analysis
    int sample_rate;         ///< Sample rate in Hertz
    int time_osr;            ///< Number of time subdivisions
    int freq_osr;            ///< Number of frequency subdivisions
    ftx_protocol_t protocol; ///< Protocol: FT4 or FT8
} monitor_config_t;

/// FT4/FT8 monitor object that manages DSP processing of incoming audio data
/// and prepares a waterfall object
typedef struct
{
    float symbol_period; ///< FT4/FT8 symbol period in seconds
    int min_bin;         ///< First FFT bin in the frequency range (begin)
    int max_bin;         ///< First FFT bin outside the frequency range (end)
    int block_size;      ///< Number of samples per symbol (block)
    int subblock_size;   ///< Analysis shift size (number of samples)
    int nfft;            ///< FFT size
    float fft_norm;      ///< FFT normalization factor
    float* window;       ///< Window function for STFT analysis (nfft samples)
    float* last_frame;   ///< Current STFT analysis frame (nfft samples)
    /* ⛔ STFT SCRATCH, AND IT USED TO BE TWO VARIABLE-LENGTH ARRAYS ON THE
     * STACK inside monitor_process():
     *
     *     kiss_fft_scalar timedata[me->nfft];
     *     kiss_fft_cpx    freqdata[me->nfft / 2 + 1];
     *
     * At FT8's nfft of 3840 that is 15 360 + 15 368 = ~30 KB of a 64 KB task
     * stack, which worked and left no sign that it was a problem. JS8 Slow
     * doubles nfft to 7680 and the pair becomes ~61 KB - the `ft8` task took a
     * Stack protection fault on its first Slow capture, 2026-10-14, SP 4112
     * bytes below the stack bottom.
     *
     * The fix is not a bigger stack: the cost SCALED WITH THE PROTOCOL, so a
     * stack large enough for Slow is one that silently waits for the next
     * longer symbol period. Off the stack entirely, allocated once, and the
     * stack cost of monitor_process stops depending on nfft at all.
     *
     * On the device both are shared across the whole pool and point at one
     * allocation - monitor_process runs on the capture task only, one monitor
     * at a time (see s_shared_fft_work in ft8_test.c, same lifecycle, same
     * reasoning). A standalone monitor_init() owns its own pair. */
    kiss_fft_scalar* timedata; ///< STFT input scratch (nfft samples)
    kiss_fft_cpx*    freqdata; ///< STFT output scratch (nfft/2 + 1 bins)
    ftx_waterfall_t wf;  ///< Waterfall object
    float max_mag;       ///< Maximum detected magnitude (debug stats)

    // KISS FFT housekeeping variables
    void* fft_work;        ///< Work area required by Kiss FFT
    kiss_fftr_cfg fft_cfg; ///< Kiss FFT housekeeping object
#ifdef WATERFALL_USE_PHASE
    int nifft;             ///< iFFT size
    void* ifft_work;       ///< Work area required by inverse Kiss FFT
    kiss_fft_cfg ifft_cfg; ///< Inverse Kiss FFT housekeeping object
#endif
} monitor_t;

void monitor_init(monitor_t* me, const monitor_config_t* cfg);
void monitor_reset(monitor_t* me);
void monitor_process(monitor_t* me, const float* frame);
void monitor_free(monitor_t* me);
/// True only if every buffer monitor_init() allocated actually exists. On a
/// memory-starved device monitor_init() can come back with any of them NULL,
/// and monitor_process() writes through them unchecked.
bool monitor_alloc_ok(const monitor_t* me);

#ifdef WATERFALL_USE_PHASE
void monitor_resynth(const monitor_t* me, const candidate_t* candidate, float* signal);
#endif

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_MONITOR_H_