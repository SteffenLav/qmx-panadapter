#include "cw_audio.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8388_codec.h"
#include "driver/i2s_std.h"
#include "dsps_fir.h"

#include "dsp.h"          // DSP_FFT_SIZE, DSP_SAMPLE_RATE_HZ, dsp_cw_* forward ring
#include "cat.h"          // cat_get_mode_str(), cat_get_cw_offset_hz()
#include "settings.h"

static const char *TAG = "cw_audio";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---- Tunables ----------------------------------------------------------
#define CW_FIR_LEN     63        // band-pass FIR taps (odd, linear phase)
#define CW_BPF_HALF_HZ 250       // half-bandwidth around the CW offset (=> ~500 Hz wide)
#define CW_OUT_CLAMP   20000.0f  // hard clip before int16 cast (headroom)
#define CW_DEF_OFFSET  700       // fallback CW offset if CAT hasn't reported one

// Per-sample AGC (@48 kHz): fast attack, slow release. Coefficients are
// 1-exp(-1/(tau*fs)); ~3 ms attack, ~150 ms release. Per-sample gain avoids
// the click you get from stepping a single gain at each 21 ms frame boundary.
#define CW_AGC_TARGET  10000.0f
#define CW_ATTACK      0.007f
#define CW_RELEASE     0.00014f
#define CW_GAIN_MAX    120.0f    // allow weak signals up
#define CW_NOISE_TC    0.00010f  // very slow noise-floor tracker (diag only for now)
// Squelch DISABLED for now (floor = 1.0 => always fully open). The previous
// noise-floor math settled at the signal average so SNR never exceeded 1 and
// it muted everything. Get clean audible AGC audio first, revisit squelch.
#define CW_SQ_LO       1.5f
#define CW_SQ_HI       2.8f
#define CW_SQ_FLOOR    1.0f

// ---- Module state ------------------------------------------------------
static volatile bool s_enabled = false;
static volatile uint8_t s_volume = 60;

static esp_codec_dev_handle_t s_codec = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;   // TX-only I2S channel (no RX/mic)
static volatile bool s_codec_ready = false;  // codec opened (at boot, pre-USB-host)
static TaskHandle_t s_task = NULL;

#ifndef CONFIG_BSP_I2S_NUM
#define CONFIG_BSP_I2S_NUM 1
#endif

// DSP work buffers (PSRAM — accessed once per ~21 ms frame, internal DRAM is
// already crowded by USB host / LVGL / FFT).
static int16_t *s_rxbuf = NULL;   // [DSP_FFT_SIZE*2] raw I/Q pairs from the ring
static float   *s_mix   = NULL;   // [DSP_FFT_SIZE]  fs/4-shifted real signal
static float   *s_filt  = NULL;   // [DSP_FFT_SIZE]  band-pass output
static int16_t *s_out   = NULL;   // [DSP_FFT_SIZE*2] interleaved L/R for codec
static int      s_mix_phase = 0;  // persistent fs/4 phase (0..3) across reads
static float   *s_coeff = NULL;   // [CW_FIR_LEN]
static float   *s_delay = NULL;   // [CW_FIR_LEN]
static fir_f32_t s_fir;

static float s_agc_env = 1.0f;
static float s_noise   = 1.0f;     // slow noise-floor estimate (for squelch)
static int   s_bpf_center_hz = 0;  // center the current FIR taps are built for

// ---- Band-pass FIR design (windowed sinc) ------------------------------
static inline float sinc_norm(float x)  // sin(pi x)/(pi x)
{
    if (fabsf(x) < 1e-6f) return 1.0f;
    float px = (float)M_PI * x;
    return sinf(px) / px;
}

// Build a linear-phase band-pass = (low-pass at fc2) - (low-pass at fc1),
// Hamming-windowed, normalised to unity gain at the band center.
static void cw_build_bpf(int center_hz)
{
    if (center_hz < CW_BPF_HALF_HZ + 50) center_hz = CW_BPF_HALF_HZ + 50;
    float fs  = (float)DSP_SAMPLE_RATE_HZ;
    float fc1 = (float)(center_hz - CW_BPF_HALF_HZ) / fs;
    float fc2 = (float)(center_hz + CW_BPF_HALF_HZ) / fs;
    int   M   = CW_FIR_LEN - 1;
    float half = M / 2.0f;

    for (int n = 0; n < CW_FIR_LEN; n++) {
        float m  = (float)n - half;
        float lp = 2.0f * fc2 * sinc_norm(2.0f * fc2 * m)
                 - 2.0f * fc1 * sinc_norm(2.0f * fc1 * m);
        float w  = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)n / (float)M); // Hamming
        s_coeff[n] = lp * w;
    }

    // Normalise to unity gain at the band-center frequency.
    float wc = 2.0f * (float)M_PI * (float)center_hz / fs;
    float re = 0.0f, im = 0.0f;
    for (int n = 0; n < CW_FIR_LEN; n++) {
        re += s_coeff[n] * cosf(wc * n);
        im += s_coeff[n] * sinf(wc * n);
    }
    float g = sqrtf(re * re + im * im);
    if (g > 1e-6f) {
        for (int n = 0; n < CW_FIR_LEN; n++) s_coeff[n] /= g;
    }

    // NB: dsps_fir_init_f32 zeroes (N+4) delay slots and the FIR uses that
    // padding, so s_delay MUST be allocated with CW_FIR_LEN+4 floats.
    memset(s_delay, 0, (CW_FIR_LEN + 4) * sizeof(float));
    dsps_fir_init_f32(&s_fir, s_coeff, s_delay, CW_FIR_LEN);
    s_bpf_center_hz = center_hz;
}

static inline bool mode_is_cw(void)
{
    const char *m = cat_get_mode_str();
    return (strcmp(m, "CW") == 0 || strcmp(m, "CW-R") == 0);
}

// ---- Demodulation task -------------------------------------------------
static void cw_audio_task(void *arg)
{
    (void)arg;
    bool active_prev = false;

    while (1) {
        // The codec/I2S output path is opened once at boot (cw_audio_preopen,
        // before the USB host claims the DMA-capable RAM). The task never
        // opens it — it just produces audio when active.
        bool active = s_enabled && s_codec_ready && mode_is_cw();

        if (!active) {
            if (active_prev) { dsp_cw_forward_enable(false); active_prev = false; }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (!active_prev) {
            s_agc_env = 1.0f;
            s_noise   = 1.0f;
            s_mix_phase = 0;
            dsp_cw_forward_enable(true);
            active_prev = true;
            ESP_LOGI(TAG, "CW audio on (vol=%d)", (int)s_volume);
        }

        // Track the live CW offset (re-design the band-pass if it moved).
        int off = cat_get_cw_offset_hz();
        if (off < 100 || off > 5000) off = CW_DEF_OFFSET;
        if (off != s_bpf_center_hz) cw_build_bpf(off);

        int pairs = (int)dsp_cw_read(s_rxbuf, DSP_FFT_SIZE, 60);
        if (pairs <= 0) {
            // Producer momentarily behind: feed the I2S a frame of silence so
            // the DMA never underruns (an underrun is an audible click). A
            // brief silence is far less objectionable than breaking up.
            memset(s_out, 0, DSP_FFT_SIZE * 2 * sizeof(int16_t));
            esp_codec_dev_write(s_codec, s_out, DSP_FFT_SIZE * 2 * (int)sizeof(int16_t));
            continue;
        }

        // fs/4 sign-flip down-mix: removes the QMX +12 kHz IF, putting the dial
        // at DC and the CW signal at +offset Hz (real part only; the -offset
        // mirror is rejected by the narrow band-pass below). Phase (s_mix_phase)
        // continues across reads so variable read sizes don't click.
        for (int i = 0; i < pairs; i++) {
            int ph = (s_mix_phase + i) & 3;
            float v;
            switch (ph) {
                case 0:  v =  (float)s_rxbuf[2 * i];     break;  // +I
                case 1:  v =  (float)s_rxbuf[2 * i + 1]; break;  // +Q
                case 2:  v = -(float)s_rxbuf[2 * i];     break;  // -I
                default: v = -(float)s_rxbuf[2 * i + 1]; break;  // -Q
            }
            s_mix[i] = v;
        }
        s_mix_phase = (s_mix_phase + pairs) & 3;

        // Narrow band-pass around the CW offset.
        dsps_fir_f32(&s_fir, s_mix, s_filt, pairs);

        // Per-sample AGC (smooth, no frame-boundary clicks) + noise-floor
        // squelch (gaps between CW elements go quiet instead of hissing).
        for (int i = 0; i < pairs; i++) {
            float a = fabsf(s_filt[i]);
            if (a > s_agc_env) s_agc_env += (a - s_agc_env) * CW_ATTACK;
            else               s_agc_env += (a - s_agc_env) * CW_RELEASE;

            // Slow tracker settles between noise and signal; SNR = env/noise.
            s_noise += (s_agc_env - s_noise) * CW_NOISE_TC;
            if (s_noise < 1.0f) s_noise = 1.0f;

            float gain = CW_AGC_TARGET / (s_agc_env + 1.0f);
            if (gain > CW_GAIN_MAX) gain = CW_GAIN_MAX;

            float snr = s_agc_env / s_noise;
            float sq  = (snr - CW_SQ_LO) / (CW_SQ_HI - CW_SQ_LO);
            if (sq < 0.0f) sq = 0.0f;
            else if (sq > 1.0f) sq = 1.0f;
            sq = CW_SQ_FLOOR + (1.0f - CW_SQ_FLOOR) * sq;   // gentle floor, not a hard gate

            float v = s_filt[i] * gain * sq;
            if (v >  CW_OUT_CLAMP) v =  CW_OUT_CLAMP;
            if (v < -CW_OUT_CLAMP) v = -CW_OUT_CLAMP;
            int16_t s = (int16_t)v;
            s_out[2 * i]     = s;   // L
            s_out[2 * i + 1] = s;   // R
        }

        // Blocking write paces the task to real time (≈21 ms per frame).
        esp_codec_dev_write(s_codec, s_out, pairs * 2 * (int)sizeof(int16_t));
    }
}

// ---- Public API --------------------------------------------------------
void cw_audio_init(void)
{
    if (s_task) return;  // already initialised

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    s_enabled = cfg.cw_audio_en;
    s_volume  = cfg.cw_audio_vol;

    // Work buffers in PSRAM (core-1 only). Putting all of these in internal
    // RAM starved the internal heap and destabilised boot, so only the
    // cross-core ring lives in internal RAM (see dsp.c).
    s_rxbuf = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_mix   = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),     MALLOC_CAP_SPIRAM);
    s_filt  = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),     MALLOC_CAP_SPIRAM);
    s_out   = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_coeff = heap_caps_malloc(CW_FIR_LEN * sizeof(float),       MALLOC_CAP_SPIRAM);
    s_delay = heap_caps_malloc((CW_FIR_LEN + 4) * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_rxbuf || !s_mix || !s_filt || !s_out || !s_coeff || !s_delay) {
        ESP_LOGE(TAG, "buffer alloc failed; CW audio disabled");
        return;
    }
    cw_build_bpf(CW_DEF_OFFSET);

    // Priority 6 (above fft_task=4): the task mostly blocks on the I2S write,
    // but when the DMA needs the next chunk it must run promptly or the buffer
    // underruns (audio breaks up). Brief, bursty CPU use — doesn't starve FFT.
    xTaskCreatePinnedToCore(cw_audio_task, "cw_audio", 4096, NULL, 6, &s_task, 1);
    ESP_LOGI(TAG, "init (enabled=%d vol=%d codec_ready=%d)",
             (int)s_enabled, (int)s_volume, (int)s_codec_ready);
}

void cw_audio_preopen(void)
{
    // Open the ES8388 / I2S output path NOW, before the USB host starts and
    // claims the DMA-capable internal RAM. I2S allocates its DMA descriptors
    // from that pool; doing it after the UAC stream is up fails (NO_MEM) and
    // esp_codec_dev_open then crashes on the un-checked error. We keep the
    // codec open for the whole session; the task only writes when CW audio is
    // active. Gated on the persisted enable flag so units that never use CW
    // audio don't claim I2S/DMA at all (and there's zero risk to USB host).
    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (!cfg.cw_audio_en) {
        ESP_LOGI(TAG, "preopen skipped (CW audio disabled)");
        return;
    }
    s_volume = cfg.cw_audio_vol;

    // --- Minimal TX-ONLY I2S channel (NOT bsp_audio_codec_speaker_init) ---
    // bsp_audio_init creates BOTH a TX and an RX (mic, TDM 4-slot) channel and
    // uses large default DMA buffers — two GDMA channels + several KB of DMA
    // RAM. That starves the USB host's endpoint allocation (CDC-ACM/CAT can't
    // claim its EPs). We only need playback, so create just the TX channel with
    // small DMA buffers: one GDMA channel, ~1.5 KB DMA, leaving room for USB.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear    = true;
    // ~40 ms of DMA buffering (6 x ~6.7 ms) so the playback survives the
    // jitter in how fast fft_task forwards I/Q frames (it's also doing the
    // FFT + LVGL push). Too small and the DMA underruns between frames, which
    // sounds like digitised/robotic dropouts. Still TX-only = one GDMA channel.
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 320;
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "preopen: i2s_new_channel failed");
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DSP_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "preopen: i2s_channel_init_std_mode failed");
        return;
    }
    // Leave the channel in READY state (not enabled) — esp_codec_dev_open
    // reconfigures + enables it.

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM, .tx_handle = s_tx_chan, .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0, .addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es8388_codec_cfg_t es_cfg = {
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .ctrl_if     = ctrl_if,
        .pa_pin      = -1,
    };
    const audio_codec_if_t *es_dev = es8388_codec_new(&es_cfg);
    if (!data_if || !ctrl_if || !es_dev) {
        ESP_LOGE(TAG, "preopen: codec interface init failed");
        return;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es_dev, .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) {
        ESP_LOGE(TAG, "preopen: esp_codec_dev_new failed");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .sample_rate     = DSP_SAMPLE_RATE_HZ,
    };
    int ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "preopen: codec_open failed (%d)", ret);
        return;
    }
    esp_codec_dev_set_out_vol(s_codec, (int)s_volume);
    s_codec_ready = true;
    ESP_LOGI(TAG, "preopen OK (TX-only I2S, codec ready, vol=%d, free_int=%u)",
             (int)s_volume, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void cw_audio_set_enabled(bool en)
{
    s_enabled = en;
    settings_set_cw_audio_en(en);
    if (en && !s_codec_ready) {
        // Codec is only opened at boot (before the USB host takes the DMA
        // RAM). Turning CW audio on mid-session can't open it safely, so it
        // takes effect after a restart.
        ESP_LOGW(TAG, "CW audio enabled — restart required to take effect");
    }
}

bool cw_audio_is_enabled(void) { return s_enabled; }

void cw_audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_codec) esp_codec_dev_set_out_vol(s_codec, (int)vol);
    settings_set_cw_audio_vol(vol);
}

uint8_t cw_audio_get_volume(void) { return s_volume; }
