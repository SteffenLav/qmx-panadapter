/* On-device WSPR decoder self-test + timing probe. See wspr_selftest.h for why
 * this exists at all - the short version is that the Phase 3 RX slot loop has a
 * 120 s budget nobody has measured on this silicon, and a UI built on an
 * unmeasured budget shows nothing for reasons that are very hard to find later.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wspr_proto.h"
#include "wspr_fano.h"
#include "wspr_decode.h"
#include "util/psram_task.h"
#include "wspr_selftest.h"

static const char *TAG = "wspr_st";

// One full WSPR receive window. 120 s at 12 kHz.
#define CAP_SAMPLES ((long)(120.0 * WSPR_SAMPLE_RATE_HZ))
#define TONE_SPACING_HZ (WSPR_SAMPLE_RATE_HZ / (double)WSPR_SYM_LEN_SAMPLES)

// What we synthesize, and therefore what we require back. Deliberately NOT the
// operator's own callsign: this must fail loudly if the decoder returns
// something plausible-but-wrong, and a call we would recognise anyway is a
// weaker test than an arbitrary one.
#define ST_CALL  "W5BIT"
#define ST_GRID  "EL09"
#define ST_DBM   17
#define ST_FREQ_HZ 1492.0
#define ST_START_S 1.6   // the real-world start offset, see WSPR_TX_START_OFFSET_MS

static volatile bool s_running = false;

// xorshift + Box-Muller, lifted from test/wspr_synth_harness.c so the on-device
// signal is generated the same way the host tests generate theirs.
/* FLOAT, not double. The P4's FPU is single-precision only, so every double
 * sqrt/log/cos here is software-emulated: the first version of this self-test
 * spent 80 SECONDS synthesizing its 1,440,000 samples. That is outside the
 * decode budget being measured, so it was never wrong - just slow enough to
 * make the probe annoying to re-run, which is its own kind of wrong. */
static uint32_t s_rng = 0xC0FFEE01u;
static float rng_gaussian(void)
{
    uint32_t s = s_rng;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    float u1 = (s % 1000000 + 1) / 1000001.0f;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    float u2 = (s % 1000000 + 1) / 1000001.0f;
    s_rng = s;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

/* Fill `buf` with gaussian noise, then add one continuous-phase 4-FSK WSPR
 * transmission on top of it.
 *
 * Done in ONE int16 buffer rather than the host harness's float accumulator +
 * conversion, because that would need 5.76 MB of float on top of 2.88 MB of
 * int16 and there is no reason to spend it: with a single signal every sample
 * is written once, so noise-first-then-add works and halves the footprint.
 */
static bool synth_into(int16_t *buf, long n, float noise_sigma, float amplitude)
{
    for (long i = 0; i < n; i++) {
        float v = noise_sigma * rng_gaussian() * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        buf[i] = (int16_t)v;
    }

    wspr_msg_bytes_t msg;
    if (!wspr_pack_message(ST_CALL, ST_GRID, ST_DBM, &msg)) return false;
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);

    long start = (long)(ST_START_S * WSPR_SAMPLE_RATE_HZ);
    float phase = 0.0f;
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        float freq = (float)ST_FREQ_HZ + tones[sym] * (float)TONE_SPACING_HZ;
        float dphi = 2.0f * (float)M_PI * freq / (float)WSPR_SAMPLE_RATE_HZ;
        for (int k = 0; k < WSPR_SYM_LEN_SAMPLES; k++) {
            long idx = start + (long)sym * WSPR_SYM_LEN_SAMPLES + k;
            if (idx >= 0 && idx < n) {
                float v = buf[idx] + amplitude * sinf(phase) * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                buf[idx] = (int16_t)v;
            }
            phase += dphi;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
    }
    return true;
}

static void heap_line(const char *when)
{
    // free size only - largest_free_block walks the heap with interrupts off
    // and must stay off anything that could become a periodic path (the
    // cyan-flash rule). This runs on demand, but the habit is the point.
    ESP_LOGI(TAG, "heap %s: internal free=%u KB  psram free=%u KB", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void wspr_selftest_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "=== WSPR on-device self-test: %s %s %d dBm at %.1f Hz ===",
             ST_CALL, ST_GRID, ST_DBM, ST_FREQ_HZ);
    heap_line("at start");

    int16_t *buf = (int16_t *)heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "could not allocate %ld KB for the capture buffer",
                 (long)(CAP_SAMPLES * sizeof(int16_t) / 1024));
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int64_t t = esp_timer_get_time();
    // 0.05 amplitude against 0.02 noise sigma is deliberately the EASIEST point
    // of the host sensitivity sweep in test/wspr_synth_harness.c, with the same
    // station identity. That makes this a control: if the P4 fails here, the
    // silicon is at fault, not the signal.
    bool ok = synth_into(buf, CAP_SAMPLES, 0.02f, 0.05f);
    int64_t synth_ms = (esp_timer_get_time() - t) / 1000;
    if (!ok) {
        ESP_LOGE(TAG, "synthesis refused the message - check ST_CALL/ST_GRID/ST_DBM");
        free(buf);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "synthesized %ld samples (%.0f s) in %lld ms",
             (long)CAP_SAMPLES, CAP_SAMPLES / WSPR_SAMPLE_RATE_HZ, (long long)synth_ms);
    heap_line("after synth");

    // --- stage 1: candidate search (averaged periodogram over the capture) ---
    wspr_freq_candidate_t cands[8];
    t = esp_timer_get_time();
    int ncand = wspr_find_candidates(buf, CAP_SAMPLES, 1400.0, 1600.0, cands, 8);
    int64_t find_ms = (esp_timer_get_time() - t) / 1000;
    ESP_LOGW(TAG, "STAGE 1  find_candidates: %d candidate(s) in %lld ms",
             ncand, (long long)find_ms);

    // --- stage 2: decode each candidate ---
    int64_t decode_ms_total = 0;
    bool found = false;
    for (int i = 0; i < ncand; i++) {
        wspr_decode_result_t r;
        t = esp_timer_get_time();
        wspr_decode_candidate(buf, CAP_SAMPLES, cands[i].freq_hz, &r);
        int64_t ms = (esp_timer_get_time() - t) / 1000;
        decode_ms_total += ms;

        if (r.ok) {
            ESP_LOGW(TAG, "  #%d f=%.2f  DECODED '%s' '%s' %d dBm  dt=%.3fs cycles=%u  [%lld ms]",
                     i, cands[i].freq_hz, r.callsign, r.grid, r.power_dbm,
                     r.best_dt_samples / WSPR_SAMPLE_RATE_HZ, r.cycles, (long long)ms);
            if (strcmp(r.callsign, ST_CALL) == 0 &&
                strncmp(r.grid, ST_GRID, 4) == 0 &&
                r.power_dbm == ST_DBM) {
                found = true;
            }
        } else {
            ESP_LOGI(TAG, "  #%d f=%.2f  rejected (cycles=%u)  [%lld ms]",
                     i, cands[i].freq_hz, r.cycles, (long long)ms);
        }
    }

    int64_t total_ms = find_ms + decode_ms_total;
    ESP_LOGW(TAG, "STAGE 2  decode: %lld ms across %d candidate(s)",
             (long long)decode_ms_total, ncand);
    heap_line("after decode");

    // The verdict that matters for Phase 3. A WSPR cycle is 120 s and the next
    // capture starts immediately, so the decode has to finish inside the slot
    // with room to spare - not merely "eventually".
    ESP_LOGW(TAG, "TOTAL decode path: %lld ms  (%.1f%% of a 120 s cycle)",
             (long long)total_ms, 100.0 * total_ms / 120000.0);
    if (found) {
        ESP_LOGW(TAG, "RESULT: PASS - the P4 recovered the exact message it was given");
    } else {
        ESP_LOGE(TAG, "RESULT: FAIL - '%s' '%s' %d dBm was NOT recovered",
                 ST_CALL, ST_GRID, ST_DBM);
    }

    free(buf);
    ESP_LOGI(TAG, "self-test task stack: %u bytes still free",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    s_running = false;
    vTaskDelete(NULL);
}

bool wspr_selftest_running(void) { return s_running; }

void wspr_selftest_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "self-test already running - ignoring");
        return;
    }
    // Claimed BEFORE the task is created, and cleared if creation fails. Setting
    // it as the task's first statement would mean "has begun running", while
    // every reader needs "exists" - the exact #199 bug, which on this board is a
    // wide window because low-priority tasks can wait a second for a first slice.
    s_running = true;

    // 32 KB, and in PSRAM - both measured, neither a guess.
    //
    // MEASURED: wspr_decode_candidate() reserves a flat 16,384-byte frame in its
    // prologue (read straight out of the disassembly: `add sp,sp,t0` with
    // t0 = 0xffffc000). The compiler inlines try_hard_decision() and
    // try_weighted_decision() into it, so their locals - mettab at 2 KB, three
    // 1296-byte double arrays, tp at 5184 - all land in ONE frame that is
    // reserved unconditionally on entry. A 16 KB task stack therefore overflowed
    // by ~1260 bytes before the function had done anything, which is the same
    // reserved-at-the-prologue shape as the v0.20.1 pounce crash.
    //
    // PSRAM: xTaskCreate() takes its stack from INTERNAL RAM, of which this
    // board has ~40 KB free - so a permanent 32 KB decode task there is not
    // affordable, and the eventual RX slot loop will hit exactly this wall.
    // Decoding is background work on a 2-minute cadence, which is precisely
    // what psram_task_create() is for. Measuring WITH the PSRAM stack is
    // therefore measuring the real thing, not a laboratory best case.
    if (!psram_task_create(wspr_selftest_task, "wspr_st", 32768, NULL,
                           tskIDLE_PRIORITY + 1, tskNO_AFFINITY)) {
        ESP_LOGE(TAG, "could not create the self-test task");
        s_running = false;
    }
}
