/* On-device WSPR decoder self-test + timing probe. See wspr_selftest.h for why
 * this exists at all - the short version is that the Phase 3 RX slot loop has a
 * 120 s budget nobody has measured on this silicon, and a UI built on an
 * unmeasured budget shows nothing for reasons that are very hard to find later.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wspr_proto.h"
#include "wspr_fano.h"
#include "wspr_decode.h"
#include "util/psram_task.h"
#include "util/maidenhead.h"
#include "util/dxcc.h"
#include "storage/settings.h"
#include "wspr_sim.h"
#include "wspr_spots.h"
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
/* Synthesis lives in wspr_sim.c and is shared with simulation mode. Three
 * copies of a 4-FSK synthesizer is how they drift apart and stop testing the
 * same thing, so there is one. */

static void heap_line(const char *when)
{
    // free size only - largest_free_block walks the heap with interrupts off
    // and must stay off anything that could become a periodic path (the
    // cyan-flash rule). This runs on demand, but the habit is the point.
    ESP_LOGI(TAG, "heap %s: internal free=%u KB  psram free=%u KB", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

/* THREE stations, the same set the host harness's multi-signal test uses.
 *
 * One station proves the decoder runs. Three prove it SEPARATES - that the
 * candidate search finds each of them and that a strong neighbour does not
 * swamp a weak one, which is the case a real band presents constantly and a
 * single synthetic signal cannot exercise at all.
 *
 * They also give the Phase 3 UI something real to render. These are genuine
 * decodes of genuine (synthesized) signals, not injected rows - but until the
 * RX slot loop exists they are the ONLY way spots reach the store, which is why
 * /api/wspr reports rx_live:false.
 */
static const struct { const char *call, *grid; int dbm; double f0; float amp; }
s_stations[] = {
    { "K1ABC",  "FN20", 37, 1420.0, 0.30f },
    { "OZ1LAV", "JO45", 23, 1500.0, 0.15f },
    { "VE3XYZ", "EN00", 30, 1580.0, 0.20f },
};
#define ST_N_STATIONS (int)(sizeof(s_stations) / sizeof(s_stations[0]))

/* Turn a decode into a spot: distance and bearing from the operator's own grid,
 * country from the callsign - the same helpers the FT8 list and /api/decodes
 * use, so the two screens cannot disagree. */
static void record_spot(const wspr_decode_result_t *r, int64_t cycle_utc)
{
    wspr_spot_t sp;
    memset(&sp, 0, sizeof(sp));
    /* snprintf throughout: these sources are exactly sizeof(dst)-1 characters,
     * so strncpy copies without a terminator and -Werror=stringop-truncation is
     * right to complain even though the memset above happens to save it. */
    snprintf(sp.call, sizeof(sp.call), "%s", r->callsign);
    snprintf(sp.grid, sizeof(sp.grid), "%s", r->grid);
    sp.cycle_utc = cycle_utc;
    sp.freq_hz   = (float)r->freq_hz;
    /* NOT measured here. sync_score is a correlation figure, not an SNR, and
     * casting it into an SNR field produced -5839 / -24994 in the first run -
     * a fabricated measurement, which is the one thing this project refuses to
     * display (see the ADIF "599" note). The slot loop will measure it. */
    sp.snr_db    = WSPR_SNR_UNKNOWN;
    sp.drift_hz  = WSPR_DRIFT_UNKNOWN;
    sp.power_dbm = (int16_t)r->power_dbm;
    sp.km = -1; sp.bearing_deg = -1;

    const char *cty = dxcc_lookup_alpha3(r->callsign);
    /* snprintf, not strncpy: the alpha-3 is exactly sizeof(cty)-1 characters, so
     * strncpy copies without a terminator and -Werror=stringop-truncation is
     * right to complain even though the memset above happens to save it. */
    if (cty) snprintf(sp.cty, sizeof(sp.cty), "%s", cty);

    qmx_settings_t qs;
    settings_load_all(&qs);
    double mlat, mlon, tlat, tlon;
    if (qs.my_grid[0] && maidenhead_to_latlon(qs.my_grid, &mlat, &mlon) &&
        maidenhead_to_latlon(sp.grid, &tlat, &tlon)) {
        sp.km          = (int32_t)haversine_km(mlat, mlon, tlat, tlon);
        sp.bearing_deg = (int16_t)bearing_deg(mlat, mlon, tlat, tlon);
    }
    wspr_spots_add(&sp);
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
    /* Light noise, then three stations at different frequencies and amplitudes -
     * the host harness's own multi-signal case, so a device result can be
     * compared against a host result directly. */
    wspr_sim_noise(buf, CAP_SAMPLES, 0.02f);
    bool ok = true;
    for (int i = 0; i < ST_N_STATIONS; i++) {
        if (!wspr_sim_add_station(buf, CAP_SAMPLES, s_stations[i].call, s_stations[i].grid,
                                  s_stations[i].dbm, s_stations[i].f0, ST_START_S,
                                  s_stations[i].amp)) {
            ESP_LOGE(TAG, "synthesis refused '%s' - check the callsign/grid/power",
                     s_stations[i].call);
            ok = false;
        }
    }
    int64_t synth_ms = (esp_timer_get_time() - t) / 1000;
    if (!ok) {
        free(buf);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "synthesized %ld samples (%.0f s), %d stations, in %lld ms",
             (long)CAP_SAMPLES, CAP_SAMPLES / WSPR_SAMPLE_RATE_HZ,
             ST_N_STATIONS, (long long)synth_ms);
    heap_line("after synth");

    // --- stage 1: candidate search (averaged periodogram over the capture) ---
    wspr_freq_candidate_t cands[8];
    t = esp_timer_get_time();
    int ncand = wspr_find_candidates(buf, CAP_SAMPLES, 1350.0, 1650.0, cands, 8);
    int64_t find_ms = (esp_timer_get_time() - t) / 1000;
    ESP_LOGW(TAG, "STAGE 1  find_candidates: %d candidate(s) in %lld ms",
             ncand, (long long)find_ms);

    // --- stage 2: decode each candidate ---
    int64_t decode_ms_total = 0;
    bool found[ST_N_STATIONS];
    memset(found, 0, sizeof(found));
    int64_t cycle_utc = (int64_t)time(NULL);
    cycle_utc -= cycle_utc % 120;     /* the even minute this "capture" belongs to */

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
            record_spot(&r, cycle_utc);
            for (int k = 0; k < ST_N_STATIONS; k++) {
                if (strcmp(r.callsign, s_stations[k].call) == 0 &&
                    strncmp(r.grid, s_stations[k].grid, 4) == 0 &&
                    r.power_dbm == s_stations[k].dbm) {
                    found[k] = true;
                }
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

    int nfound = 0;
    for (int k = 0; k < ST_N_STATIONS; k++) {
        ESP_LOGW(TAG, "  %-8s %s", s_stations[k].call, found[k] ? "recovered" : "MISSING");
        if (found[k]) nfound++;
    }
    if (nfound == ST_N_STATIONS) {
        ESP_LOGW(TAG, "RESULT: PASS - all %d stations recovered, %d spot(s) in the store",
                 ST_N_STATIONS, wspr_spots_count());
    } else {
        ESP_LOGE(TAG, "RESULT: FAIL - %d of %d stations recovered",
                 nfound, ST_N_STATIONS);
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
