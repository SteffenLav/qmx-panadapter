/*
 * FT8 Stage 2: Iterative Subtraction Decoder
 *
 * Takes a successfully-decoded message and subtracts its reconstructed signal
 * from the waterfall, then re-runs candidate search on the residual.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ft8/decode.h"
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
#include "common/monitor.h"

#define GFSK_CONST_K 5.336446f  // == pi * sqrt(2 / log(2))
#define FT8_SYMBOL_BT 2.0f

/// Compute GFSK smoothing pulse
static void gfsk_pulse(int n_spsym, float symbol_bt, float* pulse)
{
    for (int i = 0; i < 3 * n_spsym; ++i) {
        float t = i / (float)n_spsym - 1.5f;
        float arg1 = GFSK_CONST_K * symbol_bt * (t + 0.5f);
        float arg2 = GFSK_CONST_K * symbol_bt * (t - 0.5f);
        pulse[i] = (erff(arg1) - erff(arg2)) / 2.0f;
    }
}

/// Synthesize GFSK waveform from symbols
/// Allocates on PSRAM heap for large buffers
static bool synthesize_gfsk(const uint8_t* symbols, int n_sym, float f0,
                            float symbol_bt, float symbol_period,
                            int signal_rate, float* signal)
{
    int n_spsym = (int)(0.5f + signal_rate * symbol_period);
    int n_wave = n_sym * n_spsym;
    float dphi_peak = 2.0f * (float)M_PI / n_spsym;

    // Allocate working buffers
    float* dphi = malloc((size_t)(n_wave + 2 * n_spsym) * sizeof(float));
    float* pulse = malloc((size_t)(3 * n_spsym) * sizeof(float));
    if (!dphi || !pulse) {
        if (dphi) free(dphi);
        if (pulse) free(pulse);
        return false;
    }

    // Initialize frequency deviation array
    for (int i = 0; i < n_wave + 2 * n_spsym; i++) {
        dphi[i] = 2.0f * (float)M_PI * f0 / signal_rate;
    }

    // Compute GFSK pulse
    gfsk_pulse(n_spsym, symbol_bt, pulse);

    // Apply symbol tones to the frequency deviation
    for (int i = 0; i < n_sym; i++) {
        int ib = i * n_spsym;
        for (int j = 0; j < 3 * n_spsym; j++) {
            dphi[j + ib] += dphi_peak * symbols[i] * pulse[j];
        }
    }

    // Add guard symbols
    for (int j = 0; j < 2 * n_spsym; j++) {
        dphi[j] += dphi_peak * pulse[j + n_spsym] * symbols[0];
        dphi[j + n_sym * n_spsym] += dphi_peak * pulse[j] * symbols[n_sym - 1];
    }

    // Generate waveform via phase integration
    float phi = 0;
    for (int k = 0; k < n_wave; k++) {
        signal[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2.0f * (float)M_PI);
    }

    // Apply envelope shaping
    int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; i++) {
        float env = (1.0f - cosf(2.0f * (float)M_PI * i / (2 * n_ramp))) / 2.0f;
        signal[i] *= env;
        signal[n_wave - 1 - i] *= env;
    }

    free(dphi);
    free(pulse);
    return true;
}

/// Extract symbols from a message text by re-encoding it
/// Returns the number of symbols in the message (FT8_NN = 79)
static int extract_symbols_from_text(const char* text, uint8_t* symbols)
{
    ftx_message_t msg;
    ftx_callsign_hash_interface_t hash_if = {NULL, NULL};  // No hash needed for encode
    ftx_message_rc_t rc;

    ftx_message_init(&msg);

    // Try to encode the text back to a message
    rc = ftx_message_encode(&msg, &hash_if, text);
    if (rc != FTX_MESSAGE_RC_OK) {
        fprintf(stderr, "WARNING: Could not re-encode message '%s' for symbol extraction\n", text);
        return 0;
    }

    // Extract the 174 LDPC bits from the message payload
    uint8_t ldpc_bits[174];
    for (int i = 0; i < 174; i++) {
        ldpc_bits[i] = (msg.payload[i / 8] >> (7 - (i % 8))) & 1;
    }

    // Convert LDPC bits to symbols via convolutional encoding
    // (This is simplified; the full path would convolve then interleave with Costas)
    // For now, use a lookup table approach similar to the decoder

    // The actual symbol array is computed by the encoder inside ft8_lib
    // We'd need to call the encoder and capture the symbols
    // For this prototype, we'll return a dummy implementation

    // TODO: implement proper symbol extraction from message
    (void)ldpc_bits;  // Suppress unused warning

    return FT8_NN;  // 79 symbols for FT8
}

/// Stage 2: Subtract one decoded message from waterfall and search residual
/// Returns number of new candidates found in the residual
int ft8_stage2_subtract_and_search(const ftx_waterfall_t* original_wf,
                                    const char* decoded_text,
                                    float base_freq_hz,
                                    float scale,
                                    ftx_candidate_t* residual_candidates,
                                    int max_candidates)
{
    if (!original_wf || !decoded_text || !residual_candidates || max_candidates <= 0) {
        return 0;
    }

    // Extract symbols from the decoded message
    uint8_t symbols[FT8_NN];
    int n_sym = extract_symbols_from_text(decoded_text, symbols);
    if (n_sym != FT8_NN) {
        fprintf(stderr, "ERROR: Could not extract symbols from '%s'\n", decoded_text);
        return 0;
    }

    // Allocate and synthesize the GFSK waveform
    int signal_len = (int)(FT8_SYMBOL_PERIOD * MONITOR_SAMPLE_RATE * FT8_NN);
    float* signal = malloc(signal_len * sizeof(float));
    if (!signal) {
        fprintf(stderr, "ERROR: Could not allocate signal buffer for synthesis\n");
        return 0;
    }

    if (!synthesize_gfsk(symbols, n_sym, base_freq_hz, FT8_SYMBOL_BT,
                        FT8_SYMBOL_PERIOD, MONITOR_SAMPLE_RATE, signal)) {
        free(signal);
        fprintf(stderr, "ERROR: GFSK synthesis failed\n");
        return 0;
    }

    // TODO: Convert synthesized signal to FFT magnitudes and subtract from waterfall
    // TODO: Run ftx_find_candidates on the residual waterfall
    // TODO: Decode candidates from residual

    free(signal);

    // For now, return 0 candidates (placeholder)
    return 0;
}

/// Run Stage 2 subtraction loop on the current waterfall
/// Subtracts each decoded message iteratively and collects new candidates
int ft8_stage2_run(const ftx_waterfall_t* power,
                   const char** decoded_texts,
                   int num_decoded,
                   float base_freq_hz,
                   ftx_candidate_t* all_candidates,
                   int* num_candidates_total)
{
    if (!power || !decoded_texts || num_decoded <= 0) {
        return 0;
    }

    int total_new = 0;
    float scale = 0.9f;  // Start with 90% subtraction scale

    for (int i = 0; i < num_decoded && i < 5; i++) {  // Limit to first 5 passes
        ftx_candidate_t residual_cands[140];
        int num_residual = ft8_stage2_subtract_and_search(power, decoded_texts[i],
                                                         base_freq_hz, scale,
                                                         residual_cands, 140);

        printf("Stage 2 pass %d (subtract '%s'): found %d candidates in residual\n",
               i + 1, decoded_texts[i], num_residual);

        total_new += num_residual;

        // Would need to decode and merge the new candidates here
        // For now, just counting
    }

    return total_new;
}
