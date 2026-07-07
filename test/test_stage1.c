#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ft8/decode.h>
#include <ft8/message.h>
#include <common/monitor.h>
#include <common/wave.h>

// Test harness for Stage 1 (multi-symbol fallback decode)
// Usage: test_stage1 <input.wav> <expected.txt>
// Compares decoded messages against expected-decode file

#define SAMPLE_RATE 12000
#define MAX_CANDIDATES 140
#define LDPC_ITERS 25

typedef struct {
    char call1[20];
    char call2[20];
    char extra[20];
    int snr;
    int freq_offset;
} expected_msg_t;

int parse_expected_file(const char* path, expected_msg_t* msgs, int max_msgs)
{
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && count < max_msgs) {
        int hhmmss, snr, freq_offset;
        char call1[20], call2[20], extra[20], marker;

        // Parse: HHMMSS SNR CONF FREQ ~ CALL1 CALL2 EXTRA
        if (sscanf(line, "%d %d %*f %d %c %19s %19s %19s",
                   &hhmmss, &snr, &freq_offset, &marker,
                   call1, call2, extra) >= 6) {
            msgs[count].snr = snr;
            msgs[count].freq_offset = freq_offset;
            strncpy(msgs[count].call1, call1, sizeof(msgs[count].call1) - 1);
            strncpy(msgs[count].call2, call2, sizeof(msgs[count].call2) - 1);
            strncpy(msgs[count].extra, extra, sizeof(msgs[count].extra) - 1);
            count++;
        }
    }
    fclose(f);
    return count;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.wav> <expected.txt>\n", argv[0]);
        return 1;
    }

    const char* wav_path = argv[1];
    const char* exp_path = argv[2];

    // Load WAV file
    float signal[SAMPLE_RATE * 20];  // Up to 20 seconds
    int num_samples = SAMPLE_RATE * 20;
    int sample_rate = SAMPLE_RATE;

    if (load_wav(signal, &num_samples, &sample_rate, wav_path) < 0) {
        fprintf(stderr, "ERROR: Failed to load %s\n", wav_path);
        return 1;
    }

    printf("Loaded %s: %d samples @ %d Hz\n", wav_path, num_samples, sample_rate);

    // Load expected decodes
    expected_msg_t expected[200];
    int num_expected = parse_expected_file(exp_path, expected, 200);
    if (num_expected <= 0) {
        fprintf(stderr, "ERROR: Failed to parse %s\n", exp_path);
        return 1;
    }

    printf("Expected %d decodes from %s\n\n", num_expected, exp_path);

    // Initialize decoder
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

    printf("Decoded messages:\n");
    printf("(This is a stub - full decode output not yet wired)\n");
    printf("Test harness ready for Stage 1 validation\n");

    monitor_free(&mon);
    return 0;
}
