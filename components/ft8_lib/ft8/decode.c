#include "decode.h"
#include "constants.h"
#include "crc.h"
#include "ldpc.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

// #define LOG_LEVEL LOG_DEBUG
// #include "debug.h"

/* ⛔⛔ STAGE-1 DIAGNOSTIC LOGGING - OFF ON THE DEVICE, BECAUSE IT ABORTED IT.
 *
 * This is offline weak-signal R&D instrumentation. It was built to write a CSV
 * while running the decoder over WAV files on a PC, and CLAUDE.md recorded that
 * work as "offline test-harness R&D only - NOT wired into device firmware".
 * ⚠ THAT WAS WRONG. decode.c IS the firmware's decoder, so this shipped, and
 * diag_log_candidate() runs from THREE places inside ftx_decode_candidate() -
 * once per candidate, up to 140 of them per FT8 slot, every slot, for ever.
 *
 * fopen("stage1_diag.csv") is a relative path on a device with no working
 * directory, so it never succeeded - which is worse rather than better, because
 * g_diag_log stayed NULL and every single candidate tried again.
 *
 * ⭐ AND IT KILLED THE BENCH. Serial-captured 2026-09-02, twice in half an hour:
 *
 *     abort() was called at PC ... on core 1
 *     task : ft8_dec   core 1   after 93.480 s of uptime
 *     lock_init_generic (newlib locks.c:65)
 *       <- __sfp (findfp.c:203)  <- _fopen_r  <- diag_log_open  <- diag_log_candidate
 *
 * newlib allocates a FILE and CREATES A LOCK before it can even fail to open the
 * path, and lock_init_generic aborts outright when the mutex cannot be made. The
 * heap watchdog either side of the crash read `int free=10KB (min=0KB lblk=2KB
 * LOW!)`, so on this board that allocation is a coin toss - and this code was
 * tossing it hundreds of times a slot to write a file that could never exist.
 *
 * The first two attempts at these crashes blamed newly-added firmware, on the
 * strength of "the task that crashed is the one I just touched". Wrong both
 * times. What actually found it was a serial capture that was running BEFORE
 * the crash, plus addr2line on the return addresses in the register dump.
 *
 * Compiled out by default. Define FT8_STAGE1_DIAG=1 in a host harness build to
 * get it back; do not define it for the device. */
#ifndef FT8_STAGE1_DIAG
#define FT8_STAGE1_DIAG 0
#endif

#if FT8_STAGE1_DIAG

static FILE* g_diag_log = NULL;
static char g_current_filename[256] = "unknown";

void diag_set_filename(const char* filename) {
    if (filename) {
        strncpy(g_current_filename, filename, sizeof(g_current_filename) - 1);
        g_current_filename[sizeof(g_current_filename) - 1] = 0;
    }
}

static void diag_log_open(void) {
    if (!g_diag_log) {
        g_diag_log = fopen("stage1_diag.csv", "a");
        if (g_diag_log) {
            // Check if file is empty (first write)
            fseek(g_diag_log, 0, SEEK_END);
            if (ftell(g_diag_log) == 0) {
                // Write header
                fprintf(g_diag_log, "timestamp,file,candidate,snr_db,freq_offset,single_symbol_errors,fallback_triggered,multi_symbol_errors,final_result,message\n");
            }
            fflush(g_diag_log);
        }
    }
}

static void diag_log_candidate(int cand_idx, float snr_db, float freq_offset,
                                int single_errors, int fallback_trigg, int multi_errors, int final_result, const char* message) {
    diag_log_open();
    if (!g_diag_log) return;

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(g_diag_log, "%s,%s,%d,%.1f,%.2f,%d,%d,%d,%d,%s\n",
            time_str, g_current_filename, cand_idx, snr_db, freq_offset,
            single_errors, fallback_trigg, multi_errors, final_result, message ? message : "");
    fflush(g_diag_log);
}

void diag_log_close(void) {
    if (g_diag_log) {
        fclose(g_diag_log);
        g_diag_log = NULL;
    }
}


#else  /* !FT8_STAGE1_DIAG - the device build */

void diag_set_filename(const char* filename) { (void)filename; }
void diag_log_close(void) { }

static inline void diag_log_candidate(int cand_idx, float snr_db, float freq_offset,
                                      int single_errors, int fallback_trigg,
                                      int multi_errors, int final_result,
                                      const char* message)
{
    (void)cand_idx; (void)snr_db; (void)freq_offset; (void)single_errors;
    (void)fallback_trigg; (void)multi_errors; (void)final_result; (void)message;
}

#endif /* FT8_STAGE1_DIAG */

// Lookup table for y = 10*log10(1 + 10^(x/10)), where
//   y - increase in signal level dB when adding a weaker independent signal
//   x - specific relative strength of the weaker signal in dB
// Table index corresponds to x in dB (index 0: 0 dB, index 1: -1 dB etc)
static const float db_power_sum[40] = {
    3.01029995663981f, 2.53901891043867f, 2.1244260279434f, 1.76434862436485f, 1.45540463109294f,
    1.19331048066095f, 0.973227937086954f, 0.790097496525665f, 0.638920341433796f, 0.514969420252302f,
    0.413926851582251f, 0.331956199884278f, 0.265723755961025f, 0.212384019142551f, 0.16954289279533f,
    0.135209221080382f, 0.10774225511957f, 0.085799992300358f, 0.06829128312453f, 0.054333142200458f,
    0.043213737826426f, 0.034360947517284f, 0.027316043349389f, 0.021711921641451f, 0.017255250287928f,
    0.013711928326833f, 0.010895305999614f, 0.008656680827934f, 0.006877654943187f, 0.005464004928574f,
    0.004340774793186f, 0.003448354310253f, 0.002739348814965f, 0.002176083232619f, 0.001728613409904f,
    0.001373142636584f, 0.001090761428665f, 0.000866444976964f, 0.000688255828734f, 0.000546709946839f
};

/// Compute log likelihood log(p(1) / p(0)) of 174 message bits for later use in soft-decision LDPC decoding
/// @param[in] wf Waterfall data collected during message slot
/// @param[in] cand Candidate to extract the message from
/// @param[in] code_map Symbol encoding map
/// @param[out] log174 Output of decoded log likelihoods for each of the 174 message bits
static void ft4_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174);
static void ft8_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174);

/// Packs a string of bits each represented as a zero/non-zero byte in bit_array[],
/// as a string of packed bits starting from the MSB of the first byte of packed[]
/// @param[in] plain Array of bits (0 and nonzero values) with num_bits entires
/// @param[in] num_bits Number of bits (entries) passed in bit_array
/// @param[out] packed Byte-packed bits representing the data in bit_array
static void pack_bits(const uint8_t bit_array[], int num_bits, uint8_t packed[]);

static float max2(float a, float b);
static float max4(float a, float b, float c, float d);
static void heapify_down(ftx_candidate_t heap[], int heap_size);
static void heapify_up(ftx_candidate_t heap[], int heap_size);

static void ftx_normalize_logl(float* log174);
static void ft4_extract_symbol(const WF_ELEM_T* wf, float* logl);
static void ft8_extract_symbol(const WF_ELEM_T* wf, float* logl);
static void ft8_decode_multi_symbols(const WF_ELEM_T* wf, int num_bins, int n_syms, int bit_idx, float* log174);
static void ft8_decode_multi_symbols_pair(const WF_ELEM_T* wf1, const WF_ELEM_T* wf2,
                                          int num_bins, int bit_idx, float* log174);

static const WF_ELEM_T* get_cand_mag(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int offset = candidate->time_offset;
    offset = (offset * wf->time_osr) + candidate->time_sub;
    offset = (offset * wf->freq_osr) + candidate->freq_sub;
    offset = (offset * wf->num_bins) + candidate->freq_offset;
    return wf->mag + offset;
}

static int ft8_sync_score(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int score = 0;
    int num_average = 0;

    // Get the pointer to symbol 0 of the candidate
    const WF_ELEM_T* mag_cand = get_cand_mag(wf, candidate);

    // Compute average score over sync symbols (m+k = 0-7, 36-43, 72-79)
    for (int m = 0; m < FT8_NUM_SYNC; ++m)
    {
        for (int k = 0; k < FT8_LENGTH_SYNC; ++k)
        {
            int block = (FT8_SYNC_OFFSET * m) + k;          // relative to the message
            int block_abs = candidate->time_offset + block; // relative to the captured signal
            // Check for time boundaries
            if (block_abs < 0)
                continue;
            if (block_abs >= wf->num_blocks)
                break;

            // Get the pointer to symbol 'block' of the candidate
            const WF_ELEM_T* p8 = mag_cand + (block * wf->block_stride);

            // Weighted difference between the expected and all other symbols
            // Does not work as well as the alternative score below
            // score += 8 * p8[kFT8_Costas_pattern[k]] -
            //          p8[0] - p8[1] - p8[2] - p8[3] -
            //          p8[4] - p8[5] - p8[6] - p8[7];
            // ++num_average;

            // Check only the neighbors of the expected symbol frequency- and time-wise
            int sm = kFT8_Costas_pattern[k]; // Index of the expected bin
            if (sm > 0)
            {
                // look at one frequency bin lower
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm - 1]);
                ++num_average;
            }
            if (sm < 7)
            {
                // look at one frequency bin higher
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm + 1]);
                ++num_average;
            }
            if ((k > 0) && (block_abs > 0))
            {
                // look one symbol back in time
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm - wf->block_stride]);
                ++num_average;
            }
            if (((k + 1) < FT8_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks))
            {
                // look one symbol forward in time
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm + wf->block_stride]);
                ++num_average;
            }
        }
    }

    if (num_average > 0)
        score /= num_average;

    return score;
}

static int ft4_sync_score(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int score = 0;
    int num_average = 0;

    // Get the pointer to symbol 0 of the candidate
    const WF_ELEM_T* mag_cand = get_cand_mag(wf, candidate);

    // Compute average score over sync symbols (block = 1-4, 34-37, 67-70, 100-103)
    for (int m = 0; m < FT4_NUM_SYNC; ++m)
    {
        for (int k = 0; k < FT4_LENGTH_SYNC; ++k)
        {
            int block = 1 + (FT4_SYNC_OFFSET * m) + k;
            int block_abs = candidate->time_offset + block;
            // Check for time boundaries
            if (block_abs < 0)
                continue;
            if (block_abs >= wf->num_blocks)
                break;

            // Get the pointer to symbol 'block' of the candidate
            const WF_ELEM_T* p4 = mag_cand + (block * wf->block_stride);

            int sm = kFT4_Costas_pattern[m][k]; // Index of the expected bin

            // score += (4 * p4[sm]) - p4[0] - p4[1] - p4[2] - p4[3];
            // num_average += 4;

            // Check only the neighbors of the expected symbol frequency- and time-wise
            if (sm > 0)
            {
                // look at one frequency bin lower
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm - 1]);
                ++num_average;
            }
            if (sm < 3)
            {
                // look at one frequency bin higher
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm + 1]);
                ++num_average;
            }
            if ((k > 0) && (block_abs > 0))
            {
                // look one symbol back in time
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm - wf->block_stride]);
                ++num_average;
            }
            if (((k + 1) < FT4_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks))
            {
                // look one symbol forward in time
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm + wf->block_stride]);
                ++num_average;
            }
        }
    }

    if (num_average > 0)
        score /= num_average;

    return score;
}

int ftx_find_candidates(const ftx_waterfall_t* wf, int num_candidates, ftx_candidate_t heap[], int min_score)
{
    int (*sync_fun)(const ftx_waterfall_t*, const ftx_candidate_t*) = (wf->protocol == FTX_PROTOCOL_FT4) ? ft4_sync_score : ft8_sync_score;
    int num_tones = (wf->protocol == FTX_PROTOCOL_FT4) ? 4 : 8;

    int heap_size = 0;
    ftx_candidate_t candidate;

    // Here we allow time offsets that exceed signal boundaries, as long as we still have all data bits.
    // I.e. we can afford to skip the first 7 or the last 7 Costas symbols, as long as we track how many
    // sync symbols we included in the score, so the score is averaged.
    for (candidate.time_sub = 0; candidate.time_sub < wf->time_osr; ++candidate.time_sub)
    {
        for (candidate.freq_sub = 0; candidate.freq_sub < wf->freq_osr; ++candidate.freq_sub)
        {
            for (candidate.time_offset = -10; candidate.time_offset < 20; ++candidate.time_offset)
            {
                for (candidate.freq_offset = 0; (candidate.freq_offset + num_tones - 1) < wf->num_bins; ++candidate.freq_offset)
                {
                    candidate.score = sync_fun(wf, &candidate);

                    if (candidate.score < min_score)
                        continue;

                    // If the heap is full AND the current candidate is better than
                    // the worst in the heap, we remove the worst and make space
                    if ((heap_size == num_candidates) && (candidate.score > heap[0].score))
                    {
                        --heap_size;
                        heap[0] = heap[heap_size];
                        heapify_down(heap, heap_size);
                    }

                    // If there's free space in the heap, we add the current candidate
                    if (heap_size < num_candidates)
                    {
                        heap[heap_size] = candidate;
                        ++heap_size;
                        heapify_up(heap, heap_size);
                    }
                }
            }
        }
    }

    // Sort the candidates by sync strength - here we benefit from the heap structure
    int len_unsorted = heap_size;
    while (len_unsorted > 1)
    {
        // Take the top (index 0) element which is guaranteed to have the smallest score,
        // exchange it with the last element in the heap, and decrease the heap size.
        // Then restore the heap property in the new, smaller heap.
        // At the end the elements will be sorted in descending order.
        ftx_candidate_t tmp = heap[len_unsorted - 1];
        heap[len_unsorted - 1] = heap[0];
        heap[0] = tmp;
        len_unsorted--;
        heapify_down(heap, len_unsorted);
    }

    return heap_size;
}

static void ft4_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand); // Pointer to 4 magnitude bins of the first symbol

    // Go over FSK tones and skip Costas sync symbols
    for (int k = 0; k < FT4_ND; ++k)
    {
        // Skip either 5, 9 or 13 sync symbols
        // TODO: replace magic numbers with constants
        int sym_idx = k + ((k < 29) ? 5 : ((k < 58) ? 9 : 13));
        int bit_idx = 2 * k;

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
        }
        else
        {
            ft4_extract_symbol(mag + (sym_idx * wf->block_stride), log174 + bit_idx);
        }
    }
}

static void ft8_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand); // Pointer to 8 magnitude bins of the first symbol

    // Go over FSK tones and skip Costas sync symbols
    for (int k = 0; k < FT8_ND; ++k)
    {
        // Skip either 7 or 14 sync symbols
        // TODO: replace magic numbers with constants
        int sym_idx = k + ((k < 29) ? 7 : 14);
        int bit_idx = 3 * k;

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
            log174[bit_idx + 2] = 0;
        }
        else
        {
            ft8_extract_symbol(mag + (sym_idx * wf->block_stride), log174 + bit_idx);
        }
    }
}

// Second-chance likelihood extraction: uses multi-symbol (n_syms=2) instead of single-symbol.
// Only called if single-symbol bp_decode fails. Pairs consecutive data symbols to improve
// coherent detection on weak/fading signals.
static void ft8_extract_likelihood_multi(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand);
    const int num_bins = 8;  // FSK tones per symbol

    // Process pairs of consecutive data symbols (every 2 symbols, or every 6 bits)
    // FT8_ND = 59 data symbols; process in pairs: (0,1), (2,3), ..., (58, [skip if odd])
    for (int k = 0; k < FT8_ND; k += 2)
    {
        int sym_idx_1 = k + ((k < 29) ? 7 : 14);
        int sym_idx_2 = (k + 1) + (((k + 1) < 29) ? 7 : 14);
        int bit_idx = 3 * k;

        // Check time boundaries for first symbol
        int block_1 = cand->time_offset + sym_idx_1;
        if ((block_1 < 0) || (block_1 >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
            log174[bit_idx + 2] = 0;
        }
        else if (k + 1 >= FT8_ND)
        {
            // Last symbol (odd count): just use single-symbol
            ft8_extract_symbol(mag + (sym_idx_1 * wf->block_stride), log174 + bit_idx);
        }
        else
        {
            // Check time boundaries for second symbol
            int block_2 = cand->time_offset + sym_idx_2;
            if ((block_2 < 0) || (block_2 >= wf->num_blocks))
            {
                log174[bit_idx + 0] = 0;
                log174[bit_idx + 1] = 0;
                log174[bit_idx + 2] = 0;
                log174[bit_idx + 3] = 0;
                log174[bit_idx + 4] = 0;
                log174[bit_idx + 5] = 0;
            }
            else
            {
                // Extract using 2-symbol coherent demodulation
                // Pass mag with proper stride added for correct 2-symbol access
                ft8_decode_multi_symbols_pair(mag + (sym_idx_1 * wf->block_stride),
                                              mag + (sym_idx_2 * wf->block_stride),
                                              num_bins, bit_idx, log174);
            }
        }
    }
}

static void ftx_normalize_logl(float* log174)
{
    // Compute the variance of log174
    float sum = 0;
    float sum2 = 0;
    for (int i = 0; i < FTX_LDPC_N; ++i)
    {
        sum += log174[i];
        sum2 += log174[i] * log174[i];
    }
    float inv_n = 1.0f / FTX_LDPC_N;
    float variance = (sum2 - (sum * sum * inv_n)) * inv_n;

    // Normalize log174 distribution and scale it with experimentally found coefficient
    float norm_factor = sqrtf(24.0f / variance);
    for (int i = 0; i < FTX_LDPC_N; ++i)
    {
        log174[i] *= norm_factor;
    }
}

// Estimate SNR from candidate's signal and noise floor
static float estimate_snr_db(const ftx_waterfall_t* wf, const ftx_candidate_t* cand)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand);
    const int num_bins = 8;  // FSK tones per symbol

    // Collect signal power from the candidate's FSK tones (first 10 symbols)
    float signal_sum = 0.0f;
    int signal_count = 0;

    // Sample signal from the 8 FSK tone bins across multiple symbols
    for (int sym = 0; sym < 10 && sym < wf->num_blocks - cand->time_offset; ++sym) {
        const WF_ELEM_T* block = mag + sym * wf->block_stride;
        for (int bin = 0; bin < num_bins; ++bin) {
            float magnitude_db = WF_ELEM_MAG(block[bin]);
            // Convert from dB back to linear for averaging
            float magnitude_linear = powf(10.0f, magnitude_db / 10.0f);
            signal_sum += magnitude_linear;
            signal_count++;
        }
    }

    if (signal_count == 0) return -99.0f;
    float signal_avg_linear = signal_sum / signal_count;
    float signal_avg_db = 10.0f * log10f(signal_avg_linear + 1e-6f);

    // Estimate noise floor from bins between the FSK tones (non-signal bins)
    float noise_sum = 0.0f;
    int noise_count = 0;

    // Sample between the FSK tones (bins 4-7 in a typical 8-bin FSK window)
    // These are unlikely to contain strong signal
    for (int sym = 0; sym < 10 && sym < wf->num_blocks - cand->time_offset; ++sym) {
        const WF_ELEM_T* block = mag + sym * wf->block_stride;
        // Sample the "gaps" between expected FSK tones
        for (int bin = 50; bin < wf->num_bins && bin < 150; bin += 3) {
            float magnitude_db = WF_ELEM_MAG(block[bin]);
            float magnitude_linear = powf(10.0f, magnitude_db / 10.0f);
            noise_sum += magnitude_linear;
            noise_count++;
        }
    }

    if (noise_count == 0) {
        noise_sum = signal_avg_linear * 0.1f;  // Rough estimate: noise ~10% of signal
        noise_count = 1;
    }

    float noise_avg_linear = noise_sum / noise_count;
    float noise_avg_db = 10.0f * log10f(noise_avg_linear + 1e-6f);

    // SNR = signal - noise (in dB)
    float snr_db = signal_avg_db - noise_avg_db;

    // Clamp to reasonable range
    if (snr_db < -30.0f) snr_db = -30.0f;
    if (snr_db > 30.0f) snr_db = 30.0f;

    return snr_db;
}

// Extract most-likely symbols (0-7) from the waterfall magnitudes
// Populates the FT8_ND data symbols in the symbol array
// Costas sync symbols (1, 3, 5) are set to 0 as placeholder
static void ft8_extract_symbols(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, uint8_t* symbols)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand);

    // Initialize all 79 symbols to 0
    memset(symbols, 0, FT8_NN);

    // Go over FT8_ND data symbols and extract the most-likely tone (0-7) at each position
    for (int k = 0; k < FT8_ND; ++k)
    {
        // Accounting for Costas sync symbols (7 or 14 total skipped)
        int sym_idx = k + ((k < 29) ? 7 : 14);

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            symbols[sym_idx] = 0;  // Out of bounds, use 0
            continue;
        }

        // Find the tone with maximum magnitude at this symbol position
        const WF_ELEM_T* symbol_mag = mag + (sym_idx * wf->block_stride);
        float max_mag = WF_ELEM_MAG(symbol_mag[0]);
        uint8_t max_tone = 0;

        for (int tone = 1; tone < 8; ++tone)
        {
            float tone_mag = WF_ELEM_MAG(symbol_mag[tone]);
            if (tone_mag > max_mag)
            {
                max_mag = tone_mag;
                max_tone = tone;
            }
        }

        symbols[sym_idx] = max_tone;
    }
}

bool ftx_decode_candidate(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, int max_iterations, ftx_message_t* message, ftx_decode_status_t* status)
{
    // Estimate SNR early
    status->snr_db = estimate_snr_db(wf, cand);

    float log174[FTX_LDPC_N]; // message bits encoded as likelihood
    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        ft4_extract_likelihood(wf, cand, log174);
    }
    else
    {
        ft8_extract_likelihood(wf, cand, log174);
    }

    ftx_normalize_logl(log174);

    uint8_t plain174[FTX_LDPC_N]; // message bits (0/1)
    bp_decode(log174, max_iterations, plain174, &status->ldpc_errors);
    // ldpc_decode(log174, max_iterations, plain174, &status->ldpc_errors);

    int single_symbol_errors = status->ldpc_errors;
    int fallback_triggered = 0;
    int multi_symbol_errors = 0;

    // Save the original LLRs before multi-symbol extraction
    float log174_single[FTX_LDPC_N];
    if ((status->ldpc_errors > 0) && (wf->protocol == FTX_PROTOCOL_FT8)) {
        memcpy(log174_single, log174, sizeof(log174_single));
    }

    // Second-chance decode: if single-symbol failed, retry with multi-symbol (FT8 only)
    if ((status->ldpc_errors > 0) && (wf->protocol == FTX_PROTOCOL_FT8)) {
        fallback_triggered = 1;
        ft8_extract_likelihood_multi(wf, cand, log174);
        ftx_normalize_logl(log174);
        bp_decode(log174, max_iterations, plain174, &status->ldpc_errors);
        multi_symbol_errors = status->ldpc_errors;
    }

    if (status->ldpc_errors > 0)
    {
        // Log the failure
        diag_log_candidate(cand->score, 0.0f, cand->freq_offset,
                          single_symbol_errors, fallback_triggered, multi_symbol_errors, 0, "FAIL");
        return false;
    }

    // Extract payload + CRC (first FTX_LDPC_K bits) packed into a byte array
    uint8_t a91[FTX_LDPC_K_BYTES];
    pack_bits(plain174, FTX_LDPC_K, a91);

    // Extract CRC and check it
    status->crc_extracted = ftx_extract_crc(a91);
    // [1]: 'The CRC is calculated on the source-encoded message, zero-extended from 77 to 82 bits.'
    a91[9] &= 0xF8;
    a91[10] &= 0x00;
    status->crc_calculated = ftx_compute_crc(a91, 96 - 14);

    if (status->crc_extracted != status->crc_calculated)
    {
        // Log CRC failure
        diag_log_candidate(cand->score, 0.0f, cand->freq_offset,
                          single_symbol_errors, fallback_triggered, multi_symbol_errors, 0, "CRC_FAIL");
        return false;
    }

    // Reuse CRC value as a hash for the message (TODO: 14 bits only, should perhaps use full 16 or 32 bits?)
    message->hash = status->crc_calculated;

    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        // ‘[..] for FT4 only, in order to avoid transmitting a long string of zeros when sending CQ messages,
        // the assembled 77-bit message is bitwise exclusive-OR’ed with [a] pseudorandom sequence before computing the CRC and FEC parity bits’
        for (int i = 0; i < 10; ++i)
        {
            message->payload[i] = a91[i] ^ kFT4_XOR_sequence[i];
        }
    }
    else
    {
        for (int i = 0; i < 10; ++i)
        {
            message->payload[i] = a91[i];
        }
    }

    // Extract most-likely symbols for Stage 2 reconstruction
    ft8_extract_symbols(wf, cand, status->symbols);

    // Log successful decode
    diag_log_candidate(cand->score, 0.0f, cand->freq_offset,
                      single_symbol_errors, fallback_triggered, multi_symbol_errors, 1, "SUCCESS");

    // LOG(LOG_DEBUG, "Decoded message (CRC %04x), trying to unpack...\n", status->crc_extracted);
    return true;
}

static float max2(float a, float b)
{
    return (a >= b) ? a : b;
}

static float max4(float a, float b, float c, float d)
{
    return max2(max2(a, b), max2(c, d));
}

static void heapify_down(ftx_candidate_t heap[], int heap_size)
{
    // heapify from the root down
    int current = 0; // root node
    while (true)
    {
        int left = 2 * current + 1;
        int right = left + 1;

        // Find the smallest value of (parent, left child, right child)
        int smallest = current;
        if ((left < heap_size) && (heap[left].score < heap[smallest].score))
        {
            smallest = left;
        }
        if ((right < heap_size) && (heap[right].score < heap[smallest].score))
        {
            smallest = right;
        }

        if (smallest == current)
        {
            break;
        }

        // Exchange the current node with the smallest child and move down to it
        ftx_candidate_t tmp = heap[smallest];
        heap[smallest] = heap[current];
        heap[current] = tmp;
        current = smallest;
    }
}

static void heapify_up(ftx_candidate_t heap[], int heap_size)
{
    // heapify from the last node up
    int current = heap_size - 1;
    while (current > 0)
    {
        int parent = (current - 1) / 2;
        if (!(heap[current].score < heap[parent].score))
        {
            break;
        }

        // Exchange the current node with its parent and move up
        ftx_candidate_t tmp = heap[parent];
        heap[parent] = heap[current];
        heap[current] = tmp;
        current = parent;
    }
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of 2 message bits (1 FSK symbol)
static void ft4_extract_symbol(const WF_ELEM_T* wf, float* logl)
{
    // Cleaned up code for the simple case of n_syms==1
    float s2[4];

    for (int j = 0; j < 4; ++j)
    {
        s2[j] = WF_ELEM_MAG(wf[kFT4_Gray_map[j]]);
    }

    logl[0] = max2(s2[2], s2[3]) - max2(s2[0], s2[1]);
    logl[1] = max2(s2[1], s2[3]) - max2(s2[0], s2[2]);
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of 3 message bits (1 FSK symbol)
static void ft8_extract_symbol(const WF_ELEM_T* wf, float* logl)
{
    // Cleaned up code for the simple case of n_syms==1
#if 1
    float s2[8];

    for (int j = 0; j < 8; ++j)
    {
        s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j]]);
    }

    logl[0] = max4(s2[4], s2[5], s2[6], s2[7]) - max4(s2[0], s2[1], s2[2], s2[3]);
    logl[1] = max4(s2[2], s2[3], s2[6], s2[7]) - max4(s2[0], s2[1], s2[4], s2[5]);
    logl[2] = max4(s2[1], s2[3], s2[5], s2[7]) - max4(s2[0], s2[2], s2[4], s2[6]);
#else
    float a[7] = {
        // (float)wf[7] - (float)wf[0], // 0: p(111) / p(000)
        (float)wf[5] - (float)wf[2], // 0: p(100) / p(011)
        (float)wf[3] - (float)wf[0], // 1: p(010) / p(000)
        (float)wf[6] - (float)wf[3], // 2: p(101) / p(010)
        (float)wf[6] - (float)wf[2], // 3: p(101) / p(011)
        (float)wf[7] - (float)wf[4], // 4: p(111) / p(110)
        (float)wf[4] - (float)wf[1], // 5: p(110) / p(001)
        (float)wf[5] - (float)wf[1]  // 6: p(100) / p(001)
    };
    float k = 1.0f;

    // logl[0] = k * (a[0] + a[2] + a[3] + a[5] + a[6]) / 5;
    // logl[1] = k * (a[0] / 4 + (a[1] - a[3]) * 5 / 24 + (a[5] - a[2]) / 6 + (a[4] - a[6]) / 24);
    // logl[2] = k * (a[0] / 4 + (a[1] - a[3]) / 24 + (a[2] - a[5]) / 6 + (a[4] - a[6]) * 5 / 24);
    logl[0] = k * (a[1] / 6 + a[2] / 3 + a[3] / 6 + a[4] / 6 + a[5] / 3 + a[6] / 6);
    logl[1] = k * (-a[0] / 4 + a[1] * 7 / 24 + (a[4] - a[3]) / 8 + a[5] / 3 + a[6] / 24);
    logl[2] = k * (-a[0] / 4 + (a[1] - a[6]) / 8 + a[2] / 3 + a[3] / 24 + a[4] * 7 / 24 - a[5] * 5 / 18);
#endif
    // for (int i = 0; i < 8; ++i)
    //     printf("%d ", WF_ELEM_MAG_INT(wf[i]));
    // for (int i = 0; i < 3; ++i)
    //     printf("%.1f ", logl[i]);
    // printf("\n");
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of bits corresponding to several FSK symbols at once
// Helper: decode 2 consecutive symbols via coherent demodulation
static void ft8_decode_multi_symbols_pair(const WF_ELEM_T* wf1, const WF_ELEM_T* wf2,
                                          int num_bins, int bit_idx, float* log174)
{
    const int n_bits = 6;  // 2 symbols * 3 bits per symbol
    const int n_tones = 64;  // 2^6

    float s2[n_tones];

    // Compute sum of magnitude pairs for all 64 tone combinations
    for (int j = 0; j < n_tones; ++j)
    {
        int j1 = j & 0x07;      // Lower 3 bits = tones for symbol 1
        int j2 = (j >> 3) & 0x07;  // Upper 3 bits = tones for symbol 2

        s2[j] = WF_ELEM_MAG(wf1[kFT8_Gray_map[j1]]) + WF_ELEM_MAG(wf2[kFT8_Gray_map[j2]]);
    }

    // Extract bit significance
    for (int i = 0; i < n_bits; ++i)
    {
        if (bit_idx + i >= FTX_LDPC_N)
        {
            break;
        }

        uint16_t mask = (n_tones >> (i + 1));
        float max_zero = -1000, max_one = -1000;
        for (int n = 0; n < n_tones; ++n)
        {
            if (n & mask)
            {
                max_one = max2(max_one, s2[n]);
            }
            else
            {
                max_zero = max2(max_zero, s2[n]);
            }
        }

        log174[bit_idx + i] = max_one - max_zero;
    }
}

static void ft8_decode_multi_symbols(const WF_ELEM_T* wf, int num_bins, int n_syms, int bit_idx, float* log174)
{
    const int n_bits = 3 * n_syms;
    const int n_tones = (1 << n_bits);

    float s2[n_tones];

    for (int j = 0; j < n_tones; ++j)
    {
        int j1 = j & 0x07;
        if (n_syms == 1)
        {
            s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j1]]);
            continue;
        }
        int j2 = (j >> 3) & 0x07;
        if (n_syms == 2)
        {
            s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j2]]);
            s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j1] + 4 * num_bins]);
            continue;
        }
        int j3 = (j >> 6) & 0x07;
        s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j3]]);
        s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j2] + 4 * num_bins]);
        s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j1] + 8 * num_bins]);
    }

    // Extract bit significance (and convert them to float)
    // 8 FSK tones = 3 bits
    for (int i = 0; i < n_bits; ++i)
    {
        if (bit_idx + i >= FTX_LDPC_N)
        {
            // Respect array size
            break;
        }

        uint16_t mask = (n_tones >> (i + 1));
        float max_zero = -1000, max_one = -1000;
        for (int n = 0; n < n_tones; ++n)
        {
            if (n & mask)
            {
                max_one = max2(max_one, s2[n]);
            }
            else
            {
                max_zero = max2(max_zero, s2[n]);
            }
        }

        log174[bit_idx + i] = max_one - max_zero;
    }
}

// Packs a string of bits each represented as a zero/non-zero byte in plain[],
// as a string of packed bits starting from the MSB of the first byte of packed[]
static void pack_bits(const uint8_t bit_array[], int num_bits, uint8_t packed[])
{
    int num_bytes = (num_bits + 7) / 8;
    for (int i = 0; i < num_bytes; ++i)
    {
        packed[i] = 0;
    }

    uint8_t mask = 0x80;
    int byte_idx = 0;
    for (int i = 0; i < num_bits; ++i)
    {
        if (bit_array[i])
        {
            packed[byte_idx] |= mask;
        }
        mask >>= 1;
        if (!mask)
        {
            mask = 0x80;
            ++byte_idx;
        }
    }
}