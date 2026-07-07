/*
 * FT8 Stage 2: Residual (iterative-subtraction) weak-signal rescue decoder.
 *
 * Approach (corrected 2026-07-07):
 *   1. Take every baseline-decoded message together with its candidate position
 *      and its per-symbol tones.
 *   2. Copy the real uint8_t waterfall and NOTCH each decoded signal's
 *      time/frequency cells down toward the local noise floor (magnitude-domain
 *      subtraction, controlled by a scale factor).
 *   3. Re-run ftx_find_candidates() + ftx_decode_candidate() on the residual
 *      waterfall and report any NEW messages that decode (deduped against the
 *      baseline) — those are the rescued weak signals.
 *
 * Note: this operates directly on the native uint8_t waterfall used by the
 * decoder (mag stored as (dB+120)*2). The earlier version reinterpreted that
 * buffer as float[], which corrupted the heap and produced a fixed 140
 * "candidates" per pass — an artifact, not real unmasking. That is fixed here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ft8/decode.h"
#include "ft8/message.h"
#include "ft8/constants.h"
#include "common/monitor.h"

// Waterfall element helpers (must match decode.h's uint8_t representation).
#define WF_DB(x)   ((float)(x) * 0.5f - 120.0f)   // uint8 -> dB
#define WF_FROM_DB(db) wf_from_db(db)             // dB -> uint8 (clamped)

// Default magnitude scale factor: fraction of each signal's power-above-noise
// removed during subtraction. 1.0 = notch fully to the local noise floor.
#define SUBTRACTION_SCALE 0.9f

// Must match the struct in ft8_test_harness.c exactly.
typedef struct {
    char text[128];
    int score;
    float snr_db;
    uint8_t symbols[FT8_NN];
    ftx_candidate_t cand;
    uint32_t hash;
} decoded_msg_t;

// Weak signals present in websdr_test1 but missed by the baseline decoder.
static const char* RESCUE_TARGETS[] = {
    "LZ1CWK DC8VA RR73",
    "CQ EA1HTF IN52",
    "YO7CGS A41ZZ -11",
    "R2ATW IZ0VLL -16",
    "CQ DX Z33Z KN11",
};
#define NUM_TARGETS ((int)(sizeof(RESCUE_TARGETS) / sizeof(RESCUE_TARGETS[0])))

static int is_rescue_target(const char* text)
{
    if (!text) return 0;
    for (int i = 0; i < NUM_TARGETS; i++) {
        if (strcmp(text, RESCUE_TARGETS[i]) == 0) return 1;
    }
    return 0;
}

static uint8_t wf_from_db(float db)
{
    int v = (int)((db + 120.0f) * 2.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

// Fill the 21 Costas sync symbols (positions 0-6, 36-42, 72-78) with the known
// sync tones; ft8_extract_symbols() leaves them as 0 placeholders.
static void fill_sync_symbols(uint8_t* symbols)
{
    for (int g = 0; g < FT8_NUM_SYNC; g++) {
        int base = g * FT8_SYNC_OFFSET;
        for (int k = 0; k < FT8_LENGTH_SYNC; k++) {
            symbols[base + k] = kFT8_Costas_pattern[k];
        }
    }
}

// Notch one waterfall cell toward the noise floor (linear-power domain).
// scale in [0,1]: fraction of (signal - noise) power removed.
static void notch_cell(uint8_t* mag, int idx, float noise_db, float scale)
{
    float sig_db = WF_DB(mag[idx]);
    if (sig_db <= noise_db) return;  // already at/below noise, nothing to remove

    float p_sig = powf(10.0f, sig_db / 10.0f);
    float p_noise = powf(10.0f, noise_db / 10.0f);
    float p_res = p_noise + (1.0f - scale) * (p_sig - p_noise);
    float res_db = 10.0f * log10f(p_res + 1e-12f);
    mag[idx] = wf_from_db(res_db);
}

// Subtract one decoded message from the residual waterfall by notching the
// (symbol, tone) cells it occupies. Returns the number of cells notched.
static int subtract_message(uint8_t* mag,
                            int num_blocks, int num_bins,
                            int time_osr, int freq_osr,
                            const ftx_candidate_t* cand,
                            const uint8_t* symbols,
                            float scale)
{
    int notched = 0;
    for (int sym = 0; sym < FT8_NN; sym++) {
        int block = cand->time_offset + sym;
        if (block < 0 || block >= num_blocks) continue;

        int tone = symbols[sym];
        int bin = cand->freq_offset + tone;
        if (bin < 0 || bin >= num_bins) continue;

        // Local noise floor: mean dB of the 7 non-signal tone bins at the
        // candidate's own sub-cell for this symbol block.
        int subcell = (((block * time_osr + cand->time_sub) * freq_osr
                        + cand->freq_sub) * num_bins);
        float noise_sum = 0.0f;
        int noise_n = 0;
        for (int tt = 0; tt < 8; tt++) {
            if (tt == tone) continue;
            int b = cand->freq_offset + tt;
            if (b < 0 || b >= num_bins) continue;
            noise_sum += WF_DB(mag[subcell + b]);
            noise_n++;
        }
        float noise_db = (noise_n > 0) ? (noise_sum / noise_n) : -110.0f;

        // Notch the signal's tone bin across every time/freq sub-cell (the
        // STFT overlap spreads a tone's energy across all OSR sub-bins).
        for (int ts = 0; ts < time_osr; ts++) {
            for (int fs = 0; fs < freq_osr; fs++) {
                int cellbase = (((block * time_osr + ts) * freq_osr + fs) * num_bins);
                notch_cell(mag, cellbase + bin, noise_db, scale);
                notched++;
            }
        }
    }
    return notched;
}

// Build a residual waterfall by copying the monitor's and subtracting every
// baseline decode at the given scale. Caller owns/frees residual_mag.
// Returns the residual mag buffer (uint8), or NULL on allocation failure.
static uint8_t* build_residual(decoded_msg_t* list, int n,
                               const monitor_t* mon, float scale,
                               int* out_total_notched)
{
    const ftx_waterfall_t* wf = &mon->wf;
    size_t mag_size = (size_t)wf->max_blocks * wf->block_stride;
    // One persistent scratch buffer, reused across scale-sweep calls. Repeatedly
    // malloc/free-ing a ~160 KB block per call proved unstable on this toolchain's
    // heap; a single reused buffer is both robust and faster (single-threaded).
    static uint8_t* residual = NULL;
    static size_t residual_cap = 0;
    if (residual_cap < mag_size) {
        free(residual);
        residual = malloc(mag_size);
        residual_cap = residual ? mag_size : 0;
    }
    if (!residual) return NULL;
    memcpy(residual, wf->mag, mag_size);

    int total_notched = 0;
    for (int i = 0; i < n; i++) {
        uint8_t syms[FT8_NN];
        memcpy(syms, list[i].symbols, FT8_NN);
        fill_sync_symbols(syms);
        total_notched += subtract_message(residual, wf->num_blocks, wf->num_bins,
                                          wf->time_osr, wf->freq_osr,
                                          &list[i].cand, syms, scale);
    }
    if (out_total_notched) *out_total_notched = total_notched;
    return residual;
}

// Decode the residual waterfall; report NEW messages not in the baseline.
// Returns residual candidate count; fills *out_new and *out_rescued.
static int decode_residual(decoded_msg_t* list, int n,
                           const monitor_t* mon, float scale,
                           int verbose, int* out_new, int* out_rescued)
{
    int total_notched = 0;
    uint8_t* residual = build_residual(list, n, mon, scale, &total_notched);
    if (!residual) {
        if (out_new) *out_new = 0;
        if (out_rescued) *out_rescued = 0;
        return 0;
    }

    // Residual waterfall shares all geometry with the original, only mag differs.
    ftx_waterfall_t rwf = mon->wf;
    rwf.mag = residual;

    #define MAX_RES_CAND 200
    ftx_candidate_t cands[MAX_RES_CAND];
    int num_cand = ftx_find_candidates(&rwf, MAX_RES_CAND, cands, 10);

    if (verbose) {
        printf("  Subtracted %d baseline signals (%d cells notched, %.0f%% scale)\n",
               n, total_notched, scale * 100.0f);
        printf("  Residual candidates: %d\n", num_cand);
    }

    int new_count = 0, rescued_count = 0;
    uint32_t seen_hash[256];
    int num_seen = 0;

    for (int idx = 0; idx < num_cand; idx++) {
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&rwf, &cands[idx], 20, &msg, &st)) continue;

        // Skip anything already in the baseline set.
        int is_baseline = 0;
        for (int j = 0; j < n; j++) {
            if (list[j].hash == msg.hash) { is_baseline = 1; break; }
        }
        if (is_baseline) continue;

        // Dedup within the residual decodes.
        int dup = 0;
        for (int j = 0; j < num_seen; j++) {
            if (seen_hash[j] == msg.hash) { dup = 1; break; }
        }
        if (dup) continue;
        if (num_seen < 256) seen_hash[num_seen++] = msg.hash;

        char text[128];
        ftx_message_offsets_t offsets;
        if (ftx_message_decode(&msg, NULL, text, &offsets) != FTX_MESSAGE_RC_OK) continue;

        new_count++;
        int rescued = is_rescue_target(text);
        if (rescued) rescued_count++;

        if (verbose) {
            printf("    NEW decode: %-22s (snr %.0f dB)%s\n",
                   text, st.snr_db, rescued ? "   <-- RESCUED TARGET" : "");
        }
    }

    if (out_new) *out_new = new_count;
    if (out_rescued) *out_rescued = rescued_count;
    return num_cand;
}

// ---------------------------------------------------------------------------
// Task 1: residual subtraction + decode at the default scale.
// ---------------------------------------------------------------------------
int ft8_stage2_run_loop(decoded_msg_t* list, int n, monitor_t* mon, int sample_rate)
{
    (void)sample_rate;
    if (!list || !mon || n <= 0) {
        printf("\n=== Stage 2 ===\nNo baseline decodes; skipping.\n");
        return 0;
    }

    printf("\n============================================================\n");
    printf(" Stage 2: Residual Weak-Signal Rescue (scale %.2f)\n", SUBTRACTION_SCALE);
    printf("============================================================\n");
    printf("Waterfall: %d blocks x %d bins (time_osr %d, freq_osr %d)\n",
           mon->wf.num_blocks, mon->wf.num_bins, mon->wf.time_osr, mon->wf.freq_osr);
    printf("Subtracting %d baseline decodes, then re-decoding residual:\n\n", n);

    int new_count = 0, rescued_count = 0;
    decode_residual(list, n, mon, SUBTRACTION_SCALE, 1, &new_count, &rescued_count);

    printf("\n  Rescued: %d new decode(s), %d of %d known weak targets.\n",
           new_count, rescued_count, NUM_TARGETS);
    return rescued_count;
}

// ---------------------------------------------------------------------------
// Task 2: scale-factor tuning sweep.
// ---------------------------------------------------------------------------
void ft8_stage2_scale_sweep(decoded_msg_t* list, int n, monitor_t* mon, int sample_rate)
{
    (void)sample_rate;
    if (!list || !mon || n <= 0) return;

    // 0.00 = control: no subtraction (notch removes nothing) through the exact
    // same residual pipeline, so any rescue at 0.00 is due to the wider candidate
    // heap, NOT the subtraction. Rescues above the 0.00 baseline are the real gain.
    static const float scales[] = { 0.00f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f };
    int num_scales = (int)(sizeof(scales) / sizeof(scales[0]));

    printf("\n============================================================\n");
    printf(" Stage 2: Scale-Factor Tuning Sweep\n");
    printf("============================================================\n");
    printf("%8s %14s %12s %12s\n", "Scale", "ResidualCand", "NewDecodes", "Rescued");
    printf("%8s %14s %12s %12s\n", "-----", "------------", "----------", "-------");

    for (int i = 0; i < num_scales; i++) {
        int new_count = 0, rescued_count = 0;
        int num_cand = decode_residual(list, n, mon, scales[i], 0,
                                       &new_count, &rescued_count);
        printf("%8.2f %14d %12d %12d\n", scales[i], num_cand, new_count, rescued_count);
    }
    printf("\n(Higher scale removes more of each strong signal; watch for the\n");
    printf(" point where new decodes stop increasing or false decodes appear.)\n");
}
