// ft8_test.c - Synthetic FT8 round-trip self-test.
//
// Encodes "CQ OZ1LAV JO45", synthesizes GFSK-shaped FT8 audio at 12 kHz /
// 1500 Hz base, pads to a full 15 s slot, feeds through ft8_lib's monitor
// + decoder, logs whether the original message was recovered plus
// per-stage timing.
//
// Step 2c of v0.10 plan. GFSK + slot padding mirrors demo/gen_ft8.c.

#include "ft8_test.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/decode.h"
#include "ft8/constants.h"
#include "common/monitor.h"

static const char *TAG = "ft8_test";

#define SR_HZ           12000.0f
#define F_BASE_HZ       1500.0f
#define SYMBOL_SAMPLES  1920        // SR_HZ * FT8_SYMBOL_PERIOD = 12000 * 0.16
#define NUM_SYMBOLS     79          // FT8_NN
#define BURST_SAMPLES   (SYMBOL_SAMPLES * NUM_SYMBOLS)        // 151680
#define SLOT_SAMPLES    180000                                 // 15 s @ 12 kHz
#define SILENCE_SAMPLES ((SLOT_SAMPLES - BURST_SAMPLES) / 2)   // 14160

#define FT8_SYMBOL_BT   2.0f        // GFSK BT factor for FT8
#define GFSK_CONST_K    5.336446f   // pi * sqrt(2 / log(2))

// gfsk_pulse / synth_gfsk are direct ports of demo/gen_ft8.c.
// Heap-buffer variants (no VLAs) so 600+ KB working sets land in PSRAM.

static void gfsk_pulse(int n_spsym, float symbol_bt, float *pulse)
{
    for (int i = 0; i < 3 * n_spsym; ++i) {
        float t = i / (float)n_spsym - 1.5f;
        float arg1 = GFSK_CONST_K * symbol_bt * (t + 0.5f);
        float arg2 = GFSK_CONST_K * symbol_bt * (t - 0.5f);
        pulse[i] = (erff(arg1) - erff(arg2)) / 2.0f;
    }
}

// dphi  : [n_sym * n_spsym + 2 * n_spsym] floats
// pulse : [3 * n_spsym] floats
// signal: [n_sym * n_spsym] floats
static void synth_gfsk(const uint8_t *symbols, int n_sym, float f0,
                       float symbol_bt, int n_spsym, int signal_rate,
                       float *dphi, float *pulse, float *signal)
{
    int n_wave = n_sym * n_spsym;
    float hmod = 1.0f;
    float dphi_peak = 2.0f * (float)M_PI * hmod / (float)n_spsym;

    // Constant carrier baseline.
    for (int i = 0; i < n_wave + 2 * n_spsym; ++i) {
        dphi[i] = 2.0f * (float)M_PI * f0 / (float)signal_rate;
    }

    gfsk_pulse(n_spsym, symbol_bt, pulse);

    // Per-symbol pulse-shaped frequency modulation.
    for (int i = 0; i < n_sym; ++i) {
        int ib = i * n_spsym;
        for (int j = 0; j < 3 * n_spsym; ++j) {
            dphi[j + ib] += dphi_peak * symbols[i] * pulse[j];
        }
    }
    // Dummy ramp-in / ramp-out symbols.
    for (int j = 0; j < 2 * n_spsym; ++j) {
        dphi[j]                    += dphi_peak * pulse[j + n_spsym] * symbols[0];
        dphi[j + n_sym * n_spsym]  += dphi_peak * pulse[j]            * symbols[n_sym - 1];
    }

    // Integrate phase to produce audio.
    float phi = 0.0f;
    for (int k = 0; k < n_wave; ++k) {
        signal[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2.0f * (float)M_PI);
    }

    // Raised-cosine envelope ramp on first/last n_spsym/8 samples.
    int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; ++i) {
        float env = (1.0f - cosf(2.0f * (float)M_PI * i / (2.0f * n_ramp))) / 2.0f;
        signal[i]                *= env;
        signal[n_wave - 1 - i]   *= env;
    }
}

static void ft8_self_test_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();

    // 1. Encode message text to payload.
    ftx_message_t msg;
    ftx_message_init(&msg);
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, "CQ OZ1LAV JO45");
    if (rc != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "encode failed rc=%d", rc);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "encoded payload OK");

    // 2. Payload to 79 tone symbols.
    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    // 3. Allocate PSRAM scratch for GFSK synthesis.
    int n_spsym = SYMBOL_SAMPLES;
    int n_wave  = BURST_SAMPLES;
    float *audio = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    float *dphi  = heap_caps_malloc((n_wave + 2 * n_spsym) * sizeof(float), MALLOC_CAP_SPIRAM);
    float *pulse = heap_caps_malloc(3 * n_spsym * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!audio || !dphi || !pulse) {
        ESP_LOGE(TAG, "PSRAM alloc failed (audio=%p dphi=%p pulse=%p)",
                 audio, dphi, pulse);
        if (audio) heap_caps_free(audio);
        if (dphi)  heap_caps_free(dphi);
        if (pulse) heap_caps_free(pulse);
        vTaskDelete(NULL);
        return;
    }

    // Zero the full slot, then fill the burst region with GFSK audio
    // centred between SILENCE_SAMPLES of leading + trailing silence.
    memset(audio, 0, SLOT_SAMPLES * sizeof(float));
    synth_gfsk(tones, NUM_SYMBOLS, F_BASE_HZ, FT8_SYMBOL_BT, n_spsym,
               (int)SR_HZ, dphi, pulse, audio + SILENCE_SAMPLES);

    // Add ~32 dB SNR additive noise across the whole slot. Mirrors the
    // realistic-receive intent from the original test; small enough not
    // to degrade decode but breaks any degenerate sync edge cases.
    for (int i = 0; i < SLOT_SAMPLES; i++) {
        float n = ((float)(rand() & 0x3FF) / 1024.0f - 0.5f) * 0.05f;
        audio[i] += n;
    }

    int64_t t_synth = esp_timer_get_time();
    ESP_LOGI(TAG, "synth (gfsk + 15s slot) done in %lld ms",
             (t_synth - t0) / 1000);

    // 4. Monitor setup (upstream defaults).
    monitor_t mon;
    monitor_config_t cfg = {
        .f_min       = 200.0f,
        .f_max       = 3000.0f,
        .sample_rate = (int)SR_HZ,
        .time_osr    = 2,
        .freq_osr    = 2,
        .protocol    = FTX_PROTOCOL_FT8,
    };
    monitor_init(&mon, &cfg);
    monitor_reset(&mon);

    // 5. Feed audio in block_size chunks (matches demo/decode_ft8.c).
    int blk = mon.block_size;
    int n_blocks_fed = 0;
    for (int pos = 0; pos + blk <= SLOT_SAMPLES; pos += blk) {
        monitor_process(&mon, &audio[pos]);
        n_blocks_fed++;
    }
    int64_t t_mon = esp_timer_get_time();
    ESP_LOGI(TAG, "monitor done in %lld ms, %d blocks fed (wf.num_blocks=%d)",
             (t_mon - t_synth) / 1000, n_blocks_fed, mon.wf.num_blocks);

    // 6. Candidate search.
    ftx_candidate_t heap[140];
    int n_cand = ftx_find_candidates(&mon.wf, 140, heap, 10);
    int64_t t_cand = esp_timer_get_time();
    ESP_LOGI(TAG, "found %d candidates in %lld ms",
             n_cand, (t_cand - t_mon) / 1000);

    int best_score = -32768, best_idx = -1;
    for (int i = 0; i < n_cand; i++) {
        if (heap[i].score > best_score) { best_score = heap[i].score; best_idx = i; }
    }
    if (best_idx >= 0) {
        ESP_LOGI(TAG, "top candidate: score=%d time_off=%d freq_off=%d",
                 heap[best_idx].score,
                 heap[best_idx].time_offset,
                 heap[best_idx].freq_offset);
    }

    // 7. Decode candidates; look for our own callsign + grid.
    int n_decoded = 0;
    bool found_self = false;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t out_msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon.wf, &heap[i], 60, &out_msg, &st)) {
            continue;
        }
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&out_msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            n_decoded++;
            ESP_LOGI(TAG, "decoded: '%s'", text);
            if (strstr(text, "OZ1LAV") && strstr(text, "JO45")) {
                found_self = true;
            }
        }
    }
    int64_t t_dec = esp_timer_get_time();
    ESP_LOGI(TAG, "decoded %d msgs in %lld ms; self-test %s",
             n_decoded, (t_dec - t_cand) / 1000,
             found_self ? "PASS" : "FAIL");

    monitor_free(&mon);
    heap_caps_free(pulse);
    heap_caps_free(dphi);
    heap_caps_free(audio);
    vTaskDelete(NULL);
}

// Public entry point: spawn the self-test on a dedicated 64 KB stack
// pinned to core 1. monitor_process declares a ~15 KB float frame buffer
// on its stack and kiss_fft.c::kf_work recurses deeper than expected;
// 32 KB was insufficient.
void ft8_self_test(void)
{
    BaseType_t rc = xTaskCreatePinnedToCore(
        ft8_self_test_task,
        "ft8_test",
        65536,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        1);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_self_test_task (rc=%d)", (int)rc);
    }
}
