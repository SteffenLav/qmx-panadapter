/* WSPR receive slot loop. See wspr_rx.h for what it does and what it does not
 * do (it decodes every other cycle - read that note before "fixing" it). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "dsp.h"
#include "ui_mode.h"
#include "util/psram_task.h"
#include "util/maidenhead.h"
#include "util/dxcc.h"
#include "storage/settings.h"
#include "fft/kiss_fftr.h"
#include "wspr_decode.h"
#include "wspr_sim.h"
#include "wspr_tx.h"
#include "esp_random.h"
#include "wspr_spots.h"
#include "wspr_rx.h"
#include "wspr_wav.h"
#include "storage/sd_archive.h"
#include "net/webserver_ws.h"

static const char *TAG = "wspr_rx";

#define WSPR_CYCLE_MS     120000
#define CAP_SAMPLES       ((uint32_t)(120 * (uint32_t)WSPR_SAMPLE_RATE_HZ))  /* 1,440,000 */
/* ⛔ THIS WAS 8, AND IT WAS THE REAL SENSITIVITY LIMIT.
 *
 * Every one of the first 127 cycles ever run reported exactly 8 candidates -
 * the cap was saturated 100 % of the time, so real signals were discarded on
 * every cycle and the log line "8 candidate(s)" read like a measurement while
 * actually being a ceiling.
 *
 * Proved with the capture dump (test/wav_reference/wspr/README-260824.md):
 * wsprd found 32 stations across three windows where we found 10, and at 19:10
 * we decoded -11/-13/-13/-18 dB while MISSING -13 and -15 dB. A weak-signal
 * floor cannot do that; it would take everything above it. The comb score that
 * ranks candidates is correlation ENERGY, not SNR, so a strong station can sit
 * below eight noisier peaks and never be tried at all.
 *
 * Raising it is only safe because the decode now runs on its own task while the
 * next capture fills (the ping-pong below), which buys a whole cycle instead of
 * the leftovers of one. WSPR_DECODE_BUDGET_MS is the real limiter; this number
 * just has to be high enough not to be the limiter itself. */
#define WSPR_MAX_CANDS    20

/* Decode is ~7.9 s per candidate, and with the ping-pong it has a full 120 s
 * cycle. Stop at 105 s so the buffer is handed back before the next capture
 * needs it - the alternative is dropping a whole cycle, which costs far more
 * than the last candidate or two would have found.
 * ⭐ The count of candidates NOT tried is LOGGED, because a silent budget cut
 * is exactly the kind of invisible ceiling this file just spent a day
 * discovering. */
#define WSPR_DECODE_BUDGET_MS  115000

/* How late an arm may be and still be anchored by the pre-ring. The conversion
 * is ~2 s and an armed SD dump adds ~4.2 s, so 10 s is generous cover while
 * leaving a third of FT8_PRE_CAP's 15 s in reserve. Being late is free here;
 * SLEEPING through a boundary is what costs a whole cycle. */
#define WSPR_ARM_GRACE_MS      10000

/* The audio window sits between 1400 and 1600 Hz for a standard WSPR dial, but
 * the search is widened a little either side: the operator's dial calibration,
 * the QMX's own, and a transmitter's offset all move real signals about, and a
 * candidate found slightly outside the nominal window still decodes. */
#define SEARCH_LO_HZ      1350.0
#define SEARCH_HI_HZ      1650.0

/* ---- per-cycle waterfall ----
 * Built from the captured window (see wspr_rx.h for why a LIVE spectrum is not
 * possible on this page). Allocated on first use and reused: ~36 KB, and the
 * kiss_fftr config for 8192 points is a few hundred KB more, so neither wants
 * to be built 176 times a cycle - let alone once per row. */
static wspr_guards_t s_guards;      /* see wspr_decode.h; set at slot-loop start */

static uint8_t  *s_wf;                 /* RING: WSPR_WF_HIST_ROWS * WSPR_WF_COLS */
static int       s_wf_head;            /* ring index the NEXT row is written to */
static int       s_wf_cycle_base;      /* ring index of this cycle's row 0 */
static uint32_t  s_wf_seq;
static SemaphoreHandle_t s_wf_mtx;

static volatile bool s_run;
static TaskHandle_t  s_task;
/* ONE BUFFER PER WRITER. Capture and decode are separate tasks now and both
 * report progress; sharing a single static would tear the string. Composed on
 * read instead, which also makes the parallelism visible - "cap 45/120 | dec
 * 3/20" says at a glance that the receiver is no longer deaf while decoding. */
static char          s_cap_status[40] = "idle";
static char          s_dec_status[40] = "";

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_cap_status, sizeof(s_cap_status), fmt, ap);
    va_end(ap);
}

static void set_dec_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_dec_status, sizeof(s_dec_status), fmt, ap);
    va_end(ap);
}

const char *wspr_rx_status(void)
{
    static char out[88];
    if (s_dec_status[0]) snprintf(out, sizeof(out), "%s | %s", s_cap_status, s_dec_status);
    else                 snprintf(out, sizeof(out), "%s", s_cap_status);
    return out;
}
bool wspr_rx_running(void)       { return s_run; }

/* Same conversion the spot store's other producer does. Distance, bearing and
 * country come from the device's own helpers so this list and the FT8 list
 * cannot disagree. */
static void file_spot(const wspr_decode_result_t *r, int64_t cycle_utc, int snr_db)
{
    wspr_spot_t sp;
    memset(&sp, 0, sizeof(sp));
    snprintf(sp.call, sizeof(sp.call), "%s", r->callsign);
    snprintf(sp.grid, sizeof(sp.grid), "%s", r->grid);
    sp.cycle_utc = cycle_utc;
    sp.freq_hz   = (float)r->freq_hz;
    sp.power_dbm = (int16_t)r->power_dbm;
    sp.snr_db    = (int16_t)snr_db;          /* WSPR_SNR_UNKNOWN until measured */
    sp.drift_hz  = WSPR_DRIFT_UNKNOWN;       /* likewise - 0 Hz is a REAL value */
    sp.km = -1; sp.bearing_deg = -1;

    const char *cty = dxcc_lookup_alpha3(r->callsign);
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

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* One row per symbol period, one column per 1.4648 Hz bin.
 *
 * The bin size is not a choice: an 8192-point FFT at 12 kHz gives exactly
 * WSPR's tone spacing, so a transmission occupies four adjacent columns and
 * reads as a clean vertical trace rather than a smear.
 *
 * ---- WHY THIS IS ROW-AT-A-TIME ----
 *
 * It used to build the whole image after the capture closed, which meant the
 * pane showed nothing for two minutes and then everything at once, plus an
 * ~8 s pause while 352 FFTs ran. Rows are independent, so they can instead be
 * produced AS THE CAPTURE FILLS: row r needs samples [r*8192, r*8192+12288)
 * and is finished the moment the capture passes that mark. One row every
 * 0.68 s at ~45 ms of work, about 6.6% of a core - the same total work, spread
 * out - and the picture scrolls while the band is being heard.
 *
 * Row r advances 8192 samples but CONSUMES 12288: two 8192-point FFTs
 * overlapped by half. A single bin's noise power is exponentially distributed,
 * so its standard deviation equals its mean - ~5.6 dB of speckle, which swamps
 * a weak WSPR trace sitting only ~7 dB above the floor. Averaging two halves
 * the variance, and at 1.4 s per row the lost time resolution is free against
 * a 110 s transmission.
 *
 * ---- THE FLOOR, AND THE TRAP IT AVOIDS ----
 *
 * Scaling is relative to the capture's own median: WSPR windows have no
 * absolute reference, since RF gain, band and time of day all move the floor.
 * Live, that median does not exist yet - so a row is painted against the
 * PREVIOUS cycle's floor, and the definitive median is computed once at
 * finalise, which repaints every row from magnitudes already stored.
 *
 * The floor is therefore updated ONCE PER CYCLE and never per row. Deriving it
 * per row is the documented trap that left the panadapter's per-bin adaptive
 * floor never actually running: ui_flat_mode_reset() re-seeds it ~17 times a
 * second, so the tracker is seeded, updated once and thrown away. A floor that
 * re-derives itself faster than it converges is not adaptive, it is noise. */

static float *s_wf_mag;          /* ROWS*COLS magnitudes, PSRAM, alive per cycle */
static float  s_wf_floor;        /* median carried from the previous cycle */
static int    s_wf_rows_done;
static kiss_fftr_cfg     s_wf_cfg;
static kiss_fft_scalar  *s_wf_in;
static kiss_fft_cpx     *s_wf_sp;

#define WF_NFFT   8192
#define WF_STEP   (WF_NFFT)                 /* samples a row advances */
#define WF_SPAN   (WF_NFFT + WF_NFFT / 2)   /* samples a row consumes: 12288 */
#define WF_LO_DB  4.0f
#define WF_HI_DB  16.0f

/* Samples that must have landed before row r can be computed. */
static long wf_samples_for_row(int r) { return (long)r * WF_STEP + WF_SPAN; }

static uint8_t wf_byte(float magv, float floorv)
{
    float db = 10.0f * log10f((magv + 1e-20f) / floorv);
    float t = (db - WF_LO_DB) / (WF_HI_DB - WF_LO_DB);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    /* 254, not 255: 255 is reserved for the cycle-boundary marker so the view
     * can colour it distinctly. See WSPR_WF_MARK. */
    return (uint8_t)(t * 254.0f);
}

/* Ring index this cycle's row r occupies. Rows arrive in order, so a cycle
 * lays down a contiguous run starting at s_wf_cycle_base. */
static inline int wf_ring_idx(int row)
{
    return (s_wf_cycle_base + row) % WSPR_WF_HIST_ROWS;
}

/* Publish one already-computed row of bytes at ring index `idx`, advancing the
 * head past it. Caller holds no lock; this takes it. */
static void wf_publish(int idx, const uint8_t *bytes)
{
    if (s_wf_mtx) xSemaphoreTake(s_wf_mtx, portMAX_DELAY);
    memcpy(&s_wf[(size_t)idx * WSPR_WF_COLS], bytes, WSPR_WF_COLS);
    /* Callers only ever publish at or after the head, in increasing order, so
     * this cannot rewind. wf_finalise() sets the head itself and does not come
     * through here. */
    s_wf_head = (idx + 1) % WSPR_WF_HIST_ROWS;
    s_wf_seq++;
    if (s_wf_mtx) xSemaphoreGive(s_wf_mtx);
}

/* One dashed row marking a cycle boundary - and the ~68 s of deafness that
 * follows it while the decoder runs. See WSPR_WF_MARK. */
static void wf_mark_boundary(void)
{
    if (!s_wf) return;
    uint8_t row[WSPR_WF_COLS];
    for (int c = 0; c < WSPR_WF_COLS; c++)
        row[c] = ((c / 4) & 1) ? 0 : WSPR_WF_MARK;
    /* ⛔ TWO rows, not one. The view downsamples HIST_ROWS into a 200 px pane
     * nearest-neighbour - a step of 1.76 - so a single row is SKIPPED whenever
     * the map jumps by 2, i.e. the marker would silently vanish on roughly two
     * cycles in five. Consecutive floor(y*1.76) values step by 1 or 2, so
     * missing two adjacent source rows would need a step of 3 and cannot
     * happen. Anything one row thick in this buffer has the same problem. */
    for (int k = 0; k < WSPR_WF_MARK_ROWS; k++) wf_publish(s_wf_head, row);
}

static bool wf_begin(void)
{
    if (!s_wf) {
        s_wf = heap_caps_malloc(WSPR_WF_HIST_ROWS * WSPR_WF_COLS,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wf) { ESP_LOGW(TAG, "no PSRAM for the waterfall"); return false; }
        memset(s_wf, 0, WSPR_WF_HIST_ROWS * WSPR_WF_COLS);   /* once, at birth */
    }
    if (!s_wf_mag) {
        s_wf_mag = heap_caps_malloc((size_t)WSPR_WF_ROWS * WSPR_WF_COLS * sizeof(float),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wf_mag) { ESP_LOGW(TAG, "no PSRAM for waterfall magnitudes"); return false; }
    }
    if (!s_wf_cfg) s_wf_cfg = kiss_fftr_alloc(WF_NFFT, 0, NULL, NULL);
    if (!s_wf_in)  s_wf_in  = malloc((size_t)WF_NFFT * sizeof(kiss_fft_scalar));
    if (!s_wf_sp)  s_wf_sp  = malloc((size_t)(WF_NFFT / 2 + 1) * sizeof(kiss_fft_cpx));
    if (!s_wf_cfg || !s_wf_in || !s_wf_sp) {
        ESP_LOGW(TAG, "waterfall: out of memory");
        return false;
    }
    /* ⛔ NOTHING IS BLANKED. The old version cleared the buffer here, which is
     * what produced the black-screen-then-drip the operator reported. The
     * comment that used to justify it ("a short cycle must not leave last
     * cycle's rows standing below this one's") was reasoning about a
     * fixed-window picture; in a carpet the older rows SHOULD stand, that is
     * the entire point, and a short cycle simply contributes fewer rows. */
    s_wf_rows_done  = 0;
    s_wf_cycle_base = s_wf_head;
    return true;
}

/* Compute one row. Exactly one of fsrc/isrc is non-NULL: the live capture
 * hands back floats, the simulator synthesizes int16. Both are only ever read
 * relative to this capture's own median, so their differing absolute scales
 * cancel and no normalisation is needed here. */
static void wf_row(const float *fsrc, const int16_t *isrc, long navail, int row)
{
    if (!s_wf_mag || row < 0 || row >= WSPR_WF_ROWS) return;
    const double bin_hz = WSPR_SAMPLE_RATE_HZ / WF_NFFT;
    const int lo_bin = (int)(WSPR_WF_LO_HZ / bin_hz + 0.5);
    float *magrow = &s_wf_mag[(size_t)row * WSPR_WF_COLS];

    for (int c = 0; c < WSPR_WF_COLS; c++) magrow[c] = 0;
    for (int k = 0; k < 2; k++) {
        long off = ((long)row * 2 + k) * (WF_NFFT / 2);
        for (int i = 0; i < WF_NFFT; i++) {
            long s = off + i;
            if (s >= navail)  s_wf_in[i] = 0;
            else if (fsrc)    s_wf_in[i] = (kiss_fft_scalar)fsrc[s];
            else              s_wf_in[i] = (kiss_fft_scalar)(isrc[s] / 32768.0);
        }
        kiss_fftr(s_wf_cfg, s_wf_in, s_wf_sp);
        for (int c = 0; c < WSPR_WF_COLS; c++) {
            int b = lo_bin + c;
            float p = (b <= WF_NFFT / 2)
                    ? s_wf_sp[b].r * s_wf_sp[b].r + s_wf_sp[b].i * s_wf_sp[b].i : 0;
            magrow[c] += p * 0.5f;
        }
    }

    /* ⛔ FIRST CYCLE AFTER BOOT: seed a provisional floor from THIS ROW.
     *
     * The floor is normally carried from the previous cycle's median, so on the
     * very first cycle after boot there is none and this branch never ran - the
     * pane stayed black for a full 120 s and then the whole window appeared at
     * once. Reported from the bench as "it took like 2 min before anything
     * happened", which is the wrong first impression for a live display.
     *
     * One row's 205 bins is a fair floor estimate because WSPR occupies only a
     * few of them, and it is only ever used to make the row visible NOW -
     * wf_finalise() repaints this whole cycle against the real median at the
     * end regardless, so a poor seed self-corrects within the cycle and is
     * never what the operator ends up judging. */
    if (s_wf_floor <= 0.0f) {
        static float med[WSPR_WF_COLS];
        memcpy(med, magrow, sizeof(med));
        for (int a = 1; a < WSPR_WF_COLS; a++) {
            float v = med[a]; int b = a - 1;
            while (b >= 0 && med[b] > v) { med[b + 1] = med[b]; b--; }
            med[b + 1] = v;
        }
        float m = med[WSPR_WF_COLS / 2];
        if (m > 0.0f) {
            s_wf_floor = m;
            ESP_LOGI(TAG, "waterfall: seeded provisional floor from row %d", row);
        }
    }

    /* Paint now against the carried floor so the row is visible immediately;
     * wf_finalise() repaints it against this cycle's own median. */
    if (s_wf_floor > 0.0f) {
        uint8_t bytes[WSPR_WF_COLS];
        for (int c = 0; c < WSPR_WF_COLS; c++)
            bytes[c] = wf_byte(magrow[c], s_wf_floor);
        wf_publish(wf_ring_idx(row), bytes);
    }
    if (row >= s_wf_rows_done) s_wf_rows_done = row + 1;
}

/* Definitive pass: this cycle's own median, every row repainted from the
 * magnitudes already computed. No FFTs here - they were done row by row. */
static void wf_finalise(void)
{
    if (!s_wf || !s_wf_mag) return;

    static float samp[2048];
    int ns = 0;
    for (int i = 0; i < WSPR_WF_ROWS * WSPR_WF_COLS && ns < 2048; i += 17)
        samp[ns++] = s_wf_mag[i];
    for (int a = 1; a < ns; a++) {
        float v = samp[a]; int b = a - 1;
        while (b >= 0 && samp[b] > v) { samp[b + 1] = samp[b]; b--; }
        samp[b + 1] = v;
    }
    float med = ns ? samp[ns / 2] : 1e-12f;
    float top = ns ? samp[(int)(ns * 0.995f)] : med * 100.0f;
    if (med <= 0) med = 1e-12f;
    if (top <= med) top = med * 100.0f;

    /* BLACK IS WELL ABOVE THE FLOOR, and the TOP IS FIXED.
     *
     * The top used to be this capture's 99.5th percentile clamped to 20-40 dB,
     * so THE LOUDEST THING IN THE WINDOW set the scale - and on this band that
     * is broadband QRM, not a WSPR signal. A screenshot proved it: with a burst
     * present hi_db pinned at 40 and a weak 7 dB trace rendered at 6%
     * brightness while the interference took the whole top of the ramp.
     *
     * Fixed span instead: black at +4 dB, saturated at +16 dB over the median.
     * A WSPR signal in a 1.4648 Hz bin is ~7 dB above the floor when weak and
     * ~20 dB when strong, so those map to 25% and fully-on. Interference clips,
     * which is the right trade for a display whose job is showing WSPR. The
     * FLOOR stays adaptive; only the SPAN is fixed. */
    ESP_LOGI(TAG, "waterfall: span %.0f-%.0f dB over median; loudest bin +%.1f dB",
             WF_LO_DB, WF_HI_DB, 10.0f * log10f(top / med));

    /* Only THIS cycle's rows are repainted, at the ring positions they already
     * occupy. Older cycles keep the scale they were drawn with: rescaling them
     * to a median measured minutes later would silently rewrite history, and a
     * carpet whose past changes brightness cannot be read for trends. */
    if (s_wf_mtx) xSemaphoreTake(s_wf_mtx, portMAX_DELAY);
    for (int r = 0; r < s_wf_rows_done; r++) {
        uint8_t *dst = &s_wf[(size_t)wf_ring_idx(r) * WSPR_WF_COLS];
        const float *src = &s_wf_mag[(size_t)r * WSPR_WF_COLS];
        for (int c = 0; c < WSPR_WF_COLS; c++) dst[c] = wf_byte(src[c], med);
    }
    /* ⛔ The head is advanced HERE as well as in wf_publish(), because on the
     * very first cycle after boot s_wf_floor is still 0, so wf_row() computes
     * magnitudes but publishes nothing - and a head left at the cycle base
     * would make the next cycle overwrite this one in place, i.e. no carpet at
     * all until the second cycle. Idempotent when rows did publish. */
    s_wf_head = wf_ring_idx(s_wf_rows_done);
    s_wf_seq++;
    if (s_wf_mtx) xSemaphoreGive(s_wf_mtx);

    s_wf_floor = med;   /* the next cycle paints its live rows against this */
}

/* Whole-window build, used by the simulator, which has the entire window at
 * once. Identical output to the incremental path - same row function, same
 * finalise - so the two cannot drift apart. */
static void build_waterfall(const int16_t *pcm, long n)
{
    if (!wf_begin()) return;
    for (int r = 0; r < WSPR_WF_ROWS; r++) wf_row(NULL, pcm, n, r);
    wf_finalise();
    wf_mark_boundary();
}

bool wspr_rx_get_waterfall(uint8_t *out)
{
    if (!s_wf || !out || s_wf_seq == 0) return false;
    /* Un-ring into DISPLAY order: out row 0 is the newest row, and each later
     * row is one symbol period older. Doing the reversal here rather than in
     * the view keeps every consumer - Tab5 page, and anything added later -
     * agreeing on which way time runs, which is the bug this replaced. */
    if (s_wf_mtx) xSemaphoreTake(s_wf_mtx, portMAX_DELAY);
    int idx = s_wf_head;
    for (int r = 0; r < WSPR_WF_HIST_ROWS; r++) {
        idx = (idx - 1 + WSPR_WF_HIST_ROWS) % WSPR_WF_HIST_ROWS;
        memcpy(&out[(size_t)r * WSPR_WF_COLS],
               &s_wf[(size_t)idx * WSPR_WF_COLS], WSPR_WF_COLS);
    }
    if (s_wf_mtx) xSemaphoreGive(s_wf_mtx);
    return true;
}

uint32_t wspr_rx_waterfall_seq(void) { return s_wf_seq; }

/* ---- PING-PONG: capture and decode at the same time --------------------
 *
 * The receiver used to be DEAF for a whole cycle out of every two: a capture
 * fills 120 s, then the same task decoded for ~63 s, and by then the next
 * boundary had passed. Measured directly - captures armed at 13:52:00 and
 * 13:56:00 with 13:54:00 skipped, i.e. 121 + 68 = 189 s of a 120 s cycle.
 *
 * Two int16 windows, so one can be decoded while the other is being filled.
 * Only the int16 side is doubled: the float capture buffer is fully consumed by
 * the conversion before the next capture opens, so a second one would be 5.6 MB
 * of PSRAM bought for nothing.
 *
 * ⛔ A BUSY BUFFER IS NEVER OVERWRITTEN. If the decoder still holds both, the
 * cycle is DROPPED and said so out loud - the ft8_test.c precedent, and the
 * only safe answer: blocking would push the next capture off its UTC boundary,
 * which breaks WSPR timing outright, and overwriting corrupts a decode in
 * flight. With a 105 s budget against a 120 s cycle this should never fire; it
 * exists so that if it does, it is visible rather than silently wrong. */
#define WSPR_PCM_SLOTS 2

static int16_t      *s_pcm[WSPR_PCM_SLOTS];
static volatile bool s_pcm_busy[WSPR_PCM_SLOTS];
static QueueHandle_t s_dec_q;
static TaskHandle_t  s_dec_task;
static volatile bool s_dec_exited;

typedef struct {
    int     slot;
    int64_t cycle_utc;
} wspr_dec_job_t;

/* Claimed by the CAPTURE task only, released by the DECODE task only - one
 * writer each way, so a plain volatile flag is sufficient and no lock is
 * needed. Returns -1 when the decoder holds everything. */
static int claim_pcm_slot(void)
{
    for (int i = 0; i < WSPR_PCM_SLOTS; i++) {
        if (!s_pcm_busy[i]) { s_pcm_busy[i] = true; return i; }
    }
    return -1;
}

/* ---- capture dump ------------------------------------------------------ */

/* Armed count lives in NVS, not in a volatile, because the dump can only
 * SUCCEED on a boot with WiFi off (see settings.h wspr_dump_cycles) and so has
 * to survive the reboot that makes it possible. */
int wspr_rx_request_dump(int cycles)
{
    if (cycles < 0) return 0;
    if (cycles > WSPR_DUMP_MAX_CYCLES) cycles = WSPR_DUMP_MAX_CYCLES;
    settings_set_wspr_dump_cycles((uint8_t)cycles);
    ESP_LOGW(TAG, "dump: armed for %d cycle(s), ~%d MB%s", cycles,
             (int)((size_t)cycles * CAP_SAMPLES * 2 / (1024 * 1024)),
             sd_archive_is_mounted() ? ""
                 : " - NO CARD MOUNTED: reboot with WiFi off, or nothing is written");
    return cycles;
}

int wspr_rx_dump_pending(void)
{
    qmx_settings_t st;
    settings_load_all(&st);
    return st.wspr_dump_cycles;
}

/* Write one captured window as a 12 kHz mono 16-bit WAV under
 * /sdcard/qmx-panadapter/wspr/.
 *
 * ⛔ SD WRITES AND WIFI ARE THE WEDGE-PRONE COMBINATION on this board - the SD
 * path and the C6's SDIO link contend, and CLAUDE.md records three separate
 * field failures from it. So this takes sd_archive_lock() and quiets both the
 * WS stream and the DSP transfers for its duration, exactly as the reader's
 * offline save, the log download and the QRZ upload already do. 2.88 MB is a
 * far bigger burst than any of those, which makes it more important here, not
 * less.
 *
 * ⛔ fsync is MANDATORY, not tidiness: fflush leaves the bytes in FatFs
 * buffers, so a card pulled - or a power cut, which happened once already
 * today - reads the file back EMPTY. That bit the SD diag log once and the
 * rule is in CLAUDE.md.
 *
 * Called from the slot loop AFTER the int16 conversion and BEFORE the decode,
 * so the file holds byte-for-byte what the decoder is about to be given. A
 * dump taken from anywhere else would be answering a different question. */
static void dump_window(const int16_t *pcm, uint32_t nsamples, int64_t cycle_utc)
{
    char name[32];
    if (wspr_wav_filename(name, sizeof(name), cycle_utc) == 0) return;

    char path[96];
    snprintf(path, sizeof(path), "/sdcard/qmx-panadapter/wspr/%s", name);

    if (!sd_archive_lock(5000)) {
        ESP_LOGW(TAG, "dump: SD busy - skipping %s", name);
        return;
    }
    webserver_ws_set_paused(true);
    dsp_set_transfer_quiet(true);

    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    mkdir("/sdcard/qmx-panadapter/wspr", 0777);   /* EEXIST is fine */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "dump: cannot open %s (errno %d)", path, errno);
    } else {
        uint8_t hdr[WSPR_WAV_HDR_BYTES];
        size_t hn = wspr_wav_header(hdr, nsamples, (uint32_t)WSPR_SAMPLE_RATE_HZ);
        ok = (hn == sizeof(hdr)) && fwrite(hdr, 1, hn, f) == hn;
        /* Written in chunks so one 2.88 MB fwrite cannot sit on the card for
         * seconds at a time with the lock held. */
        const uint32_t CH = 32768;
        for (uint32_t off = 0; ok && off < nsamples; off += CH) {
            uint32_t cnt = (nsamples - off) < CH ? (nsamples - off) : CH;
            ok = fwrite(pcm + off, sizeof(int16_t), cnt, f) == cnt;
        }
        if (ok) { fflush(f); ok = (fsync(fileno(f)) == 0); }
        fclose(f);
    }

    dsp_set_transfer_quiet(false);
    webserver_ws_set_paused(false);
    sd_archive_unlock();

    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (ok) ESP_LOGW(TAG, "dump: wrote %s (%u KB, %d ms), %d cycle(s) left",
                     name, (unsigned)(nsamples * 2 / 1024), ms, wspr_rx_dump_pending() - 1);
    else    ESP_LOGE(TAG, "dump: FAILED writing %s after %d ms", name, ms);
}

/* Decode one finished window. Runs on the DECODE task; the capture task is
 * filling the other buffer while this works. Reads `pcm` and nothing the
 * capture task writes, which is what makes the concurrency safe - and the
 * decoder itself is re-entrant, every FFT config and scratch buffer being
 * malloc'd and freed per call rather than shared. (A shared FFT scratch is
 * exactly what bit the FT8 monitor pool; see CLAUDE.md.) */
static void decode_one_window(const int16_t *pcm, int64_t cycle_utc)
{
    /* ---- decode ---- */
    int64_t t0 = esp_timer_get_time();
    wspr_freq_candidate_t cands[WSPR_MAX_CANDS];
    int ncand = wspr_find_candidates(pcm, CAP_SAMPLES, SEARCH_LO_HZ, SEARCH_HI_HZ,
                                      cands, WSPR_MAX_CANDS);
    int decoded = 0;
    int guarded = 0;
    wspr_accepted_t accepted = { 0 };
    int skipped = 0;
    for (int i = 0; i < ncand && s_run; i++) {
        /* ⛔ The budget is checked BEFORE starting a candidate, never mid-way:
         * wspr_decode_candidate() is not interruptible, so a check inside it
         * would either do nothing or leave a half-decoded result. */
        int64_t used_ms = (esp_timer_get_time() - t0) / 1000;
        if (used_ms > WSPR_DECODE_BUDGET_MS) { skipped = ncand - i; break; }
        set_dec_status("dec %d/%d", i + 1, ncand);
        wspr_decode_result_t r;
        wspr_decode_candidate(pcm, CAP_SAMPLES, cands[i].freq_hz, &r);
        /* Log EVERY candidate, not just the ones that decode. A silent
         * "0 decodes" cannot distinguish "nothing was on the air" from
         * "the search looked in the wrong place" from "the audio was
         * wrong" - and those need completely different fixes. */
        ESP_LOGI(TAG, "  cand %d: f=%.2f Hz score=%.3g cycles=%u %s",
                 i, cands[i].freq_hz, (double)cands[i].comb_score,
                 r.cycles, r.ok ? "DECODED" : "rejected");
        if (!r.ok) continue;

        /* Both guards are MEASURED here whatever is enforced, so an
         * ordinary session accumulates the evidence to choose between
         * them on real signals. See wspr_decode.h. */
        double dnear = -1.0;
        int would_near = 0, would_slow = 0;
        wspr_guard_verdict_t v = wspr_guard_check(&s_guards, &accepted, &r,
                                                  &dnear, &would_near, &would_slow);
        if (v != WSPR_GUARD_PASS) {
            guarded++;
            ESP_LOGW(TAG, "  GUARDED '%s' '%s' f=%.2f Hz cycles=%u dnear=%.2f Hz"
                          " - rejected by %s (near=%d slow=%d)",
                     r.callsign, r.grid, r.freq_hz, r.cycles, dnear,
                     v == WSPR_GUARD_REJECT_NEAR ? "NEAR" : "SLOW",
                     would_near, would_slow);
            continue;
        }

        decoded++;
        wspr_accepted_add(&accepted, r.freq_hz);
        /* `agree` is the re-encode score - how well the received audio actually
         * supports this message (wspr_decode.h). It is logged on EVERY decode
         * on purpose: it is now the check that stands between us and
         * publishing a station that was never on the air, and a check nobody
         * can see is worth no more than no check at all. A field log therefore
         * carries the evidence to re-set WSPR_AGREE_MIN without a reflash. */
        ESP_LOGW(TAG, "  DECODED '%s' '%s' %d dBm  f=%.2f Hz dt=%.2fs cycles=%u"
                      " agree=%.3f/%.3f dnear=%.2f Hz would[near=%d slow=%d]",
                 r.callsign, r.grid, r.power_dbm, r.freq_hz,
                 r.best_dt_samples / WSPR_SAMPLE_RATE_HZ, r.cycles,
                 r.agree_hard, r.agree_soft, dnear, would_near, would_slow);
        file_spot(&r, cycle_utc, WSPR_SNR_UNKNOWN);
    }
    int64_t dec_ms = (esp_timer_get_time() - t0) / 1000;
    /* `skipped` is reported even when zero. A budget that quietly drops the tail
     * of the candidate list is the same invisible ceiling as the old cap of 8,
     * and the whole point of raising that cap was that nothing had ever said so. */
    ESP_LOGW(TAG, "cycle %lld: %d candidate(s), %d decode(s), %d guarded, "
                  "%d skipped (budget), %lld ms",
             (long long)cycle_utc, ncand, decoded, guarded, skipped,
             (long long)dec_ms);
    if (skipped)
        ESP_LOGW(TAG, "cycle %lld: BUDGET CUT %d candidate(s) after %lld ms - "
                      "lower WSPR_MAX_CANDS or make the decode faster",
                 (long long)cycle_utc, skipped, (long long)dec_ms);
    set_dec_status("%d decoded", decoded);
}

/* Nothing but "wait for a window, decode it, give the buffer back".
 *
 * ⛔ The buffer is released in a SINGLE place, after the decode returns, and the
 * capture task frees the memory only once this task has confirmed it is gone
 * (s_dec_exited). That ordering is not defensive habit: v0.19.5 crashed with
 * "Load address misaligned" because a teardown freed a stack while a worker
 * still held a pointer into it, and the fix was to JOIN rather than to delay. */
static void wspr_dec_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "decode task up");
    while (s_run) {
        wspr_dec_job_t job;
        if (xQueueReceive(s_dec_q, &job, pdMS_TO_TICKS(250)) != pdTRUE) continue;
        if (job.slot < 0 || job.slot >= WSPR_PCM_SLOTS) continue;
        decode_one_window(s_pcm[job.slot], job.cycle_utc);
        s_pcm_busy[job.slot] = false;
    }
    set_dec_status("%s", "");
    ESP_LOGI(TAG, "decode task stopped");
    s_dec_exited = true;
    vTaskDelete(NULL);
}

static void wspr_rx_task(void *arg)
{
    (void)arg;

    /* Two buffers, allocated once and reused for the life of the loop.
     *
     * float capture (5.76 MB) + int16 for the decoder (2.88 MB) = 8.6 MB of the
     * ~14.7 MB of free PSRAM. Allocating per cycle instead would risk failing on
     * a fragmented heap 20 minutes in, which is exactly the kind of fault that
     * shows up as "it stopped decoding overnight" and is miserable to chase. */
    float   *cap = heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(float),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* TWO int16 windows - the ping-pong. Only the int16 side is doubled: the
     * float buffer is fully consumed by the conversion before the next capture
     * opens, so a second one would be 5.6 MB of PSRAM bought for nothing. */
    bool pcm_ok = true;
    for (int i = 0; i < WSPR_PCM_SLOTS; i++) {
        s_pcm[i] = heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_pcm_busy[i] = false;
        if (!s_pcm[i]) pcm_ok = false;
    }
    if (!cap || !pcm_ok) {
        ESP_LOGE(TAG, "could not allocate the capture buffers (%u KB + %d x %u KB)",
                 (unsigned)(CAP_SAMPLES * sizeof(float) / 1024),
                 WSPR_PCM_SLOTS,
                 (unsigned)(CAP_SAMPLES * sizeof(int16_t) / 1024));
        free(cap);
        for (int i = 0; i < WSPR_PCM_SLOTS; i++) { free(s_pcm[i]); s_pcm[i] = NULL; }
        set_status("out of memory");
        s_run = false; s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "guards: near=%s (%.1f Hz)  slow=%s (%u cycles) - both always measured",
             s_guards.enforce_near ? "ENFORCED" : "measured", s_guards.near_hz,
             s_guards.enforce_slow ? "ENFORCED" : "measured", s_guards.slow_cycles);
    ESP_LOGI(TAG, "slot loop up: %u KB capture + %d x %u KB decode (ping-pong), "
                  "up to %d candidates, %d s budget, searching %.0f-%.0f Hz",
             (unsigned)(CAP_SAMPLES * sizeof(float) / 1024), WSPR_PCM_SLOTS,
             (unsigned)(CAP_SAMPLES * sizeof(int16_t) / 1024),
             WSPR_MAX_CANDS, WSPR_DECODE_BUDGET_MS / 1000,
             SEARCH_LO_HZ, SEARCH_HI_HZ);

    int64_t last_cycle_idx = -1;

    while (s_run) {
        /* ---- wait for the next even UTC minute, OR arm late-but-anchored ----
         *
         * ⛔ THIS IS WHAT ACTUALLY ENDED THE EVERY-OTHER-CYCLE DEAFNESS, and
         * splitting the decode onto its own task was NOT enough on its own.
         *
         * A WSPR capture is 120 s and a cycle is 120 s, so there is no slack:
         * the next capture has to begin the instant the previous one ends. The
         * float->int16 conversion (~2 s over 1.44 M samples) and, when armed, a
         * 4.2 s SD dump sit in between - so by the time this loop came back
         * round, the boundary had just passed and the old `wait` sent it to
         * sleep for another 118 s. Measured after the ping-pong landed:
         * captures still armed 19:46:00 and 19:50:00, 240 s apart.
         *
         * The pre-ring already solves it. `backfill` prepends however late we
         * armed, and FT8_PRE_CAP is 180000 samples = 15 s of slack - far more
         * than the ~7 s worst case. So being a few seconds late costs NOTHING:
         * the window is still anchored to the boundary, sample-exact.
         *
         * The cycle-index guard is not optional. Without it the SIM path, which
         * synthesizes a window in a moment rather than capturing for 120 s,
         * would come straight back inside the grace window and re-run the same
         * cycle in a tight loop. */
        int64_t t = now_ms();
        int64_t into = t % WSPR_CYCLE_MS;
        int64_t cyc  = t / WSPR_CYCLE_MS;
        int64_t wait = (into <= WSPR_ARM_GRACE_MS && cyc != last_cycle_idx)
                     ? 0 : (WSPR_CYCLE_MS - into);
        set_status("waiting %llds", (long long)(wait / 1000));
        while (s_run && wait > 0) {
            int64_t chunk = wait > 500 ? 500 : wait;   /* stay responsive to stop */
            vTaskDelay(pdMS_TO_TICKS((uint32_t)chunk));
            t = now_ms();
            into = t % WSPR_CYCLE_MS;
            wait = (into == 0) ? 0 : (WSPR_CYCLE_MS - into);
            if (wait > WSPR_CYCLE_MS - 100) break;     /* boundary just passed */
        }
        if (!s_run) break;

        int64_t cycle_utc = (now_ms() / WSPR_CYCLE_MS) * (WSPR_CYCLE_MS / 1000);
        last_cycle_idx = now_ms() / WSPR_CYCLE_MS;

        /* ---- duty-cycle scheduler ------------------------------------
         *
         * WSPR's convention is "transmit in this fraction of slots, AT
         * RANDOM", which is why the control is a duty cycle and not a
         * transmit button. The randomness is load-bearing, not decoration:
         * stations on a fixed schedule collide with the same neighbours
         * forever.
         *
         * ⚠ ORDER MATTERS, and it is the opposite of what it first looks.
         * The obvious design is "roll at the top of cycle N, arm for N+1",
         * and it is WRONG here - measured, not reasoned: this loop reaches
         * the top of a cycle exactly ON the even minute, and
         * wspr_tx_arm() then treats THAT minute as its slot. The burst
         * started 1000 ms after the arm, i.e. WSPR_TX_START_OFFSET_MS, in
         * the same cycle. So the arm is for THIS cycle, and the receiver
         * stand-down has to come AFTER it, not before.
         *
         * Getting this backwards is invisible in simulation - the sim
         * synthesizes its window instead of capturing, so nothing clashes -
         * and live would have spent 120 s capturing our own 110 s
         * transmission. */
        qmx_settings_t ws;
        settings_load_all(&ws);

        char txtext[64];

        /* A burst still running from the previous cycle owns the radio. */
        if (wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
            set_status("transmitting");
            ESP_LOGW(TAG, "cycle %lld: TX still busy - receiver stood down",
                     (long long)cycle_utc);
            while (s_run && wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        if (ws.wspr_tx_en && ws.wspr_duty_pct > 0 &&
            (esp_random() % 100u) < ws.wspr_duty_pct) {
            wspr_tx_request_t req;
            char err[80] = "";
            if (!ws.my_callsign[0] || !ws.my_grid[0]) {
                ESP_LOGW(TAG, "TX skipped: callsign/grid not set");
            } else if (!wspr_tx_build_request(ws.my_callsign, ws.my_grid,
                                              ws.wspr_tx_dbm, WSPR_TX_DEFAULT_FREQ_HZ,
                                              &req, err, sizeof(err))) {
                ESP_LOGW(TAG, "TX skipped: %s", err);
            } else if (!wspr_tx_arm(&req, err, sizeof(err))) {
                ESP_LOGW(TAG, "TX arm refused: %s", err);
            } else {
                ESP_LOGW(TAG, "TX armed for THIS cycle: %s %s %d dBm (duty %u%%)",
                         ws.my_callsign, ws.my_grid, ws.wspr_tx_dbm,
                         (unsigned)ws.wspr_duty_pct);
            }
        }

        /* Re-read AFTER the arm - see the ordering note above. */
        if (wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
            set_status("transmitting");
            ESP_LOGW(TAG, "cycle %lld: TX cycle - receiver stood down",
                     (long long)cycle_utc);
            while (s_run && wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        /* ---- SIMULATION: synthesize the window instead of capturing it ----
         *
         * Everything downstream is untouched - the same waterfall, the same
         * decoder, the same spot store, the same page. That is the whole value:
         * a sim that shortcut the decoder would test nothing and would quietly
         * misrepresent what the radio can hear. If a phantom does not decode,
         * it does not appear.
         *
         * No 120 s wait either. The audio is generated in a moment, so results
         * land ~70 s into the cycle instead of ~190 s, which makes this usable
         * for practice and for iterating on the display. */
        if (wspr_sim_enabled()) {
            set_status("simulating");
            int sslot = claim_pcm_slot();
            if (sslot < 0) {
                ESP_LOGW(TAG, "sim: both decode buffers busy - skipping a cycle");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            wspr_sim_build_window(s_pcm[sslot], CAP_SAMPLES, cycle_utc);
            build_waterfall(s_pcm[sslot], CAP_SAMPLES);
            /* Through the SAME queue as a real window - a sim that took a
             * shortcut past the handoff would stop exercising the thing most
             * likely to be wrong about it. */
            wspr_dec_job_t sjob = { .slot = sslot, .cycle_utc = cycle_utc };
            if (!s_dec_q || xQueueSend(s_dec_q, &sjob, 0) != pdTRUE) {
                ESP_LOGE(TAG, "sim: decode queue full - dropping window");
                s_pcm_busy[sslot] = false;
            }
            continue;
        }

        /* ---- capture the window ----
         * backfill covers however late we armed: the pre-ring is already being
         * filled continuously by the DSP in this mode, so the window is anchored
         * to the boundary rather than to when this task got a slice. Same
         * mechanism, and same reason, as the FT8 boundary-discard fix. */
        uint32_t start_off_ms = (uint32_t)(now_ms() % WSPR_CYCLE_MS);
        uint32_t backfill     = start_off_ms * (uint32_t)(WSPR_SAMPLE_RATE_HZ / 1000);
        /* Allocate and blank BEFORE the capture opens: the wait loop starts
         * painting rows the moment samples land.
         *
         * ⛔ The return value is LOAD-BEARING. wf_row() advances s_wf_rows_done
         * itself, and early-returns without advancing it when the magnitude
         * store could not be allocated - so driving the row loop after a failed
         * wf_begin() would spin forever and hang the capture, turning an
         * out-of-memory condition into a dead receiver. */
        const bool wf_live = wf_begin();
        esp_err_t err = dsp_ft8_capture_begin(cap, CAP_SAMPLES, backfill);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "capture_begin failed (%s) - skipping this cycle",
                     esp_err_to_name(err));
            set_status("capture failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "cycle %lld: capturing (armed +%u ms)",
                 (long long)cycle_utc, (unsigned)start_off_ms);

        while (s_run) {
            int got = dsp_ft8_capture_progress();

            /* ---- LIVE WATERFALL ----
             * Every row whose samples have landed is drawn now, so the pane
             * scrolls while the band is being heard instead of showing nothing
             * for two minutes and everything at once. Rows are painted against
             * the PREVIOUS cycle's floor; wf_finalise() below repaints the lot
             * against this cycle's own median once it exists. */
            while (wf_live && s_wf_rows_done < WSPR_WF_ROWS &&
                   (long)got >= wf_samples_for_row(s_wf_rows_done)) {
                wf_row(cap, NULL, (long)got, s_wf_rows_done);
                if (!s_run) break;
            }

            if (got >= (int)CAP_SAMPLES) break;
            int64_t elapsed = now_ms() % WSPR_CYCLE_MS;
            if (elapsed > WSPR_CYCLE_MS - 500 && got > 0) break;  /* boundary */
            set_status("capturing %d/120 s", got / (int)WSPR_SAMPLE_RATE_HZ);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        dsp_ft8_capture_finish(2000);
        if (!s_run) break;

        /* A slot is claimed AFTER the capture, not before: the decoder may well
         * have freed one during those 120 s, and claiming early would drop a
         * cycle that had somewhere to go by the time it mattered. */
        int slot = claim_pcm_slot();
        if (slot < 0) {
            ESP_LOGE(TAG, "cycle %lld: both decode buffers still busy - "
                          "DROPPING this window", (long long)cycle_utc);
            set_status("decoder behind");
            continue;
        }
        int16_t *pcm = s_pcm[slot];

        /* ---- float -> int16 for the decoder ----
         * The capture pipeline produces floats; the decoder takes int16, the
         * format every WSPR reference recording uses and the one the host
         * harnesses are validated against. Converting is cheap next to the
         * decode and keeps the decoder byte-identical to the tested one. */
        /* ---- float -> int16, NORMALISED ----
         *
         * MEASURED, and it overturned the obvious assumption. The capture
         * pipeline hands back floats built from raw int16 IQ, so a straight cast
         * looked right - but the real levels off the air are
         * min=-57.5 max=22.2 rms=3.7. Casting those to int16 gives the decoder
         * about SIX bits of a sixteen-bit format and silently throws ~9 bits of
         * dynamic range away. It still decoded a strong Australian beacon, which
         * is exactly why this would have gone unnoticed: the failure is not "no
         * decodes", it is "only the loud ones".
         *
         * A per-capture gain is the correct fix rather than a fudge: every stage
         * downstream - the comb search, the sync correlation, the Fano metric -
         * works on RELATIVE power within the capture, so scaling the whole window
         * changes nothing except how much of the int16 range is used.
         *
         * Guarded against silence: a capture of near-nothing must not be
         * amplified into full-scale noise, which would manufacture candidates
         * out of the noise floor. */
        float peak = 0.0f;
        for (uint32_t i = 0; i < CAP_SAMPLES; i++) {
            float a = cap[i] < 0 ? -cap[i] : cap[i];
            if (a > peak) peak = a;
        }
        float gain = 1.0f;
        if (peak > 1e-3f) {
            gain = 28000.0f / peak;          /* headroom below full scale */
            if (gain > 4096.0f) gain = 4096.0f;   /* silence guard */
            if (gain < 1.0f)    gain = 1.0f;      /* already large: don't attenuate */
        }
        double sumsq = 0;
        for (uint32_t i = 0; i < CAP_SAMPLES; i++) {
            float v = cap[i] * gain;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            sumsq += (double)v * v;
            pcm[i] = (int16_t)v;
        }
        ESP_LOGW(TAG, "capture level: peak=%.1f gain=%.0fx -> rms=%.0f of 32768",
                 peak, gain, sqrt(sumsq / CAP_SAMPLES));

        /* The rows were drawn live as the capture filled; this only recomputes
         * the floor from the finished window and repaints against it. No FFTs,
         * so the ~8 s post-capture pause the old whole-window build cost is
         * gone - the picture was already there. */
        wf_finalise();

        /* Dump BEFORE the decode, so the file is byte-for-byte the audio the
         * decoder is about to be handed. Dumping after would let a future
         * in-place decode step change the samples and quietly answer a
         * different question than the one being asked. */
        {
            qmx_settings_t dst;
            settings_load_all(&dst);
            if (dst.wspr_dump_cycles > 0 && sd_archive_is_mounted()) {
                dump_window(pcm, CAP_SAMPLES, cycle_utc);
                /* Decrement only after a write ATTEMPT, so a card that fails
                 * mid-run cannot spin forever on the same cycle - and only
                 * when a card is present, so an armed request survives until
                 * there is somewhere to write it. */
                settings_set_wspr_dump_cycles((uint8_t)(dst.wspr_dump_cycles - 1));
            }
        }

        /* The carpet stops advancing from here until the next capture opens -
         * ~68 s in which the receiver is genuinely DEAF, not merely idle. Mark
         * it, or a stalled carpet is indistinguishable from a hung display. */
        wf_mark_boundary();

        /* Hand the finished window to the decode task and go straight back to
         * the next boundary. THIS is what ends the every-other-cycle deafness:
         * the capture below starts on time while this window is still decoding. */
        {
            wspr_dec_job_t job = { .slot = slot, .cycle_utc = cycle_utc };
            if (!s_dec_q || xQueueSend(s_dec_q, &job, 0) != pdTRUE) {
                ESP_LOGE(TAG, "cycle %lld: decode queue full - dropping window",
                         (long long)cycle_utc);
                s_pcm_busy[slot] = false;
            }
        }
    }

    /* ⛔ JOIN, DO NOT DELAY. The decode task reads s_pcm[] and this task owns
     * that memory, so freeing before it has exited is a use-after-free. v0.19.5
     * crashed exactly this way ("Load address misaligned") because a teardown
     * used a fixed 50 ms sleep instead of waiting for the worker to confirm it
     * was gone. The decode loop checks s_run between candidates, so the wait is
     * bounded by one candidate (~8 s); 30 s is generous cover for a slow one. */
    for (int i = 0; i < 300 && !s_dec_exited; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_dec_exited)
        ESP_LOGE(TAG, "decode task did not exit - LEAKING the windows rather "
                      "than freeing memory it may still be reading");

    free(cap);
    if (s_dec_exited) {
        for (int i = 0; i < WSPR_PCM_SLOTS; i++) { free(s_pcm[i]); s_pcm[i] = NULL; }
    }
    if (s_dec_q) { vQueueDelete(s_dec_q); s_dec_q = NULL; }
    s_dec_task = NULL;
    set_status("idle");
    set_dec_status("%s", "");
    ESP_LOGI(TAG, "slot loop stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

bool wspr_rx_start(void)
{
    if (s_run) return true;

    /* Claimed BEFORE the task exists, and cleared if creation fails - #199: a
     * flag set as the task's first statement means "has begun running" while
     * every reader needs "exists", and on this board the gap between the two is
     * long enough to matter. */
    if (!s_wf_mtx) s_wf_mtx = xSemaphoreCreateMutex();
    /* Only if untouched: a wspr_guards dev action set before the page is
     * entered must survive starting the loop, or an experiment silently
     * reverts to defaults the moment it is run. */
    if (s_guards.near_hz <= 0.0) wspr_guards_defaults(&s_guards);
    s_run = true;
    ui_mode_set(UI_MODE_WSPR);

    /* 32 KB and in PSRAM: wspr_decode_candidate() alone reserves a flat 16 KB
     * frame in its prologue, the self-test measured ~27.5 KB of high-water, and
     * xTaskCreate() would take that from the ~40 KB of free INTERNAL RAM. This
     * is background work on a two-minute cadence, which is what
     * psram_task_create() is for. */
    /* The decode task and its queue come up FIRST, so the capture task can never
     * finish a window and find nowhere to hand it. Same stack budget and the
     * same reasoning as the capture task - this is where the decode actually
     * runs now, so it is the one that needs the 32 KB. */
    s_dec_exited = false;
    for (int i = 0; i < WSPR_PCM_SLOTS; i++) s_pcm_busy[i] = false;
    if (!s_dec_q) s_dec_q = xQueueCreate(WSPR_PCM_SLOTS, sizeof(wspr_dec_job_t));
    if (!s_dec_q) {
        ESP_LOGE(TAG, "could not create the decode queue");
        s_run = false;
        ui_mode_set(UI_MODE_PANADAPTER);
        return false;
    }
    s_dec_task = psram_task_create(wspr_dec_task, "wspr_dec", 32768, NULL,
                                   tskIDLE_PRIORITY + 1, tskNO_AFFINITY);
    if (!s_dec_task) {
        ESP_LOGE(TAG, "could not create the decode task");
        vQueueDelete(s_dec_q); s_dec_q = NULL;
        s_run = false;
        ui_mode_set(UI_MODE_PANADAPTER);
        return false;
    }

    s_task = psram_task_create(wspr_rx_task, "wspr_rx", 32768, NULL,
                               tskIDLE_PRIORITY + 1, tskNO_AFFINITY);
    if (!s_task) {
        ESP_LOGE(TAG, "could not create the slot-loop task");
        s_run = false;   /* stands the decode task down too */
        ui_mode_set(UI_MODE_PANADAPTER);
        return false;
    }
    return true;
}

void wspr_rx_stop(void)
{
    if (!s_run) return;
    s_run = false;
    /* The task frees its own buffers and clears s_task; the mode goes back here
     * so the panadapter starts drawing again immediately rather than waiting for
     * a capture in flight to finish. */
    ui_mode_set(UI_MODE_PANADAPTER);
}

void wspr_rx_set_guards(int enforce_near, double near_hz,
                        int enforce_slow, unsigned int slow_cycles)
{
    if (s_guards.near_hz <= 0.0) wspr_guards_defaults(&s_guards);
    s_guards.enforce_near = enforce_near ? 1 : 0;
    s_guards.enforce_slow = enforce_slow ? 1 : 0;
    if (near_hz     > 0.0) s_guards.near_hz     = near_hz;
    if (slow_cycles > 0u)  s_guards.slow_cycles = slow_cycles;
    ESP_LOGW(TAG, "guards now: near=%s (%.1f Hz)  slow=%s (%u cycles)",
             s_guards.enforce_near ? "ENFORCED" : "measured", s_guards.near_hz,
             s_guards.enforce_slow ? "ENFORCED" : "measured", s_guards.slow_cycles);
}
