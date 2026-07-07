/*
 * FT8 Test Harness — Stage 1 (multi-symbol fallback) validation
 *
 * Decodes a WAV file and compares decoded messages against expected-decode .txt file
 * Output format: "OLD: X correct, NEW: Y correct out of Z expected (improvement: +Z)"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include <ft8/decode.h>
#include <ft8/message.h>
#include <common/monitor.h>

// Simple WAV loader (replaces common/wave.h which doesn't exist standalone)
int load_wav(float* signal, int* num_samples, int* sample_rate, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    // Read RIFF header
    char riff[4];
    fread(riff, 1, 4, f);
    if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F') {
        fprintf(stderr, "ERROR: Not a valid WAV file\n");
        fclose(f);
        return -1;
    }

    // Skip to fmt chunk
    fseek(f, 12, SEEK_SET);  // Skip RIFF size and "WAVE"
    int chunk_id, chunk_size, sample_rate_tmp, fmt;
    while (fread(&chunk_id, 4, 1, f) == 1) {
        fread(&chunk_size, 4, 1, f);
        if (chunk_id == 0x20746d66) {  // "fmt "
            fread(&fmt, 2, 1, f);  // Format code (should be 1 for PCM)
            short channels;
            fread(&channels, 2, 1, f);
            fread(&sample_rate_tmp, 4, 1, f);
            *sample_rate = sample_rate_tmp;
            fseek(f, chunk_size - 8, SEEK_CUR);
        } else if (chunk_id == 0x61746164) {  // "data"
            // Read audio data
            short* pcm_data = malloc(chunk_size);
            fread(pcm_data, 1, chunk_size, f);

            // Convert 16-bit PCM to float
            int num_samples_read = chunk_size / 2;
            *num_samples = (num_samples_read < *num_samples) ? num_samples_read : *num_samples;
            for (int i = 0; i < *num_samples; i++) {
                signal[i] = (float)pcm_data[i] / 32768.0f;
            }
            free(pcm_data);
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    fclose(f);
    return 0;
}

#define SAMPLE_RATE 12000
#define MAX_CANDIDATES 140
#define LDPC_ITERS 25
#define MAX_DECODED 100
#define MAX_EXPECTED 200

typedef struct {
    char text[128];  // Decoded message text (e.g., "VK4BLE OH8JK R-17")
    int score;       // Sync score (for ranking)
    float snr_db;    // Measured SNR in dB
    uint8_t symbols[FT8_NN];  // Decoded symbols (0-7) for Stage 2 reconstruction
    ftx_candidate_t cand;     // Candidate position (time/freq offset) for subtraction
    uint32_t hash;            // Message hash (for residual dedup vs baseline decodes)
} decoded_msg_t;

// Forward declarations for Stage 2 residual subtraction/decode (see ft8_stage2.c)
int ft8_stage2_run_loop(decoded_msg_t* decoded_list, int num_decoded,
                       monitor_t* mon, int sample_rate);
void ft8_stage2_scale_sweep(decoded_msg_t* decoded_list, int num_decoded,
                       monitor_t* mon, int sample_rate);

typedef struct {
    char text[128];  // Expected message text
    float snr_db;    // Documented SNR in dB (from .txt file)
} expected_msg_t;

// Global arrays to capture decoded messages from decode loop
decoded_msg_t decoded_list[MAX_DECODED];
int num_decoded = 0;

// Simple hash table for deduplication (from demo/decode_ft8.c)
typedef struct {
    char text[128];
    uint32_t hash;
} hashtable_entry_t;

hashtable_entry_t hashtable[MAX_DECODED];

int parse_expected_file(const char* path, expected_msg_t* expected, int max_expected)
{
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open %s\n", path);
        return -1;
    }

    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && count < max_expected) {
        // Parse: HHMMSS SNR CONF FREQ ~ MESSAGE...
        // Extract SNR from column 2
        int time_val;
        float snr_db;
        int parsed = sscanf(line, "%d %f", &time_val, &snr_db);
        if (parsed < 2) {
            expected[count].snr_db = 0.0f;  // Default if parse fails
        } else {
            expected[count].snr_db = snr_db;
        }

        // Find the ~ separator
        char* sep = strchr(line, '~');
        if (!sep) continue;

        // Message starts after ~ and space
        char* msg_start = sep + 1;
        while (*msg_start == ' ') msg_start++;

        // Trim trailing newline
        int msg_len = strlen(msg_start);
        while (msg_len > 0 && (msg_start[msg_len-1] == '\n' || msg_start[msg_len-1] == '\r')) {
            msg_len--;
        }

        // FT8 messages have max 3 space-separated tokens (e.g., "CALL1 CALL2 GRID" or "CQ CALL GRID")
        // Location info (like "Italy", "Spain") comes after as extra tokens
        // Count spaces and trim after the 3rd token
        int space_count = 0;
        int real_msg_len = msg_len;

        for (int j = 0; j < msg_len; j++) {
            if (msg_start[j] == ' ') {
                space_count++;
                if (space_count == 3) {  // 3 spaces = 4 tokens, trim at the 4th token
                    real_msg_len = j;
                    break;
                }
            }
        }

        // Trim trailing spaces
        while (real_msg_len > 0 && msg_start[real_msg_len-1] == ' ') {
            real_msg_len--;
        }

        if (real_msg_len > 0) {
            strncpy(expected[count].text, msg_start, sizeof(expected[count].text) - 1);
            expected[count].text[real_msg_len] = '\0';
            count++;
        }
    }
    fclose(f);
    return count;
}

// Check if two message texts match (ignoring case)
int messages_match(const char* a, const char* b)
{
    if (!a || !b) return 0;

    // Normalize and compare
    while (*a && *b) {
        // Skip spaces
        while (*a == ' ') a++;
        while (*b == ' ') b++;

        if (!*a || !*b) break;

        if (tolower(*a) != tolower(*b)) return 0;
        a++;
        b++;
    }

    // Both should be at end
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    return !*a && !*b;
}

int count_matches(decoded_msg_t* decoded, int num_decoded, expected_msg_t* expected, int num_expected)
{
    int matches = 0;

    // For each expected message, check if it was decoded
    for (int i = 0; i < num_expected; i++) {
        for (int j = 0; j < num_decoded; j++) {
            if (messages_match(expected[i].text, decoded[j].text)) {
                matches++;
                break;  // Count this expected message as matched
            }
        }
    }

    return matches;
}

// Modified decode function (copied from demo/decode_ft8.c, but captures to global array)
void decode_and_capture(const monitor_t* mon, struct tm* tm_slot_start)
{
    const ftx_waterfall_t* wf = &mon->wf;
    ftx_candidate_t candidate_list[MAX_CANDIDATES];
    int num_candidates = ftx_find_candidates(wf, MAX_CANDIDATES, candidate_list, 10);

    ftx_message_t decoded[MAX_DECODED];
    ftx_message_t* decoded_hashtable[MAX_DECODED];
    for (int i = 0; i < MAX_DECODED; ++i) {
        decoded_hashtable[i] = NULL;
    }

    num_decoded = 0;

    for (int idx = 0; idx < num_candidates && num_decoded < MAX_DECODED; ++idx) {
        const ftx_candidate_t* cand = &candidate_list[idx];

        ftx_message_t message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(wf, cand, LDPC_ITERS, &message, &status)) {
            continue;
        }

        // Deduplication hash table
        int idx_hash = message.hash % MAX_DECODED;
        int found_empty = 0;
        int found_dup = 0;
        do {
            if (decoded_hashtable[idx_hash] == NULL) {
                found_empty = 1;
            } else if ((decoded_hashtable[idx_hash]->hash == message.hash) &&
                       (0 == memcmp(decoded_hashtable[idx_hash]->payload, message.payload, sizeof(message.payload)))) {
                found_dup = 1;
            } else {
                idx_hash = (idx_hash + 1) % MAX_DECODED;
            }
        } while (!found_empty && !found_dup);

        if (found_empty) {
            memcpy(&decoded[idx_hash], &message, sizeof(message));
            decoded_hashtable[idx_hash] = &decoded[idx_hash];

            // Decode message text
            char text[128];
            ftx_message_offsets_t offsets;
            ftx_message_rc_t rc = ftx_message_decode(&message, NULL, text, &offsets);
            if (rc != FTX_MESSAGE_RC_OK) {
                snprintf(text, sizeof(text), "??");
            }

            // Capture to global array (including symbols for Stage 2)
            strncpy(decoded_list[num_decoded].text, text, sizeof(decoded_list[num_decoded].text) - 1);
            decoded_list[num_decoded].score = cand->score;
            decoded_list[num_decoded].snr_db = status.snr_db;
            memcpy(decoded_list[num_decoded].symbols, status.symbols, sizeof(status.symbols));
            decoded_list[num_decoded].cand = *cand;         // position for Stage 2 subtraction
            decoded_list[num_decoded].hash = message.hash;  // for residual dedup
            num_decoded++;
        }
    }
}

/// Stage 2: Run residual subtraction + decode to attempt rescuing weak signals
void test_stage2_concept(monitor_t* mon, int sample_rate)
{
    if (num_decoded == 0) {
        printf("\n=== Stage 2 ===\nNo baseline decodes to subtract; skipping.\n");
        return;
    }

    // Task 1: subtract all baseline decodes, re-search + re-decode the residual
    ft8_stage2_run_loop(decoded_list, num_decoded, mon, sample_rate);

    // Task 2: scale-factor tuning sweep
    ft8_stage2_scale_sweep(decoded_list, num_decoded, mon, sample_rate);
}

void analyze_snr_calibration(decoded_msg_t* decoded, int num_decoded, expected_msg_t* expected, int num_expected)
{
    if (num_expected == 0) {
        printf("(No expected messages for SNR calibration)\n");
        return;
    }

    printf("\n[DEBUG] analyze_snr_calibration called: num_decoded=%d, num_expected=%d\n", num_decoded, num_expected);

    float sum_offset = 0.0f;
    float sum_offset_sq = 0.0f;
    int matched_count = 0;

    printf("\nSNR Calibration Results:\n");
    printf("%-40s %8s %8s %8s %8s\n", "Message", "Doc(dB)", "Meas(dB)", "Error(dB)", "Status");
    printf("%-40s %8s %8s %8s %8s\n", "--------", "-------", "-------", "--------", "------");

    // DEBUG: Print first expected and first decoded to check format
    if (num_expected > 0 && num_decoded > 0) {
        printf("[DEBUG] First expected: '%s' (len %zu)\n", expected[0].text, strlen(expected[0].text));
        printf("[DEBUG] First decoded:  '%s' (len %zu)\n", decoded[0].text, strlen(decoded[0].text));
        printf("[DEBUG] Match result: %d\n", messages_match(expected[0].text, decoded[0].text));
    }

    // For each expected message, find its match and compare SNR
    for (int i = 0; i < num_expected; i++) {
        float doc_snr = expected[i].snr_db;
        float meas_snr = -99.0f;
        int found = 0;

        // DEBUG: first expected message - try all comparisons
        if (i == 0) {
            printf("[DEBUG] Searching for expected[0]='%s'\n", expected[i].text);
            for (int j = 0; j < num_decoded && j < 5; j++) {
                int match = messages_match(expected[i].text, decoded[j].text);
                printf("[DEBUG]   vs decoded[%d]='%s' -> %d\n", j, decoded[j].text, match);
            }
        }

        // Find matching decoded message
        for (int j = 0; j < num_decoded; j++) {
            if (messages_match(expected[i].text, decoded[j].text)) {
                meas_snr = decoded[j].snr_db;
                found = 1;
                if (i == 0) printf("[DEBUG] MATCHED at decoded[%d]='%s'\n", j, decoded[j].text);
                break;
            }
        }

        if (found && meas_snr > -99.0f) {
            float error = meas_snr - doc_snr;
            sum_offset += error;
            sum_offset_sq += error * error;
            matched_count++;

            // Print line
            char status[16] = "OK";
            if (fabsf(error) > 3.0f) strcpy(status, "BAD");
            else if (fabsf(error) > 1.5f) strcpy(status, "WARN");

            printf("%-40s %8.1f %8.1f %8.1f %8s\n",
                   expected[i].text, doc_snr, meas_snr, error, status);
        } else {
            printf("%-40s %8.1f %8s %8s %8s\n",
                   expected[i].text, doc_snr, "NOT DEC", "-", "-");
        }
    }

    if (matched_count > 0) {
        float mean_offset = sum_offset / matched_count;
        float variance = (sum_offset_sq / matched_count) - (mean_offset * mean_offset);
        float stddev = sqrtf(fmaxf(0.0f, variance));

        printf("\n--- Calibration Summary ---\n");
        printf("Matched: %d / %d (%.1f%%)\n", matched_count, num_expected, 100.0f * matched_count / num_expected);
        printf("Mean SNR offset: %.2f dB (measured - documented)\n", mean_offset);
        printf("Std deviation: %.2f dB\n", stddev);
        printf("Min/Max offset: %.2f / +%.2f dB\n", mean_offset - 2*stddev, mean_offset + 2*stddev);

        if (fabsf(mean_offset) < 1.0f) {
            printf("Result: EXCELLENT calibration\n");
        } else if (fabsf(mean_offset) < 2.0f) {
            printf("Result: GOOD calibration (systematic offset ~%.1f dB)\n", mean_offset);
        } else if (fabsf(mean_offset) < 3.0f) {
            printf("Result: FAIR (may need recalibration)\n");
        } else {
            printf("Result: POOR (recalibration needed)\n");
        }
    }
}

// Analyze the diagnostic CSV file
void analyze_diagnostics(const char* csv_path)
{
    FILE* f = fopen(csv_path, "r");
    if (!f) {
        printf("(No diagnostic data)\n");
        return;
    }

    // Statistics
    int total_candidates = 0;
    int fallback_triggered = 0;
    int single_success = 0;
    int fallback_success = 0;
    int final_success = 0;

    // Skip header
    char line[512];
    fgets(line, sizeof(line), f);

    // Parse each line
    while (fgets(line, sizeof(line), f)) {
        int fallback_trig, final_result;
        sscanf(line, "%*[^,],%*[^,],%*d,%*f,%*f,%*d,%d,%*d,%d,%*[^\n]",
               &fallback_trig, &final_result);

        total_candidates++;
        if (fallback_trig) fallback_triggered++;
        if (final_result) final_success++;

        // Check if single-symbol succeeded (fallback not triggered but final success)
        if (!fallback_trig && final_result) {
            single_success++;
        }

        // Check if fallback succeeded (fallback triggered and final success)
        if (fallback_trig && final_result) {
            fallback_success++;
        }
    }
    fclose(f);

    printf("Total candidates processed: %d\n", total_candidates);
    printf("Fallback triggered: %d (%.1f%%)\n", fallback_triggered,
           total_candidates > 0 ? 100.0f * fallback_triggered / total_candidates : 0);
    printf("Single-symbol succeeded: %d (%.1f%%)\n", single_success,
           total_candidates > 0 ? 100.0f * single_success / total_candidates : 0);
    printf("Fallback rescued: %d (%.1f%% of triggered)\n", fallback_success,
           fallback_triggered > 0 ? 100.0f * fallback_success / fallback_triggered : 0);
    printf("Total successful: %d (%.1f%%)\n", final_success,
           total_candidates > 0 ? 100.0f * final_success / total_candidates : 0);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.wav> <expected.txt>\n", argv[0]);
        return 1;
    }

    const char* wav_path = argv[1];
    const char* exp_path = argv[2];

    // Clear old diagnostic log
    remove("stage1_diag.csv");

    // Load WAV
    float signal[SAMPLE_RATE * 20];
    int num_samples = SAMPLE_RATE * 20;
    int sample_rate = SAMPLE_RATE;

    if (load_wav(signal, &num_samples, &sample_rate, wav_path) < 0) {
        fprintf(stderr, "ERROR: Failed to load %s\n", wav_path);
        return 1;
    }

    printf("=== FT8 Test Harness ===\n");
    printf("WAV: %s (%d samples @ %d Hz)\n", wav_path, num_samples, sample_rate);

    // Load expected decodes
    expected_msg_t expected[MAX_EXPECTED];
    int num_expected = parse_expected_file(exp_path, expected, MAX_EXPECTED);
    if (num_expected < 0) {
        return 1;
    }

    printf("Expected: %d decodes from %s\n\n", num_expected, exp_path);

    // Set filename for diagnostic logging
    diag_set_filename(wav_path);

    // Initialize monitor
    monitor_t mon;
    monitor_config_t config = {
        .f_min = 200,
        .f_max = 3000,
        .sample_rate = sample_rate,
        .time_osr = 2,
        .freq_osr = 2,
        .protocol = FTX_PROTOCOL_FT8
    };

    monitor_init(&mon, &config);

    // Process audio
    fprintf(stderr, "Processing: ");
    for (int frame_pos = 0; frame_pos + mon.block_size <= num_samples; frame_pos += mon.block_size) {
        monitor_process(&mon, signal + frame_pos);
        fprintf(stderr, ".");
    }
    fprintf(stderr, "\n");

    // Decode
    struct tm tm_slot = {0};
    decode_and_capture(&mon, &tm_slot);

    printf("\n=== Results ===\n");
    printf("Decoded: %d messages\n", num_decoded);
    printf("Expected: %d messages\n\n", num_expected);

    // Print decoded messages
    printf("Decoded list:\n");
    for (int i = 0; i < num_decoded; i++) {
        printf("  [%2d] %s\n", i+1, decoded_list[i].text);
    }

    printf("\nExpected list:\n");
    for (int i = 0; i < num_expected; i++) {
        printf("  [%2d] %s\n", i+1, expected[i].text);
    }

    // Count matches
    int matches = count_matches(decoded_list, num_decoded, expected, num_expected);

    printf("\n=== Score ===\n");
    printf("Matched: %d / %d\n", matches, num_expected);
    if (num_expected > 0) {
        printf("Success rate: %.1f%%\n", 100.0f * matches / num_expected);
    }

    // Close diagnostic log and analyze
    diag_log_close();

    printf("\n=== Stage 1 Diagnostics ===\n");
    analyze_diagnostics("stage1_diag.csv");

    printf("\n=== SNR Calibration ===\n");
    analyze_snr_calibration(decoded_list, num_decoded, expected, num_expected);

    test_stage2_concept(&mon, sample_rate);

    monitor_free(&mon);
    return 0;
}
