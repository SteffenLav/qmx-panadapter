/*
 * WAV File Attenuator
 * Reduces signal amplitude to simulate weak signals
 * Usage: attenuate_wav <input.wav> <output.wav> <attenuation_db>
 * Example: attenuate_wav strong.wav weak.wav -12 (reduce by 12 dB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef struct {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input.wav> <output.wav> <attenuation_db>\n", argv[0]);
        fprintf(stderr, "Example: %s signal.wav weak.wav -12\n", argv[0]);
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = argv[2];
    float attenuation_db = atof(argv[3]);
    float attenuation_linear = powf(10.0f, attenuation_db / 20.0f);

    printf("Attenuating WAV file...\n");
    printf("Input: %s\n", input_file);
    printf("Output: %s\n", output_file);
    printf("Attenuation: %.1f dB (scale: %.3f)\n", attenuation_db, attenuation_linear);

    // Read input WAV
    FILE* fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "ERROR: Cannot open %s\n", input_file);
        return 1;
    }

    wav_header_t header;
    fread(&header, sizeof(wav_header_t), 1, fin);

    // Verify it's a WAV file
    if (header.riff[0] != 'R' || header.riff[1] != 'I' || header.riff[2] != 'F' || header.riff[3] != 'F') {
        fprintf(stderr, "ERROR: Not a valid WAV file\n");
        fclose(fin);
        return 1;
    }

    // Read audio data
    int num_samples = header.data_size / (header.bits_per_sample / 8);
    int16_t* samples = malloc(header.data_size);
    fread(samples, 1, header.data_size, fin);
    fclose(fin);

    // Apply attenuation
    for (int i = 0; i < num_samples; i++) {
        samples[i] = (int16_t)(samples[i] * attenuation_linear);
    }

    // Write output WAV
    FILE* fout = fopen(output_file, "wb");
    if (!fout) {
        fprintf(stderr, "ERROR: Cannot open %s for writing\n", output_file);
        return 1;
    }

    fwrite(&header, sizeof(wav_header_t), 1, fout);
    fwrite(samples, 1, header.data_size, fout);
    fclose(fout);

    printf("Wrote %s (%d samples attenuated)\n", output_file, num_samples);

    free(samples);
    return 0;
}
