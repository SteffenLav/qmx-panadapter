#include "dsp.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

// Phase 5.6: enable DC blocker on I/Q stream before FFT (standard SDR hygiene)
// Set to 0 to bypass.
#ifndef DSP_DC_BLOCKER
#define DSP_DC_BLOCKER 1
#endif

// DSP_DB_CALIBRATION_OFFSET moved to dsp.h - spur_map.c has to undo it to get
// back to raw FFT power, and two copies of a calibration constant is exactly
// the kind of thing that drifts apart.
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "dsps_fft2r.h"
#include "dsps_wind_blackman_harris.h"
#include "dsps_wind_hann.h"
#include "dsps_wind_nuttall.h"
#include "dsps_fir.h"

#include "audio.h"
#include "ui_mode.h"
#include "iq_balance.h"
#include "spur_map.h"

// Linear-power averaging for spur_map.c's dial-nudge detector. Accumulated on
// the FFT task; armed and collected from the detection task.
static float   *s_avg_acc    = NULL;
static uint32_t s_avg_want   = 0;
static volatile uint32_t s_avg_have = 0;
static volatile bool s_avg_active = false;
static volatile bool s_avg_done   = false;

// The 2026-08-13 spur investigation's DCPROBE instrumentation lived here: a 1 Hz
// power-averaged spectrum with a top-N peak list, which is what identified the
// comb and its 16x-per-Hz sweep law. Removed now that spur_map.c ships and logs
// what it learns. If it is ever needed again, the shape that mattered was
// accumulating LINEAR power per bin over ~1 s before ranking - a single FFT frame
// is +/-13 dB Rayleigh-noisy and cannot tell a spur from luck.

static const char *TAG = "dsp";

#define STATS_PERIOD_MS  1000

// Buffers
static float *s_window   = NULL;
// FFT analysis window. 0=Blackman-Harris (default, lowest leakage, widest
// main lobe), 1=Hann (narrowest main lobe -> sharpest signals, higher
// skirts), 2=Nuttall (middle ground). Switchable at runtime from the
// Waterfall drawer; rebuilds s_window in place.
static uint8_t s_window_type = 0;

// Build the analysis window into s_window. Built into a scratch buffer then
// memcpy'd so the dsp task (which reads s_window without a lock) never sees a
// half-initialised window - worst case it reads one frame across the memcpy,
// a harmless single-frame blip.
static void dsp_build_window(uint8_t type)
{
    if (!s_window) return;
    static float scratch[DSP_FFT_SIZE];
    switch (type) {
    case 1:  dsps_wind_hann_f32(scratch, DSP_FFT_SIZE);            break;
    case 2:  dsps_wind_nuttall_f32(scratch, DSP_FFT_SIZE);         break;
    case 0:
    default: dsps_wind_blackman_harris_f32(scratch, DSP_FFT_SIZE); type = 0; break;
    }
    memcpy(s_window, scratch, DSP_FFT_SIZE * sizeof(float));
    s_window_type = type;
}

void dsp_set_window(uint8_t idx)
{
    if (idx > 2) idx = 0;
    if (idx == s_window_type) return;
    dsp_build_window(idx);
    ESP_LOGI(TAG, "FFT window -> %s",
             idx == 1 ? "Hann" : idx == 2 ? "Nuttall" : "Blackman-Harris");
}
static float *s_workbuf  = NULL;
static float *s_spectrum = NULL;

static SemaphoreHandle_t s_spectrum_mtx = NULL;
static bool s_have_spectrum = false;

static TaskHandle_t s_fft_task = NULL;
static uint32_t s_frames_this_period = 0;

// ---- CW audio out: real-time I/Q forward ring -------------------------------
// Producer = audio.c process_rx (every decoded USB sample); consumer =
// cw_audio_task. BYTEBUF so variable-size producer chunks stream contiguously
// and the consumer reads fixed pair counts with continuous fs/4 mixer phase.
// ~125 ms of 48 kHz stereo int16 in INTERNAL RAM. Internal (not PSRAM) is
// deliberate: the producer (core 0) and consumer (core 1) both hit this ring
// every sample; in PSRAM that cross-core traffic contends with process_rx's
// own PSRAM writes and halves the UAC drain rate. Internal RAM has no such
// penalty.
#define CW_RING_BYTES (24 * 1024)
static RingbufHandle_t s_cw_ring    = NULL;
static volatile bool   s_cw_forward = false;
static int64_t  s_period_start_us = 0;

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ----- v0.16.0 Zoom-FFT --------------------------------------------------
// 63 taps, was 31. The decimation filter cuts at 0.45/D while the display draws
// the whole decimated span out to 0.5/D, so the outer edge of every zoomed view
// is drawn from inside the filter's transition band - which is why both ends go
// dark when you zoom. Samuel W7STF measured it and read it correctly ("about
// 1000 Hz" at x4); computed from the 31-tap design the edge sat -16.6/-10.3/-8.0
// dB down at x2/x4/x8 over 1846/1237/928 Hz per side.
//
// Transition width goes as 1/N, so doubling the taps roughly halves it and pulls
// the corner in close to the drawn edge. ⛔ Do NOT "fix" this by moving the
// cutoff toward 0.5/D instead: that trades a dark edge for energy above Nyquist
// aliasing back in as FALSE SIGNALS, which is far worse. A dark edge is honest.
//
// Cost is bounded and was measured rather than assumed - see the ZOOMFIR log
// line below. dsps_fird_init_f32 needs exactly N delay floats (the +4 quirk is
// the non-decimating dsps_fir_init_f32), and the multiple-of-4/alignment
// constraints in that init are ESP32-S3-only, so an odd 63 is fine on the P4.
#define ZOOM_FIR_LEN 63

static SemaphoreHandle_t s_zoom_cfg_mtx  = NULL;

// Config, written by dsp_set_zoom() (UI/LVGL task), read by fft_task.
static volatile int   s_zoom_decim  = 1;   // 1, 2, 4, 8, 16
static volatile float s_zoom_residual = 1.0f;
static volatile float s_zoom_fshift_hz = 0.0f;
static volatile int   s_zoom_gen    = 0;

// Applied state, owned by fft_task only.
static int   s_zoom_applied_gen   = -1;
static float s_zoom_lpf_taps[ZOOM_FIR_LEN];
static float s_zoom_fir_delay_i[ZOOM_FIR_LEN];
static float s_zoom_fir_delay_q[ZOOM_FIR_LEN];
static fir_f32_t s_zoom_fir_i;
static fir_f32_t s_zoom_fir_q;
static float s_zoom_rot_re, s_zoom_rot_im;     // per-sample NCO rotation
static float s_zoom_phase_re, s_zoom_phase_im; // current NCO phase
static float s_zoom_mix_i[DSP_FFT_SIZE];
static float s_zoom_mix_q[DSP_FFT_SIZE];
static float s_zoom_dec_i[DSP_FFT_SIZE];
static float s_zoom_dec_q[DSP_FFT_SIZE];
static float *s_zoom_acc      = NULL;  // interleaved I/Q, DSP_FFT_SIZE complex
static float *s_zoom_workbuf  = NULL;  // interleaved I/Q, DSP_FFT_SIZE complex
// Double-buffered published spectrum (dB, DSP_FFT_SIZE each). fft_task
// writes into the non-ready buffer then flips s_zoom_ready_idx; readers
// (LVGL/render task) take whichever buffer is currently marked ready
// without copying or locking. Lock-free: a 32-bit aligned int read/write
// is atomic on this target, and the writer never touches the ready buffer.
static float *s_zoom_spectrum[2] = { NULL, NULL };
static volatile int s_zoom_ready_idx = -1;
static int    s_zoom_acc_idx  = 0;
// Zoom-FIR cost accounting (see the ZOOMFIR log line). fft_task-only.
static uint32_t s_zoomfir_us = 0;
static uint32_t s_zoomfir_n  = 0;

// Windowed-sinc lowpass, Hamming window. fc_norm = cutoff / sample_rate.
static void zoom_design_lpf(float *taps, int n_taps, float fc_norm)
{
    int M = n_taps - 1;
    float sum = 0.0f;
    for (int i = 0; i <= M; i++) {
        float k = (float)i - (float)M / 2.0f;
        float sinc;
        if (fabsf(k) < 1e-6f) {
            sinc = 2.0f * fc_norm;
        } else {
            sinc = sinf(2.0f * (float)M_PI * fc_norm * k) / ((float)M_PI * k);
        }
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)M);
        taps[i] = sinc * w;
        sum += taps[i];
    }
    for (int i = 0; i <= M; i++) taps[i] /= sum;
}

// ----- Step 3 v0.10 FT8 RX capture -----------------------------------------
// 31-tap LPF: scipy.signal.firwin(31, 4500/24000, window='hamming')
// Passband 0-3 kHz flat (<0.5 dB), -65 dB at 9 kHz (worst aliaser into
// 0-3 kHz after /4 decimation to 12 kHz).
static const float s_ft8_lpf_taps[31] = {
    +9.4083799097e-04f, +1.8869410051e-03f, +2.8691449162e-03f, +3.1405658090e-03f,
    +1.3076221170e-03f, -3.7668515379e-03f, -1.1670857504e-02f, -1.9524454627e-02f,
    -2.2180134993e-02f, -1.3814782554e-02f, +9.5396753619e-03f, +4.7587798377e-02f,
    +9.4688024813e-02f, +1.4084394523e-01f, +1.7463386062e-01f, +1.8703732996e-01f,
    +1.7463386062e-01f, +1.4084394523e-01f, +9.4688024813e-02f, +4.7587798377e-02f,
    +9.5396753619e-03f, -1.3814782554e-02f, -2.2180134993e-02f, -1.9524454627e-02f,
    -1.1670857504e-02f, -3.7668515379e-03f, +1.3076221170e-03f, +3.1405658090e-03f,
    +2.8691449162e-03f, +1.8869410051e-03f, +9.4083799097e-04f
};
static float       s_ft8_fir_delay[31];
static fir_f32_t   s_ft8_fir;

// Continuous FT8 capture pre-ring (boundary-discard fix). The producer
// (fft_task FT8 branch) mixes/decimates and appends decimated 12 kHz samples
// here EVERY window while in FT8 mode — whether or not a slot capture is armed.
// dsp_ft8_capture_begin() then backfills the boundary->arm gap out of this ring
// so each slot's window starts at the UTC boundary regardless of when the
// low-priority ft8_task woke up to arm it. Before this, the gap audio (the start
// of every FT8 signal, incl. its Costas sync array) was discarded, so any slot
// armed late (off=85-176 ms) decoded far fewer signals than the first (on-time)
// slot. s_pre_head is a free-running absolute count (wrap-safe via unsigned
// deltas; wraps only after ~25 days at 12 kHz).
#define FT8_PRE_CAP   180000              // 15 s @ 12 kHz — one full FT8 slot of slack
static float   *s_ft8_pre = NULL;
static volatile uint32_t s_pre_head      = 0;   // abs count of decimated samples produced
static uint32_t s_cap_read_head  = 0;   // abs ring pos the consumer has copied into dst
static uint32_t s_cap_begin_head = 0;   // s_pre_head at begin() (no-audio detection)
static bool     s_ft8_in_mode    = false;   // producer-side FT8-mode edge tracker
static int      s_ft8_smeter_tick = 0;

static float *s_ft8_dst    = NULL;
static volatile int s_ft8_idx    = 0;   // decimated samples copied into dst so far
static int    s_ft8_target = 0;
static volatile bool s_ft8_active = false;

// Transfer-quiet: while an outbound network transfer (QRZ/eQSL upload) runs,
// fft_task (priority 4) otherwise preempts the upload task (priority 3) every
// audio window, starving the TLS handshake's crypto until it times out. When
// set, fft_task drains the ring and idles, freeing the core for the transfer.
// Stopping the FFT also stops FT8 capture/decode (they're fed from here).
static volatile bool s_xfer_quiet = false;
// True once fft_task has ACTUALLY observed the quiet flag and drained the ring
// down. Callers that follow a quiet request with something the system must be
// idle for (esp_https_ota_finish()'s 1.4 MB verify) have to wait for this -
// setting the flag and proceeding immediately does nothing, because the flag is
// cooperative and fft_task may be mid-window when it is set. Learned the hard
// way: a verify started microseconds after the flag went up took the hardware
// watchdog twice.
static volatile bool s_xfer_quiet_settled = false;

void dsp_set_transfer_quiet(bool quiet)
{
    if (quiet && !s_xfer_quiet) s_xfer_quiet_settled = false;   // must be re-earned
    s_xfer_quiet = quiet;
}

bool dsp_transfer_quiet_settled(void) { return s_xfer_quiet && s_xfer_quiet_settled; }

static float s_ft8_mix_buf[DSP_FFT_SIZE];
static float s_ft8_dec_buf[DSP_FFT_SIZE / 4];
// Debug instrumentation: once-per-second log of FT8 branch activity.
static uint32_t s_ft8_iter_count = 0;
static uint32_t s_ft8_total_n_out = 0;
static int64_t  s_ft8_last_log_us = 0;

// Incremental capture API (v0.18 fast-decode + boundary-discard fix). arm /
// poll-progress / finalize so the caller runs the FT8 STFT (monitor_process)
// block-by-block *during* the slot. The audio is now sourced from the
// continuous pre-ring (see s_ft8_pre): begin() backfills the boundary->arm gap
// and progress() drains newly-produced samples into dst, so the window is
// anchored to the UTC boundary rather than to when ft8_task woke to arm.

// Copy `n` decimated samples starting at absolute ring position `from_abs` into
// s_ft8_dst[dst_off ..], handling the ring wrap.
static void ft8_ring_copy_to_dst(uint32_t from_abs, uint32_t n, uint32_t dst_off)
{
    uint32_t rpos  = from_abs % FT8_PRE_CAP;
    uint32_t first = FT8_PRE_CAP - rpos;
    if (first > n) first = n;
    memcpy(&s_ft8_dst[dst_off], &s_ft8_pre[rpos], first * sizeof(float));
    if (n > first)
        memcpy(&s_ft8_dst[dst_off + first], &s_ft8_pre[0], (n - first) * sizeof(float));
}

// Arm capture. dst MUST point to >= target_samples floats in PSRAM and stay
// valid until finish() returns. backfill_samples = how many decimated samples
// back from "now" the UTC boundary was (start_off_ms * 12) — begin() prepends
// that many from the pre-ring so the slot window starts at the boundary.
esp_err_t dsp_ft8_capture_begin(float *dst, uint32_t target_samples,
                                uint32_t backfill_samples)
{
    if (!dst || !s_ft8_pre) return ESP_ERR_INVALID_ARG;
    s_ft8_dst    = dst;
    s_ft8_target = (int)target_samples;

    __sync_synchronize();
    uint32_t head = s_pre_head;
    uint32_t bf   = backfill_samples;
    if (bf > head)           bf = head;            // no more than produced since entry
    if (bf > FT8_PRE_CAP)    bf = FT8_PRE_CAP;     // no more than the ring holds
    if (bf > target_samples) bf = target_samples;
    if (bf > 0) ft8_ring_copy_to_dst(head - bf, bf, 0);
    s_ft8_idx        = (int)bf;
    s_cap_read_head  = head;            // live tracking continues from the boundary+gap
    s_cap_begin_head = head;
    __sync_synchronize();
    s_ft8_active = true;
    // #51 instrumentation: absolute window placement in pre-ring sample space.
    // Consecutive `start` values should advance by exactly one slot of samples
    // (180000 FT8 / 90000 FT4) if the windows tile the stream perfectly; an
    // alternating short/long delta is a direct, sample-exact measure of the
    // per-slot window misalignment under investigation. Remove when #51 closes.
    ESP_LOGI(TAG, "FT8 arm: head=%u bf=%u start=%u",
             (unsigned)head, (unsigned)bf, (unsigned)(head - bf));
    return ESP_OK;
}

// Drain any pre-ring samples produced since the last call into dst, then return
// the decimated-12 kHz sample count committed to dst so far. Called from the
// capture (consumer) task only.
int dsp_ft8_capture_progress(void)
{
    if (!s_ft8_dst) return 0;
    __sync_synchronize();
    uint32_t head  = s_pre_head;
    uint32_t avail = head - s_cap_read_head;       // unsigned delta — wrap-safe
    if (avail > FT8_PRE_CAP) {
        // Producer lapped the consumer (ft8_task stalled > ~15 s). Pathological;
        // drop the overrun rather than copy corrupted (overwritten) ring data.
        ESP_LOGW(TAG, "FT8 pre-ring lapped (avail=%u) - consumer stalled", (unsigned)avail);
        s_cap_read_head = head - FT8_PRE_CAP;
        avail = FT8_PRE_CAP;
    }
    uint32_t space = (uint32_t)(s_ft8_target - s_ft8_idx);
    uint32_t take  = (avail < space) ? avail : space;
    if (take > 0) {
        ft8_ring_copy_to_dst(s_cap_read_head, take, (uint32_t)s_ft8_idx);
        s_ft8_idx       += (int)take;
        s_cap_read_head += take;
    }
    return s_ft8_idx;
}

// Finalize: final drain, disarm, zero-pad the tail (the slot's dead air after
// the FT8 signal). The caller's streaming loop has already run to the UTC
// boundary, so there is nothing to wait for here. Returns ESP_ERR_TIMEOUT only
// on a genuine audio failure (nothing captured / producer never advanced).
esp_err_t dsp_ft8_capture_finish(uint32_t timeout_ms)
{
    (void)timeout_ms;
    dsp_ft8_capture_progress();     // sweep up any last in-flight samples
    s_ft8_active = false;
    __sync_synchronize();

    int got = s_ft8_idx;
    if (got <= 0 || s_pre_head == s_cap_begin_head) {
        ESP_LOGW(TAG, "FT8 capture: no new audio this slot");
        return ESP_ERR_TIMEOUT;     // genuine audio failure — skip decode
    }
    if (got < s_ft8_target) {
        memset(&s_ft8_dst[got], 0, (size_t)(s_ft8_target - got) * sizeof(float));
    }
    ESP_LOGD(TAG, "FT8 capture: %d/%d samples (padded tail)", got, s_ft8_target);
    return ESP_OK;
}

// One-shot capture: arm (no backfill) + finalize. Retained for any non-streaming
// caller; not used by the live slot loop.
esp_err_t dsp_ft8_capture(float *dst_180000, uint32_t timeout_ms)
{
    esp_err_t e = dsp_ft8_capture_begin(dst_180000, 180000, 0);
    if (e != ESP_OK) return e;
    return dsp_ft8_capture_finish(timeout_ms);
}
// ----- end Step 3 v0.10 FT8 RX capture --------------------------------------

static void fft_task(void *arg);

// ⛔ xSemaphoreTake(NULL, t) is not a failed take - it is an immediate abort(),
// and a timeout argument does not save you (#154). dsp_init() runs at main.c:312
// but ui_init() is at 202, so there is a window of well over a hundred lines in
// which the UI and its LVGL timers are live and these mutexes do not exist yet.
// That is precisely the shape that rebooted the device through ft8_status.c.
//
// Treating "not created yet" as "could not take" is always safe here: every
// caller already has a failure path for a busy mutex, and pre-init there is by
// definition no concurrency to protect against.
static inline bool dsp_take(SemaphoreHandle_t m, TickType_t ticks)
{
    return m && xSemaphoreTake(m, ticks) == pdTRUE;
}

esp_err_t dsp_init(void)
{
    ESP_LOGI(TAG, "DSP init (Phase 4.2 - real-time FFT on audio ring buffer)");

    s_window = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_workbuf = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(float),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_spectrum = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Continuous FT8 capture pre-ring (PSRAM) — see s_ft8_pre.
    s_ft8_pre = heap_caps_malloc(FT8_PRE_CAP * sizeof(float),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_window || !s_workbuf || !s_spectrum || !s_ft8_pre) {
        ESP_LOGE(TAG, "Failed to allocate DSP buffers");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Allocated buffers: window=%d B, workbuf=%d B, spectrum=%d B",
             (int)(DSP_FFT_SIZE * sizeof(float)),
             (int)(DSP_FFT_SIZE * 2 * sizeof(float)),
             (int)(DSP_FFT_SIZE * sizeof(float)));

    dsp_build_window(s_window_type);
    ESP_LOGI(TAG, "Analysis window computed (type %u)", (unsigned)s_window_type);

    esp_err_t err = dsps_fft2r_init_fc32(NULL, DSP_FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "esp-dsp FFT initialized for %d-pt complex", DSP_FFT_SIZE);

    s_spectrum_mtx = xSemaphoreCreateMutex();
    if (!s_spectrum_mtx) return ESP_ERR_NO_MEM;

    // v0.16.0 zoom-FFT buffers (PSRAM; only touched when zoom > x1).
    s_zoom_acc        = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_zoom_workbuf    = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_zoom_spectrum[0] = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_zoom_spectrum[1] = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_zoom_acc || !s_zoom_workbuf || !s_zoom_spectrum[0] || !s_zoom_spectrum[1]) {
        ESP_LOGE(TAG, "Failed to allocate zoom-FFT buffers");
        return ESP_ERR_NO_MEM;
    }
    s_zoom_cfg_mtx = xSemaphoreCreateMutex();
    if (!s_zoom_cfg_mtx) return ESP_ERR_NO_MEM;

    // ---- Self-test: same as Phase 4.1 — validate the FFT works at boot ----
    const int test_bin = 100;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float phase = 2.0f * (float)M_PI * (float)test_bin * (float)i
                      / (float)DSP_FFT_SIZE;
        s_workbuf[2*i]     = cosf(phase) * s_window[i];
        s_workbuf[2*i + 1] = sinf(phase) * s_window[i];
    }
    dsps_fft2r_fc32(s_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_workbuf, DSP_FFT_SIZE);
    float peak_mag = 0.0f;
    int peak_idx = -1;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_workbuf[2*i];
        float im = s_workbuf[2*i + 1];
        float mag = re*re + im*im;
        if (mag > peak_mag) { peak_mag = mag; peak_idx = i; }
    }
    if (peak_idx == test_bin) {
        ESP_LOGI(TAG, "Self-test PASSED (bin %d -> bin %d)", test_bin, peak_idx);
    } else {
        ESP_LOGW(TAG, "Self-test FAILED (expected bin %d, got %d)", test_bin, peak_idx);
    }

    // Spawn the real-time FFT task on core 1 at lower priority than audio.
    BaseType_t ok = xTaskCreatePinnedToCore(
        fft_task, "fft_task", 6144, NULL, 4, &s_fft_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fft_task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "FFT task started");
    return ESP_OK;
}

esp_err_t dsp_get_spectrum(float *dst)
{
    if (!s_spectrum_mtx || !dst) return ESP_ERR_INVALID_ARG;
    if (!dsp_take(s_spectrum_mtx, pdMS_TO_TICKS(10))) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dst, s_spectrum, DSP_FFT_SIZE * sizeof(float));
    xSemaphoreGive(s_spectrum_mtx);
    return ESP_OK;
}

// Phase 5.10D: peak dBm in a window centered on the VFO bin.
// s_spectrum is the raw (non-fftshifted) post-FFT array, DC at index 0. The
// VFO sits at the +12 kHz IF offset, not at DC, so the caller passes the
// VFO's raw-array bin index (center_bin); we search +/-half_width_bins
// around it, wrapping at the array edges.
esp_err_t dsp_get_peak_dbm_around_vfo(int center_bin, int half_width_bins, float *peak_dbm)
{
    if (!s_spectrum_mtx || !peak_dbm) return ESP_ERR_INVALID_ARG;
    if (!dsp_take(s_spectrum_mtx, pdMS_TO_TICKS(10))) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    if (half_width_bins <= 0) half_width_bins = 1;
    if (half_width_bins > DSP_FFT_SIZE / 2) half_width_bins = DSP_FFT_SIZE / 2;
    center_bin = ((center_bin % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE;
    float peak = -1e9f;
    for (int d = -half_width_bins; d <= half_width_bins; d++) {
        int i = ((center_bin + d) % DSP_FFT_SIZE + DSP_FFT_SIZE) % DSP_FFT_SIZE;
        if (s_spectrum[i] > peak) peak = s_spectrum[i];
    }
    *peak_dbm = peak;
    xSemaphoreGive(s_spectrum_mtx);
    return ESP_OK;
}

// Snap-to-peak — see dsp.h for contract.
esp_err_t dsp_find_peak_hz_around(int32_t center_hz, int32_t radius_hz, int32_t if_offset_hz, int32_t *out_hz)
{
    if (!s_spectrum_mtx || !out_hz) return ESP_ERR_INVALID_ARG;
    if (radius_hz <= 0) { *out_hz = center_hz; return ESP_OK; }

    if (!dsp_take(s_spectrum_mtx, pdMS_TO_TICKS(10))) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        *out_hz = center_hz;
        return ESP_ERR_NOT_FOUND;
    }

    // Convert "Hz from dial" to "DSP bin index".
    // DSP bin 0 = audio DC; the QMX dial maps to if_offset_hz in baseband (+12 kHz
    // IF in SSB/data, +12 kHz + CW LO offset + trim in CW — the same value the
    // display centers on). A touch at center_hz from dial maps to baseband freq
    // (center_hz + if_offset_hz) Hz → bin (.../bin_width). Negative bins wrap to N - |bin|.
    // Using the bare 12 kHz here in CW left the search window ~640 Hz off the tapped
    // carrier, so snap silently failed in CW (worst when zoomed in, where the window
    // is narrow). Passing the mode-aware offset keeps the window centered on the tap.
    const float bin_width = (float)DSP_SAMPLE_RATE_HZ / (float)DSP_FFT_SIZE;  // 46.875 Hz
    const int N = DSP_FFT_SIZE;

    int center_bin = (int)((center_hz + if_offset_hz) / bin_width);  // truncation OK; we search a window
    int radius_bins = (int)(radius_hz / bin_width);
    if (radius_bins < 1) radius_bins = 1;

    // Search [center_bin - radius_bins, center_bin + radius_bins], wrapping mod N.
    // Track the peak's offset (in bins) FROM the search center, not its absolute
    // bin, so the result is always within ±radius_hz of the tapped position.
    float peak_db = -1e9f;
    int   peak_d  = 0;
    float sum_db = 0.0f;
    int   count = 0;
    for (int d = -radius_bins; d <= radius_bins; d++) {
        int b = ((center_bin + d) % N + N) % N;  // positive modulo
        float v = s_spectrum[b];
        sum_db += v;
        count++;
        if (v > peak_db) { peak_db = v; peak_d = d; }
    }
    float mean_db = (count > 0) ? (sum_db / (float)count) : -120.0f;
    xSemaphoreGive(s_spectrum_mtx);

    // Only snap if the peak is meaningfully above local mean (avoids snapping to noise).
    if (peak_db - mean_db < 3.0f) {
        *out_hz = center_hz;
        return ESP_OK;
    }

    // Return the peak relative to the tapped position. The old code unwrapped the
    // peak's *absolute* bin to a signed frequency, which aliased once
    // center_hz+12 kHz crossed the +24 kHz Nyquist edge (taps in the right ~quarter
    // of the 48 kHz display) — wrapping to a large NEGATIVE frequency, so a tap to
    // the RIGHT tuned DOWN (reported by Ian G4LXX). Offsetting from center_hz keeps
    // the snap inside ±radius_hz by construction, so direction is always preserved.
    *out_hz = center_hz + (int32_t)lroundf((float)peak_d * bin_width);
    return ESP_OK;
}

static void log_stats(float min_db, float max_db, float mean_db)
{
    int64_t now = esp_timer_get_time();
    if (s_period_start_us == 0) { s_period_start_us = now; return; }
    if (now - s_period_start_us < STATS_PERIOD_MS * 1000) return;

    uint32_t frames = s_frames_this_period;
    s_frames_this_period = 0;
    s_period_start_us = now;
    ESP_LOGD(TAG, "Spectrum: min=%.1f dBm, max=%.1f dBm, mean=%.1f dBm, frames=%u/s",
             min_db, max_db, mean_db, (unsigned)frames);
}

// Shared DC-blocker/window/FFT/dB/publish path, used both for the normal
// panadapter pipeline and periodically during FT8 capture to keep the
// S-meter (dsp_get_peak_dbm_around_vfo reads s_spectrum) updating.
static void compute_and_publish_spectrum(int16_t *samples, float *tmp_spectrum,
                                          float *last_min, float *last_max, float *last_mean)
{
#if DSP_DC_BLOCKER
    {
        static float dc_state_I = 0.0f;
        static float dc_state_Q = 0.0f;
        const float alpha = 0.9869f;
        for (int i = 0; i < DSP_FFT_SIZE; i++) {
            float xI = (float)samples[2*i];
            float xQ = (float)samples[2*i + 1];
            float cI = xI + dc_state_I * alpha;
            float cQ = xQ + dc_state_Q * alpha;
            float yI = cI - dc_state_I;
            float yQ = cQ - dc_state_Q;
            dc_state_I = cI;
            dc_state_Q = cQ;
            samples[2*i]     = (int16_t)yI;
            samples[2*i + 1] = (int16_t)yQ;
        }
    }
#endif
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float I = (float)samples[2*i];
        float Q = (float)samples[2*i + 1];
        float w = s_window[i];
        s_workbuf[2*i]     = I * w;
        s_workbuf[2*i + 1] = Q * w;
    }

    dsps_fft2r_fc32(s_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_workbuf, DSP_FFT_SIZE);

    const float floor_mag2 = 1e-12f;
    float sum_db = 0.0f;
    float min_db = +1e9f;
    float max_db = -1e9f;

    // Linear power first, so the spur subtraction can work in the domain where
    // powers add. Subtracting in dB would remove the wrong amount from any bin
    // that also carries a real signal.
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_workbuf[2*i];
        float im = s_workbuf[2*i + 1];
        float m = re*re + im*im;
        tmp_spectrum[i] = (m < floor_mag2) ? floor_mag2 : m;
    }
    spur_map_apply(tmp_spectrum, DSP_FFT_SIZE);

    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float mag2 = tmp_spectrum[i];
        if (mag2 < floor_mag2) mag2 = floor_mag2;
        if (s_avg_active) s_avg_acc[i] += mag2;
        float db = 10.0f * log10f(mag2) + DSP_DB_CALIBRATION_OFFSET;
        tmp_spectrum[i] = db;
        sum_db += db;
        if (db < min_db) min_db = db;
        if (db > max_db) max_db = db;
    }
    if (s_avg_active && ++s_avg_have >= s_avg_want) {
        s_avg_active = false;
        s_avg_done   = true;
    }
    *last_min = min_db;
    *last_max = max_db;
    *last_mean = sum_db / (float)DSP_FFT_SIZE;

    if (dsp_take(s_spectrum_mtx, pdMS_TO_TICKS(10))) {
        memcpy(s_spectrum, tmp_spectrum, DSP_FFT_SIZE * sizeof(float));
        s_have_spectrum = true;
        xSemaphoreGive(s_spectrum_mtx);
    }

    s_frames_this_period++;
    log_stats(*last_min, *last_max, *last_mean);
}

void dsp_avg_start(uint32_t frames)
{
    if (!s_avg_acc) {
        s_avg_acc = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_avg_acc) return;
    }
    if (frames == 0) frames = 1;
    s_avg_done = false;
    memset(s_avg_acc, 0, DSP_FFT_SIZE * sizeof(float));
    s_avg_have = 0;
    s_avg_want = frames;
    s_avg_active = true;      // set last: the FFT task tests this to start
}

bool dsp_avg_ready(float *dst)
{
    if (!s_avg_done || !dst || !s_avg_acc) return false;
    float inv = 1.0f / (float)((s_avg_have > 0) ? s_avg_have : 1);
    for (int i = 0; i < DSP_FFT_SIZE; i++)
        dst[i] = 10.0f * log10f(s_avg_acc[i] * inv) + DSP_DB_CALIBRATION_OFFSET;
    s_avg_done = false;
    return true;
}

static void zoom_run_fft(void)
{
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float w = s_window[i];
        s_zoom_workbuf[2*i]     = s_zoom_acc[2*i]     * w;
        s_zoom_workbuf[2*i + 1] = s_zoom_acc[2*i + 1] * w;
    }
    dsps_fft2r_fc32(s_zoom_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_zoom_workbuf, DSP_FFT_SIZE);

    // Write into the buffer that isn't currently published (index 0 the
    // first time, when s_zoom_ready_idx is still -1).
    int ridx = s_zoom_ready_idx;
    int widx = (ridx == 0) ? 1 : 0;
    float *out = s_zoom_spectrum[widx];
    const float *prev = (ridx >= 0) ? s_zoom_spectrum[ridx] : NULL;

    // Light EMA across successive zoom-FFT frames to take the edge off
    // frame-to-frame jumps at high decimation (slower cadence). Too low an
    // alpha smears/blurs the waterfall (signals visibly trail/fade), so
    // keep this fairly high - it's a touch-up, not the main smoothing.
    const float alpha = 0.6f;
    const float floor_mag2 = 1e-12f;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_zoom_workbuf[2*i];
        float im = s_zoom_workbuf[2*i + 1];
        float mag2 = re*re + im*im;
        if (mag2 < floor_mag2) mag2 = floor_mag2;
        float db = 10.0f * log10f(mag2) + DSP_DB_CALIBRATION_OFFSET;
        out[i] = prev ? (alpha * db + (1.0f - alpha) * prev[i]) : db;
    }
    s_zoom_ready_idx = widx;
}

// Mix the pan-center to DC, LPF + decimate by D, accumulate DSP_FFT_SIZE
// decimated complex samples and run a second FFT. `samples` is the same
// (already DC-blocked) buffer compute_and_publish_spectrum just used.
static void zoom_process(const int16_t *samples)
{
    int D;
    int gen;
    float fshift;
    if (!dsp_take(s_zoom_cfg_mtx, 0)) return;
    D = s_zoom_decim;
    gen = s_zoom_gen;
    fshift = s_zoom_fshift_hz;
    xSemaphoreGive(s_zoom_cfg_mtx);

    if (D <= 1) {
        s_zoom_ready_idx = -1;
        s_zoom_applied_gen = -1;  // force re-init if D goes back up
        return;
    }

    if (gen != s_zoom_applied_gen) {
        float cutoff_norm = 0.45f / (float)D;
        zoom_design_lpf(s_zoom_lpf_taps, ZOOM_FIR_LEN, cutoff_norm);
        memset(s_zoom_fir_delay_i, 0, sizeof(s_zoom_fir_delay_i));
        memset(s_zoom_fir_delay_q, 0, sizeof(s_zoom_fir_delay_q));
        dsps_fird_init_f32(&s_zoom_fir_i, s_zoom_lpf_taps, s_zoom_fir_delay_i, ZOOM_FIR_LEN, D);
        dsps_fird_init_f32(&s_zoom_fir_q, s_zoom_lpf_taps, s_zoom_fir_delay_q, ZOOM_FIR_LEN, D);

        float omega = -2.0f * (float)M_PI * fshift / (float)DSP_SAMPLE_RATE_HZ;
        s_zoom_rot_re = cosf(omega);
        s_zoom_rot_im = sinf(omega);
        s_zoom_phase_re = 1.0f;
        s_zoom_phase_im = 0.0f;
        s_zoom_acc_idx = 0;
        s_zoom_ready_idx = -1;
        s_zoom_applied_gen = gen;
    }

    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float I = (float)samples[2*i];
        float Q = (float)samples[2*i + 1];
        float ri = s_zoom_phase_re, rq = s_zoom_phase_im;
        s_zoom_mix_i[i] = I * ri - Q * rq;
        s_zoom_mix_q[i] = I * rq + Q * ri;
        float nr = ri * s_zoom_rot_re - rq * s_zoom_rot_im;
        float nq = ri * s_zoom_rot_im + rq * s_zoom_rot_re;
        s_zoom_phase_re = nr;
        s_zoom_phase_im = nq;
    }
    // Periodic renormalization keeps the NCO phasor on the unit circle.
    float mag2 = s_zoom_phase_re * s_zoom_phase_re + s_zoom_phase_im * s_zoom_phase_im;
    if (mag2 < 0.95f || mag2 > 1.05f) {
        float inv = 1.0f / sqrtf(mag2);
        s_zoom_phase_re *= inv;
        s_zoom_phase_im *= inv;
    }

    int n_out_target = DSP_FFT_SIZE / D;
    // Measured, not assumed: this runs on fft_task, the audio ring's SOLE
    // consumer, and #51 is the standing reminder of what starving it costs. The
    // window period is 1024/48000 = 21.3 ms, so what matters is this figure
    // against that. Silent unless zoomed (D > 1), i.e. never in normal use.
    int64_t t0 = esp_timer_get_time();
    int n_out_i = dsps_fird_f32(&s_zoom_fir_i, s_zoom_mix_i, s_zoom_dec_i, n_out_target);
    int n_out_q = dsps_fird_f32(&s_zoom_fir_q, s_zoom_mix_q, s_zoom_dec_q, n_out_target);
    s_zoomfir_us += (uint32_t)(esp_timer_get_time() - t0);
    if (++s_zoomfir_n >= 47) {   // ~1 s of windows at 46.9 Hz
        ESP_LOGI(TAG, "ZOOMFIR: %u taps D=%d  %.3f ms/window avg (window period 21.3 ms)",
                 (unsigned)ZOOM_FIR_LEN, D,
                 (double)s_zoomfir_us / (double)s_zoomfir_n / 1000.0);
        s_zoomfir_us = 0;
        s_zoomfir_n  = 0;
    }
    int n_out = (n_out_i < n_out_q) ? n_out_i : n_out_q;

    for (int i = 0; i < n_out; i++) {
        if (s_zoom_acc_idx < DSP_FFT_SIZE) {
            s_zoom_acc[2*s_zoom_acc_idx]     = s_zoom_dec_i[i];
            s_zoom_acc[2*s_zoom_acc_idx + 1] = s_zoom_dec_q[i];
            s_zoom_acc_idx++;
        }
        if (s_zoom_acc_idx >= DSP_FFT_SIZE) {
            zoom_run_fft();
            s_zoom_acc_idx = 0;
        }
    }
}

// Update the zoom-FFT configuration. Called from the UI/LVGL task on every
// zoom/pan change; fft_task picks up the new generation on its next iteration.
void dsp_set_zoom(float zoom_factor, int pan_offset_bins, int if_bin_shift)
{
    int D = 1;
    if (zoom_factor >= 16.0f)      D = 16;
    else if (zoom_factor >= 8.0f)  D = 8;
    else if (zoom_factor >= 4.0f)  D = 4;
    else if (zoom_factor >= 2.0f)  D = 2;
    float residual = zoom_factor / (float)D;

    const int N = DSP_FFT_SIZE;
    int c = ((if_bin_shift + pan_offset_bins) % N + N) % N;
    int signed_bin = (c >= N / 2) ? c - N : c;
    float fshift_hz = (float)signed_bin * (float)DSP_SAMPLE_RATE_HZ / (float)N;

    if (!s_zoom_cfg_mtx) return;
    if (dsp_take(s_zoom_cfg_mtx, portMAX_DELAY)) {
        s_zoom_decim     = D;
        s_zoom_residual  = residual;
        s_zoom_fshift_hz = fshift_hz;
        s_zoom_gen++;
        xSemaphoreGive(s_zoom_cfg_mtx);
    }
}

int dsp_get_zoom_decim(void)
{
    return s_zoom_decim;
}

float dsp_get_zoom_residual(void)
{
    return s_zoom_residual;
}

const float *dsp_get_zoom_spectrum(void)
{
    if (s_zoom_decim <= 1) return NULL;
    int idx = s_zoom_ready_idx;
    if (idx < 0) return NULL;
    return s_zoom_spectrum[idx];
}

// ---- CW audio out: real-time forward-ring API -------------------------
void dsp_cw_forward_enable(bool en)
{
    if (en && s_cw_ring == NULL) {
        // BYTEBUF ring in INTERNAL RAM (see CW_RING_BYTES note re: PSRAM
        // cross-core contention throttling the UAC drain).
        s_cw_ring = xRingbufferCreateWithCaps(
            CW_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_cw_ring == NULL) {
            ESP_LOGE(TAG, "cw_ring alloc failed");
            return;
        }
        ESP_LOGI(TAG, "cw_ring created (%d bytes)", CW_RING_BYTES);
    }
    s_cw_forward = en;
}

// Producer: called by audio.c for every decoded chunk. Best-effort (0-tick);
// if the consumer is briefly behind, the oldest unread audio is the loss.
void dsp_cw_forward(const int16_t *pairs, size_t n_pairs)
{
    if (!s_cw_forward || s_cw_ring == NULL || pairs == NULL || n_pairs == 0) return;
    xRingbufferSend(s_cw_ring, pairs, n_pairs * 2 * sizeof(int16_t), 0);
}

// Consumer: fill up to n_pairs (loops over BYTEBUF wrap). Returns pairs read.
size_t dsp_cw_read(int16_t *dst, size_t n_pairs, uint32_t timeout_ms)
{
    if (s_cw_ring == NULL || dst == NULL || n_pairs == 0) return 0;
    size_t want_bytes = n_pairs * 2 * sizeof(int16_t);
    size_t got_bytes  = 0;
    uint8_t *d = (uint8_t *)dst;
    // First read blocks up to timeout; the wrap remainder is non-blocking.
    while (got_bytes < want_bytes) {
        size_t chunk = 0;
        uint32_t to = (got_bytes == 0) ? timeout_ms : 0;
        void *item = xRingbufferReceiveUpTo(s_cw_ring, &chunk, pdMS_TO_TICKS(to),
                                            want_bytes - got_bytes);
        if (item == NULL) break;
        memcpy(d + got_bytes, item, chunk);
        vRingbufferReturnItem(s_cw_ring, item);
        got_bytes += chunk;
    }
    return got_bytes / (2 * sizeof(int16_t));
}

static void fft_task(void *arg)
{
    // Local scratch buffers
    static int16_t samples[DSP_FFT_SIZE * 2];   // 1024 stereo pairs = 2048 int16
    static float tmp_spectrum[DSP_FFT_SIZE];     // dB output, local before copying

    // Track most recent stats for periodic logging
    float last_min = 0, last_max = 0, last_mean = 0;

    while (1) {
        // While a network transfer is in flight, give up the CPU: drain the
        // ring but skip all FFT/FT8 work. This stops fft_task (pri 4) from
        // preempting the transfer, and cascades to halt FT8 capture/decode.
        // Resumes the instant the flag clears.
        //
        // ⚠ THE OLD DRAIN COULD NOT KEEP UP, and its comment claimed it could.
        // It read ONE window (1024 pairs) and then slept 50 ms - about 20,000
        // pairs/s against the ~48,000 pairs/s the QMX actually produces. So it
        // fell behind more than 2:1 and the ring overflowed continuously for
        // the whole of every upload and every OTA download: measured on
        // hardware 2026-08-23 as "DROPPED=17904 (ring full)" then "DROPPED=28128"
        // one second apart. That in turn keeps audio_task (pri 6, core 0,
        // polling with no delay) in a discard loop it was never meant to run.
        //
        // Drain until the backlog is actually gone, then sleep briefly. The
        // guard bounds the loop in case production genuinely outruns us, so a
        // fast producer can never turn this into a spin.
        if (s_xfer_quiet) {
            uint32_t guard = 0;
            while (audio_ring_backlog_pairs() >= DSP_FFT_SIZE && guard++ < 64) {
                audio_read_samples(samples, DSP_FFT_SIZE, 0);
            }
            s_xfer_quiet_settled = true;   // observed the flag, ring is down
            // ⚠ 50 ms, NOT 5. Draining properly is the fix; waking 200x/s to
            // check is a regression - "quiet" has to mean IDLE, and fft_task
            // shares core 1 with the OTA task. A first version of this fix used
            // 5 ms and made the quiet state ten times busier than the one it
            // replaced, while reporting itself settled because the ring was
            // empty. Emptying the ring and being idle are not the same claim.
            //
            // 50 ms is safe with a real drain behind it: ~48,000 pairs/s makes
            // ~2,400 pairs in that window, so the loop above does two reads and
            // stops, where the old single-read version fell behind 2:1 forever.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Block until we have a full FFT window of stereo pairs (1024 pairs).
        // audio_read_samples may return less than requested - loop until full.
        size_t got = 0;
        while (got < DSP_FFT_SIZE) {
            size_t want = DSP_FFT_SIZE - got;
            size_t r = audio_read_samples(&samples[got * 2], want, 50);
            if (r == 0) {
                // No data yet (audio not streaming) - keep waiting
                continue;
            }
            got += r;
        }

        // FT8 continuous-capture branch. While in FT8 mode we ALWAYS mix the
        // QMX +12 kHz IF to DC (fs/4 sign-flip), decimate /4, and append the
        // decimated 12 kHz samples to the continuous pre-ring — whether or not a
        // slot capture is currently armed. This closes the old boundary->arm
        // discard gap (see s_ft8_pre): the low-priority ft8_task arms each slot's
        // capture 85-176 ms after the UTC boundary, and this branch used to
        // DISCARD that gap, throwing away the start of every FT8 signal (its
        // Costas sync array), so late-armed slots decoded far fewer signals than
        // the first, on-time slot. The gap audio now lives in the ring and
        // dsp_ft8_capture_begin() backfills it. dsp_ft8_capture_progress()
        // (consumer task) copies the ring into the caller's dst.
        if (ui_mode_get() == UI_MODE_FT8) {
            // FT8-mode entry edge: reset the decimation FIR + ring so a stale
            // delay line / old sample counter can't leak into slot 0.
            if (!s_ft8_in_mode) {
                memset(s_ft8_fir_delay, 0, sizeof(s_ft8_fir_delay));
                dsps_fird_init_f32(&s_ft8_fir, (float *)s_ft8_lpf_taps,
                                   s_ft8_fir_delay, 31, 4);
                s_pre_head       = 0;
                s_cap_read_head  = 0;
                s_ft8_in_mode    = true;
            }
            for (int i = 0; i < DSP_FFT_SIZE; i += 4) {
                s_ft8_mix_buf[i + 0] =  (float)samples[2*(i+0)];      // +I
                s_ft8_mix_buf[i + 1] =  (float)samples[2*(i+1) + 1];  // +Q
                s_ft8_mix_buf[i + 2] = -(float)samples[2*(i+2)];      // -I
                s_ft8_mix_buf[i + 3] = -(float)samples[2*(i+3) + 1];  // -Q
            }
            // NB: dsps_fird_f32's `len` is OUTPUT length: 256 out from 1024 in.
            int n_out = dsps_fird_f32(&s_ft8_fir, s_ft8_mix_buf,
                                      s_ft8_dec_buf, DSP_FFT_SIZE / 4);
            if (n_out > 0 && s_ft8_pre) {
                uint32_t pos   = s_pre_head % FT8_PRE_CAP;
                uint32_t first = FT8_PRE_CAP - pos;
                if (first > (uint32_t)n_out) first = (uint32_t)n_out;
                memcpy(&s_ft8_pre[pos], s_ft8_dec_buf, first * sizeof(float));
                if ((uint32_t)n_out > first)
                    memcpy(&s_ft8_pre[0], &s_ft8_dec_buf[first],
                           ((uint32_t)n_out - first) * sizeof(float));
                __sync_synchronize();
                s_pre_head += (uint32_t)n_out;
            }
            s_ft8_iter_count++;
            s_ft8_total_n_out += n_out;
            int64_t now = esp_timer_get_time();
            if (now - s_ft8_last_log_us > 1000000) {
                ESP_LOGI(TAG, "FT8 cap: %u iters %u smp head=%u active=%d idx=%d/%d",
                    (unsigned)s_ft8_iter_count, (unsigned)s_ft8_total_n_out,
                    (unsigned)s_pre_head, (int)s_ft8_active, s_ft8_idx, s_ft8_target);
                s_ft8_iter_count = 0;
                s_ft8_total_n_out = 0;
                s_ft8_last_log_us = now;
            }
            // S-meter: every ~10th window, publish spectrum from the raw IQ
            // samples[] (un-mixed — the fs/4 mixer wrote s_ft8_mix_buf, not
            // samples[]), so it stays IF-bin-aligned for the VFO peak reader.
            if (++s_ft8_smeter_tick >= 10) {
                s_ft8_smeter_tick = 0;
                compute_and_publish_spectrum(samples, tmp_spectrum, &last_min, &last_max, &last_mean);
            }
            continue;
        }
        // Not FT8 mode: mark exit so the next FT8 entry resets the FIR/ring.
        s_ft8_in_mode = false;
        // DC blocker, windowing, FFT, dB conversion, publish to s_spectrum
        // (normal panadapter path).
        compute_and_publish_spectrum(samples, tmp_spectrum, &last_min, &last_max, &last_mean);

        // v0.16.0 zoom-FFT: mix/decimate/accumulate toward a second,
        // higher-resolution FFT centered on the pan target. No-op when
        // zoom <= x2 (s_zoom_decim == 1). Reuses `samples`, which
        // compute_and_publish_spectrum already DC-blocked in place.
        zoom_process(samples);
    }
}