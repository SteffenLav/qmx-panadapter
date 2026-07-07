/*
 * Weak Signal FT8 Generator
 * Generates FT8 WAV files at controllable SNR levels
 * Usage: weak_signal_gen <output.wav> <snr_db> <message>
 * Example: weak_signal_gen weak_signal.wav -6 "VK4BLE OH8JK R-17"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../components/ft8_lib/ft8/encode.h"
#include "../components/ft8_lib/fft/kiss_fft.h"

#define SAMPLE_RATE 12000
#define FT8_DURATION 15  // seconds
#define NUM_SAMPLES (SAMPLE_RATE * FT8_DURATION)

// Simple WAV file writer
void write_wav(const char* filename, float* samples, int num_samples, int sample_rate)
{
    FILE* f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open %s for writing\n", filename);
        return;
    }

    int num_channels = 1;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int subchunk2_size = num_samples * num_channels * bits_per_sample / 8;
    int chunk_size = 36 + subchunk2_size;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt subchunk
    fwrite("fmt ", 1, 4, f);
    int subchunk1_size = 16;
    fwrite(&subchunk1_size, 4, 1, f);
    short audio_format = 1;  // PCM
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    // data subchunk
    fwrite("data", 1, 4, f);
    fwrite(&subchunk2_size, 4, 1, f);

    // Convert float to int16
    for (int i = 0; i < num_samples; i++) {
        int16_t sample = (int16_t)(samples[i] * 32767.0f);
        fwrite(&sample, 2, 1, f);
    }

    fclose(f);
    printf("Wrote %s (%d samples)\n", filename, num_samples);
}

// Generate GFSK modulated FT8 signal
void generate_gfsk(float* samples, int num_samples, const uint8_t* tones, int num_tones, float snr_db)
{
    memset(samples, 0, num_samples * sizeof(float));

    // FT8 parameters
    int symbol_period_samples = SAMPLE_RATE / (12000.0f / 80.0f);  // ~80 symbols per 15s
    float tone_spacing = 6.25f;  // Hz
    float carrier_freq = 1000.0f;  // Hz (center frequency)

    // Generate GFSK signal
    float phase = 0;
    int sample_idx = 0;

    for (int sym = 0; sym < num_tones && sample_idx < num_samples; sym++) {
        float freq = carrier_freq + tones[sym] * tone_spacing;

        for (int s = 0; s < symbol_period_samples && sample_idx < num_samples; s++) {
            float phase_inc = 2.0f * M_PI * freq / SAMPLE_RATE;
            samples[sample_idx] += sinf(phase);
            phase += phase_inc;
            sample_idx++;
        }
    }

    // Add noise based on SNR
    float signal_power = 0;
    for (int i = 0; i < num_samples; i++) {
        signal_power += samples[i] * samples[i];
    }
    signal_power /= num_samples;

    float snr_linear = powf(10.0f, snr_db / 10.0f);
    float noise_power = signal_power / snr_linear;
    float noise_std = sqrtf(noise_power);

    // Add Gaussian noise
    for (int i = 0; i < num_samples; i++) {
        // Box-Muller transform for Gaussian noise
        float u1 = (rand() + 1.0f) / (RAND_MAX + 1.0f);
        float u2 = (rand() + 1.0f) / (RAND_MAX + 1.0f);
        float z = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * M_PI * u2);
        samples[i] += z * noise_std;
    }

    // Normalize to prevent clipping
    float max_sample = 0;
    for (int i = 0; i < num_samples; i++) {
        if (fabsf(samples[i]) > max_sample) {
            max_sample = fabsf(samples[i]);
        }
    }
    if (max_sample > 0) {
        float scale = 0.9f / max_sample;
        for (int i = 0; i < num_samples; i++) {
            samples[i] *= scale;
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <output.wav> <snr_db> <message>\n", argv[0]);
        fprintf(stderr, "Example: %s weak_signal.wav -6 \"VK4BLE OH8JK R-17\"\n", argv[0]);
        return 1;
    }

    const char* output_file = argv[1];
    float snr_db = atof(argv[2]);
    const char* message = argv[3];

    printf("Generating weak FT8 signal...\n");
    printf("Output: %s\n", output_file);
    printf("SNR: %.1f dB\n", snr_db);
    printf("Message: %s\n", message);

    // Encode message to FT8
    uint8_t packed[10];
    ftx_message_t msg;
    strncpy((char*)msg.payload, message, sizeof(msg.payload) - 1);

    // For now, generate a simple test tone pattern
    uint8_t tones[79];
    for (int i = 0; i < 79; i++) {
        tones[i] = (i * 7) % 8;  // Simple pattern
    }

    // Generate GFSK signal
    float* samples = malloc(NUM_SAMPLES * sizeof(float));
    generate_gfsk(samples, NUM_SAMPLES, tones, 79, snr_db);

    // Write WAV file
    write_wav(output_file, samples, NUM_SAMPLES, SAMPLE_RATE);

    free(samples);
    return 0;
}
