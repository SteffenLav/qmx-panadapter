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
#include "esp_attr.h"        /* EXT_RAM_BSS_ATTR - see wf_finalise() */
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
#include "wspr_subtract.h"   /* two-pass: take a decoded signal back out */
#include "wspr_sim.h"
#include "wspr_tx.h"
/* A burst closer than this owns the cycle, so do not open a 120 s capture in
 * front of it. Anything further away is a whole cycle we can still listen in. */
#define WSPR_RX_TX_IMMINENT_S  10
#include "cat/cat.h"   /* #290 PA-voltage guard */
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
 * just has to be high enough not to be the limiter itself.
 *
 * ⚠ STAYS AT 20, AND THAT IS A MEASURED DECISION, NOT INERTIA. Every cycle on
 * air reports exactly 20 candidates, so this cap IS saturated 100 % of the
 * time - the same shape as the "8 candidates" ceiling described above, which
 * is a fair reason to suspect it. Swept on the four reference WAVs after the
 * 2026-08-27 speed work made room for more:
 *
 *     cap   20    24    32    48
 *     found 24    24    24    24
 *
 * NOT ONE extra station, while the correlation work rises by 95 %. So on this
 * evidence the cap is not what is limiting us and raising it would buy nothing
 * but heat.
 *
 * ⛔ WHAT THE REFERENCE FILES CANNOT TELL US is whether that holds on a
 * genuinely crowded band - four recordings from two sites is a thin sample,
 * and the comb ranks by ENERGY rather than SNR, which is exactly how a strong
 * station ended up below eight noisier peaks last time. If a real session ever
 * shows stations appearing only when the cap is lifted, this is the number to
 * raise; do not raise it on the strength of a reference file that says no. */
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
/* Newest at s_hist_n-1 once full. A plain shift keeps the getter trivial, and
 * 40 bytes is not worth a ring's index arithmetic. */
static uint8_t s_hist[WSPR_CYCLE_HISTORY];
static int     s_hist_n;

/* ---- the transmit SCHEDULE ------------------------------------------------
 *
 * ⭐ THE DUTY-CYCLE ROLL IS TAKEN IN ADVANCE, and that is the whole point.
 *
 * It used to be taken AT the cycle boundary, for that same cycle. Nothing could
 * then say when the next burst was, because nothing had decided yet - so the
 * TX button counted down to the next SLOT, i.e. to the next opportunity, and
 * every time the roll lost it reset and counted down again. The operator,
 * 2026-09-02: "when it reached 00:00 then it started counting down again
 * 01:20(!) I need to see a REAL count down to the next TX."
 *
 * So the roll is moved forward in time: one cycle index is chosen and held, the
 * countdown reads it, and the boundary merely acts on a decision already made.
 *
 * ⚠ THE RANDOMNESS IS UNCHANGED, and that matters - it is load-bearing, not
 * decoration (stations on a fixed schedule collide with the same neighbours for
 * ever). Rolling each future cycle in turn until one wins gives exactly the
 * geometric gap that independent per-cycle rolls give; the only difference is
 * that the coin is tossed before the cycle rather than at it. Duty 100 still
 * means every cycle, duty 0 still means never.
 *
 * ⚠ And a schedule can be OVERTAKEN. The PA guard can hold a burst, a build can
 * fail, a callsign can be missing - in each case the cycle passes without
 * transmitting and the next one is rolled from there, so the countdown jumps to
 * a later time rather than sitting at zero claiming a burst that is not coming.
 */
static int64_t now_ms(void);           /* UTC ms - the cycle index is UTC-aligned */
static int64_t s_next_tx_cycle = -1;   /* cycle index; -1 = nothing scheduled */
static uint8_t s_sched_duty    = 0;    /* the duty this schedule was rolled at */

/* First cycle AFTER `after` that wins the duty roll. */
static int64_t roll_next_tx_cycle(int64_t after, uint8_t duty)
{
    if (duty == 0) return -1;
    if (duty >= 100) return after + 1;
    /* Bounded so a corrupt duty can never spin here. At the lowest duty this
     * offers (10%) the chance of 2000 straight losses is about 10^-92, so the
     * bound is a safety net and not a behaviour. */
    for (int i = 1; i <= 2000; i++)
        if ((esp_random() % 100u) < duty) return after + i;
    return -1;
}

void wspr_rx_tx_schedule_reset(bool tx_en, uint8_t duty_pct)
{
    /* ⚠ Takes the two values it needs as ARGUMENTS rather than reading the
     * settings itself. Both callers are UI paths - the Tab5's TX button on
     * taskLVGL and the /api/settings handler on httpd - and settings_load_all()
     * is a multi-kilobyte struct on the caller's stack. That is the bug class
     * this board has hit four times; see "Task stacks on this board are TINY". */
    if (!tx_en || duty_pct == 0) {
        s_next_tx_cycle = -1;
        s_sched_duty    = 0;
        return;
    }
    /* Rolled from the CURRENT cycle, so the earliest possible burst is the next
     * boundary and a countdown appears the instant the operator presses TX ON -
     * rather than after up to two minutes of the button saying nothing. */
    s_next_tx_cycle = roll_next_tx_cycle(now_ms() / WSPR_CYCLE_MS, duty_pct);
    s_sched_duty    = duty_pct;
}

int wspr_rx_seconds_to_next_tx(void)
{
    int64_t sched = s_next_tx_cycle;          /* one read - the loop may write */
    if (sched < 0) return -1;
    int64_t ms = sched * WSPR_CYCLE_MS - now_ms();
    if (ms < 0) return -1;                    /* overtaken; the loop re-rolls */
    return (int)(ms / 1000);
}

int wspr_rx_cycle_history(uint8_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = s_hist_n < max ? s_hist_n : max;
    memcpy(out, s_hist + (s_hist_n - n), (size_t)n);
    return n;
}

static void hist_push(int decoded)
{
    if (decoded > 255) decoded = 255;
    if (s_hist_n >= WSPR_CYCLE_HISTORY) {
        memmove(s_hist, s_hist + 1, WSPR_CYCLE_HISTORY - 1);
        s_hist_n = WSPR_CYCLE_HISTORY - 1;
    }
    s_hist[s_hist_n++] = (uint8_t)decoded;
}

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
/* The dial as of the window being queued. Kept as a file static so the decode
 * task, which runs later and on another core, files spots under the band they
 * were HEARD on rather than the band the hopper has since moved to. */
static uint32_t s_cycle_dial_hz;
static uint32_t wspr_rx_cycle_dial_hz(void)
{
    qmx_settings_t cs; settings_load_all(&cs);
    return cs.wspr_dial_hz;
}

static void file_spot(const wspr_decode_result_t *r, int64_t cycle_utc,
                       int snr_db, int drift_hz)
{
    wspr_spot_t sp;
    memset(&sp, 0, sizeof(sp));
    snprintf(sp.call, sizeof(sp.call), "%s", r->callsign);
    snprintf(sp.grid, sizeof(sp.grid), "%s", r->grid);
    sp.cycle_utc = cycle_utc;
    sp.freq_hz   = (float)r->freq_hz;
    sp.power_dbm = (int16_t)r->power_dbm;
    sp.snr_db    = (int16_t)snr_db;
    sp.drift_hz  = (int16_t)drift_hz;        /* 0 Hz is a REAL value, not "unset" */
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
    sp.dial_hz = s_cycle_dial_hz;   /* the band it was HEARD on - see wspr_dec_job_t */
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
static float  s_wf_floor;        /* rolling per-row noise floor - see wf_row() */
/* ~3-4 rows to settle. Faster bands the picture on median noise; slower brings
 * back the per-cycle step this replaced. */
#define WF_FLOOR_ALPHA 0.25f
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

    /* ---- THE FLOOR IS TRACKED PER ROW, NOT PER CYCLE ------------------
     *
     * ⛔ IT USED TO BE ONE MEDIAN FOR A WHOLE 120 s CYCLE, and that is what
     * made the carpet visibly change level every couple of minutes: every row
     * of a cycle was painted against the PREVIOUS cycle's median while it
     * arrived, then wf_finalise() repainted all ~35 of them against this
     * cycle's own. So the display moved in one step, two minutes wide, and the
     * operator could see it happening without knowing why.
     *
     * Now each row takes the median of its own 205 bins - robust, because WSPR
     * occupies only a few of them - and that feeds a short EMA. WF_FLOOR_ALPHA
     * of 1/4 settles in about three or four rows, which is the operator's own
     * instinct ("maybe 2-3 of them"): fast enough to follow a band change,
     * slow enough that row-to-row median noise does not band the picture.
     *
     * ⚠ THE TRADE, STATED PLAINLY: a tracked floor normalises away slow
     * broadband changes, so the carpet no longer shows "the band got noisier" -
     * it shows what is above the noise, which is what a WSPR display is for.
     * CLAUDE.md records the panadapter's version of this going too far, where
     * a fully adaptive per-bin floor made steady carriers FADE OUT over ~60 s.
     * That cannot happen here because the floor is one number per row taken
     * from the MEDIAN across frequency: a carrier occupies a handful of the
     * 205 bins and can never move it. Do not make this per-bin. */
    {
        EXT_RAM_BSS_ATTR static float med[WSPR_WF_COLS];
        memcpy(med, magrow, sizeof(med));
        for (int a = 1; a < WSPR_WF_COLS; a++) {
            float v = med[a]; int b = a - 1;
            while (b >= 0 && med[b] > v) { med[b + 1] = med[b]; b--; }
            med[b + 1] = v;
        }
        const float m = med[WSPR_WF_COLS / 2];
        if (m > 0.0f) {
            if (s_wf_floor <= 0.0f) s_wf_floor = m;      /* first row ever */
            else s_wf_floor += WF_FLOOR_ALPHA * (m - s_wf_floor);
        }
    }

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
/* ⛔ A BAND CHANGE INVALIDATES THE FLOOR COMPLETELY, and nothing used to say
 * so. The floor was carried across the hop, so the first rows on the new band
 * were painted against the OLD band's noise - and since 30 m in the evening
 * sits well above 20 m, every bin read far over the stale floor and the top of
 * the carpet came out saturated red. Photographed on the bench immediately
 * after a 20 -> 30 m hop.
 *
 * Zeroing it makes the next row re-seed from its own median, which is the same
 * path the very first row after boot takes. */
void wspr_rx_wf_floor_reset(void)
{
    s_wf_floor = 0.0f;
}

static void wf_finalise(void)
{
    if (!s_wf || !s_wf_mag) return;

    /* ⛔ PSRAM, NOT INTERNAL RAM. 8 KB of internal .bss for a scratch array
     * touched once every 120 s is the most expensive idle memory in the
     * firmware: CLAUDE.md records that recovering 14,084 bytes of exactly this
     * kind was the whole difference between an OTA download completing and
     * taking four hardware watchdog resets. `static` is still right - the WSPR
     * task's stack cannot carry 8 KB - but the section is not.
     *
     * ⚠ .bss is allocated whether the feature RUNS or not, so no amount of
     * gating WSPR off would have made this free. That is the point. */
    EXT_RAM_BSS_ATTR static float samp[2048];
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

    /* ⛔ NO WHOLE-CYCLE REPAINT ANY MORE. Every row of this cycle used to be
     * redrawn here against one median, which is exactly what made the carpet
     * jump a level every two minutes - and it also rewrote rows the operator
     * had already been reading. Each row now carries the floor that was true
     * when it arrived (wf_row's rolling EMA), so there is nothing to correct
     * and history stays as it was drawn. The median below is kept only for the
     * log line, which is a useful record of where the floor sat. */
    if (s_wf_mtx) xSemaphoreTake(s_wf_mtx, portMAX_DELAY);
    /* ⛔ The head is advanced HERE as well as in wf_publish(), because on the
     * very first cycle after boot s_wf_floor is still 0, so wf_row() computes
     * magnitudes but publishes nothing - and a head left at the cycle base
     * would make the next cycle overwrite this one in place, i.e. no carpet at
     * all until the second cycle. Idempotent when rows did publish. */
    s_wf_head = wf_ring_idx(s_wf_rows_done);
    s_wf_seq++;
    if (s_wf_mtx) xSemaphoreGive(s_wf_mtx);

    /* s_wf_floor is NOT overwritten here: it is a rolling per-row value now and
     * slamming it to a whole-cycle median would put the two-minute step back. */
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

/* Up to ~20 s of retries: an FT8 slot is 15 s and that is the longest the
 * previous page can hold its pool after the mode has changed. */
#define WSPR_ALLOC_TRIES    21
#define WSPR_ALLOC_WAIT_MS 1000

static int16_t      *s_pcm[WSPR_PCM_SLOTS];
static volatile bool s_pcm_busy[WSPR_PCM_SLOTS];
static QueueHandle_t s_dec_q;
static TaskHandle_t  s_dec_task;
static volatile bool s_dec_exited;

typedef struct {
    int     slot;
    int64_t cycle_utc;
    /* ⭐ THE DIAL THIS WINDOW WAS CAPTURED ON, carried with the job.
     *
     * Reading the dial when the SPOT is filed would attribute it to the wrong
     * band: band hopping retunes HOP_LEAD_SEC before the next cycle, and a
     * decode routinely finishes after that, so spots from the cycle just ended
     * would be filed under the band we have already moved to.
     *
     * ⚠ IT IS READ AT CAPTURE-BEGIN, NOT AT QUEUE TIME. This comment used to
     * say "the moment the window is queued ... which is what the samples
     * actually belong to", and that was wrong by up to the length of the
     * waterfall build plus an SD dump: the queue happens at the END of the
     * cycle, and the hop retunes in its last 3 seconds. Kevin KQ4DTX saw spots
     * labelled with the band we had just moved to. The dial is now taken before
     * the first sample, where nothing can overtake it. */
    uint32_t dial_hz;
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
/* ---- TWO PASSES, WITH THE FIRST PASS'S SIGNALS TAKEN OUT IN BETWEEN -------
 *
 * ⭐ AFFORDABLE ONLY SINCE THE FLOAT CONVERSION. A candidate cost 11.7 s and
 * the budget fit nine of twenty; it now costs ~2.3 s gated, ~3 s decoded, so a
 * whole second pass fits inside the same 115 s with room left.
 *
 * WHY IT IS WORTH SPENDING THAT ON. Stations sit on top of each other: across
 * the reference windows, nine of the stations wsprd finds are within 6 Hz of
 * another. The candidate finder now resolves most of those (it notches comb
 * sidelobes instead of blanking 5.9 Hz), but a weak station under a strong
 * neighbour is still hidden by the neighbour, not by the search. Subtracting
 * what we already decoded is what uncovers it, and it is what wsprd does.
 *
 * Host-measured on the four reference WAVs, this recovers DD3MS, G4FBA, W3BI,
 * PA4JAM and 2E0DLC - all confirmed by wsprd, none reachable in one pass.
 *
 * ⚠ THE BUFFER IS MODIFIED IN PLACE, hence the non-const parameter. That is
 * safe because this slot is handed back to the capture task immediately after
 * this function returns and is fully overwritten before it is read again - but
 * it is exactly the kind of assumption that stops being true quietly, so if a
 * second reader of `pcm` ever appears, this needs its own scratch copy (2.9 MB
 * of PSRAM) rather than a re-reading of this comment. */
#define WSPR_PASSES 2

/* How far from a subtracted signal a later pass still bothers to look. A WSPR
 * transmission is 5.9 Hz wide and the cases this exists for - stations sharing
 * one candidate - sat 2 to 4 Hz apart, so 15 Hz is generous to the effect
 * while still excluding the rest of a 300 Hz window. */
#define WSPR_PASS2_NEAR_HZ 15.0

static void decode_one_window(int16_t *pcm, int64_t cycle_utc)
{
    /* ---- decode ---- */
    int64_t t0 = esp_timer_get_time();
    wspr_freq_candidate_t cands[WSPR_MAX_CANDS];
    int ncand = 0;
    int decoded = 0;
    int guarded = 0;
    wspr_accepted_t accepted = { 0 };
    int skipped = 0;
    int pass = 0;
    int subtracted = 0;
    int found_in_pass = 0;
    /* Where the first pass removed something. A later pass only has a reason
     * to look THERE - see the skip below. */
    double subbed_hz[WSPR_MAX_CANDS];
    int    nsubbed = 0;
    int    tried_this_pass = 0;
    /* Callsigns already reported this cycle. A station re-decoded after its own
     * subtraction is the same reception report, not a second one. */
    char seen[WSPR_MAX_CANDS][7];
    int nseen = 0;

  next_pass:
    ncand = wspr_find_candidates(pcm, CAP_SAMPLES, SEARCH_LO_HZ, SEARCH_HI_HZ,
                                  cands, WSPR_MAX_CANDS);
    found_in_pass = 0;
    tried_this_pass = 0;
    for (int i = 0; i < ncand && s_run; i++) {
        /* ⛔ A SECOND PASS MUST NOT RE-SCAN THE WHOLE BAND. Measured on
         * hardware: pass 1 took 82 s of the 115 s budget, pass 2 then started
         * again from candidate 0 and was cut after two - so it cost a full
         * pass's worth of budget to look mostly where nothing had changed.
         *
         * Subtraction can only ever reveal something NEAR a signal that was
         * removed; everywhere else the audio is bit-identical to pass 1 and
         * re-decoding it is guaranteed to reach the same answer. So a later
         * pass looks only around what it took out, which turns a second full
         * pass into a handful of candidates. */
        if (pass > 0) {
            int near = 0;
            for (int k = 0; k < nsubbed; k++)
                if (fabs(cands[i].freq_hz - subbed_hz[k]) < WSPR_PASS2_NEAR_HZ) { near = 1; break; }
            if (!near) continue;
        }
        tried_this_pass++;
        /* ⛔ The budget is checked BEFORE starting a candidate, never mid-way:
         * wspr_decode_candidate() is not interruptible, so a check inside it
         * would either do nothing or leave a half-decoded result. */
        int64_t used_ms = (esp_timer_get_time() - t0) / 1000;
        if (used_ms > WSPR_DECODE_BUDGET_MS) {
            /* ⚠ COUNT WHAT WOULD ACTUALLY HAVE BEEN TRIED. In a later pass most
             * of the remaining candidates are nowhere near a subtracted signal
             * and would have been passed over for free, so `ncand - i` reports
             * a loss that was never going to happen - it read as "13 skipped"
             * where the real number was a handful. Reporting a cut honestly is
             * the whole reason this line distinguishes the passes at all. */
            for (int j = i; j < ncand; j++) {
                if (pass == 0) { skipped++; continue; }
                for (int k = 0; k < nsubbed; k++)
                    if (fabs(cands[j].freq_hz - subbed_hz[k]) < WSPR_PASS2_NEAR_HZ) {
                        skipped++; break;
                    }
            }
            break;
        }
        set_dec_status("dec %d/%d", i + 1, ncand);
        wspr_decode_result_t r;
        wspr_decode_candidate(pcm, CAP_SAMPLES, cands[i].freq_hz, &r);
        /* Log EVERY candidate, not just the ones that decode. A silent
         * "0 decodes" cannot distinguish "nothing was on the air" from
         * "the search looked in the wrong place" from "the audio was
         * wrong" - and those need completely different fixes. */
        /* The phase breakdown is on EVERY candidate, decoded or not, because
         * the cost is what limits how much of the band gets looked at - the
         * budget fits nine candidates of twenty - and every attempt to reason
         * about where it goes from first principles has been wrong by orders
         * of magnitude. The host cannot measure it either: it has a hardware
         * double FPU and fast RAM where this board has neither. */
        ESP_LOGI(TAG, "  cand %d: f=%.2f Hz score=%.3g cycles=%u %s"
                      " [mix %.0f + coarse %.0f + curve %.0f + dec %.0f ms]",
                 i, cands[i].freq_hz, (double)cands[i].comb_score,
                 r.cycles, r.ok ? "DECODED" : "rejected",
                 r.ms_mix, r.ms_coarse, r.ms_curve, r.ms_decode);
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

        int dup = 0;
        for (int k = 0; k < nseen; k++)
            if (!strcmp(seen[k], r.callsign)) { dup = 1; break; }
        if (dup) continue;
        if (nseen < WSPR_MAX_CANDS) snprintf(seen[nseen++], 7, "%s", r.callsign);

        /* Take it out of the audio so the next pass can see underneath it.
         * Done for EVERY accepted decode including the last pass's - the cost
         * is one pass over the samples and it keeps the code honest about
         * which signals are still present. */
        if (pass + 1 < WSPR_PASSES) {
            uint8_t tones[WSPR_NSYM];
            if (wspr_tones_from_message(r.callsign, r.grid, r.power_dbm, tones) &&
                wspr_subtract(pcm, CAP_SAMPLES, r.freq_hz,
                              r.best_dt_samples, tones) > 0) {
                subtracted++;
                if (nsubbed < WSPR_MAX_CANDS) subbed_hz[nsubbed++] = r.freq_hz;
            }
        }

        decoded++;
        found_in_pass++;
        wspr_accepted_add(&accepted, r.freq_hz);
        /* `agree` is the re-encode score - how well the received audio actually
         * supports this message (wspr_decode.h). It is logged on EVERY decode
         * on purpose: it is now the check that stands between us and
         * publishing a station that was never on the air, and a check nobody
         * can see is worth no more than no check at all. A field log therefore
         * carries the evidence to re-set WSPR_AGREE_MIN without a reflash. */
        ESP_LOGW(TAG, "  DECODED '%s' '%s' %d dBm  f=%.2f Hz dt=%.2fs cycles=%u"
                      " snr=%d drift=%d agree=%.3f/%.3f dnear=%.2f Hz would[near=%d slow=%d]",
                 r.callsign, r.grid, r.power_dbm, r.freq_hz,
                 r.best_dt_samples / WSPR_SAMPLE_RATE_HZ, r.cycles,
                 r.snr_db, r.drift_hz, r.agree_hard, r.agree_soft, dnear, would_near, would_slow);
        /* MEASURED now (wspr_decode.c, measure_noise_ref) rather than the
         * placeholder this used to pass. Validated against wsprd on the four
         * reference recordings: 23 stations, median 1 dB low, stdev 2.3 dB -
         * which is inside the spread between real WSPR reporters. It stays
         * WSPR_SNR_UNKNOWN if the decoder could not measure it; the row prints
         * a dash, never an invented number. */
        file_spot(&r, cycle_utc, r.snr_db, r.drift_hz);
    }
    /* Only bother with another pass if this one actually removed something -
     * re-running the finder over unchanged audio would return the identical
     * candidates and waste half the budget proving it. */
    if (++pass < WSPR_PASSES && found_in_pass > 0 && s_run &&
        (esp_timer_get_time() - t0) / 1000 < WSPR_DECODE_BUDGET_MS) {
        ESP_LOGI(TAG, "  pass %d: %d signal(s) subtracted, looking underneath "
                      "(only within %.0f Hz of one)", pass, subtracted,
                 (double)WSPR_PASS2_NEAR_HZ);
        goto next_pass;
    }

    int64_t dec_ms = (esp_timer_get_time() - t0) / 1000;
    /* `skipped` is reported even when zero. A budget that quietly drops the tail
     * of the candidate list is the same invisible ceiling as the old cap of 8,
     * and the whole point of raising that cap was that nothing had ever said so. */
    ESP_LOGW(TAG, "cycle %lld: %d candidate(s), %d decode(s), %d guarded, "
                  "%d skipped (budget, pass %d), %d pass(es), %d subtracted, %lld ms",
             (long long)cycle_utc, ncand, decoded, guarded, skipped,
             pass, pass, subtracted, (long long)dec_ms);
    /* ⚠ WHICH PASS was cut is the whole meaning of this warning. A cut in pass
     * 1 means part of the band was never looked at; a cut in a later pass just
     * means the cheap second look ran out of time, which costs at most a
     * station hiding under another one. Reporting them the same way read as a
     * failure when pass 1 had in fact completed all 20 candidates. */
    if (skipped)
        ESP_LOGW(TAG, "cycle %lld: BUDGET CUT %d candidate(s) in pass %d after "
                      "%lld ms%s",
                 (long long)cycle_utc, skipped, pass, (long long)dec_ms,
                 pass > 1 ? " - second-look only, the band was fully scanned"
                          : " - lower WSPR_MAX_CANDS or make the decode faster");
    hist_push(decoded);
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
        s_cycle_dial_hz = job.dial_hz;   /* before decoding, so every spot from
                                            this window is filed under it */
        decode_one_window(s_pcm[job.slot], job.cycle_utc);
        s_pcm_busy[job.slot] = false;
    }
    set_dec_status("%s", "");
    ESP_LOGI(TAG, "decode task stopped");
    s_dec_exited = true;
    psram_task_park();
}

/* ---- WSPR PA-voltage guard (#290) --------------------------------------
 *
 * WSPR keys the PA for ~110 s out of every 120. Nothing else this radio does
 * approaches that duty cycle - an FT8 burst is ~12.6 s - and the QMX finals
 * overheat at full power on it. QRP Labs guard against this in the radio's OWN
 * Virtual U3S WSPR, which runs the PA at 50% voltage (a quarter of the power);
 * our WSPR TX is CAT-driven (TX;/TA;/RX;) and never enters that mode, so it
 * inherits none of that protection. This applies the same guard.
 *
 * EDGE-TRIGGERED, ONCE PER SESSION - never per burst. An MM Set is written to
 * the QMX's EEPROM, so a burst-by-burst guard would be thousands of EEPROM
 * writes over a night of beaconing. wspr_pa_saved_x10 doubles as the "currently
 * reduced" flag: non-zero means we have turned the radio down and owe it a
 * restore. It is persisted, so a Tab5 that dies mid-session still knows what to
 * put back on the next boot - the radio is left turned DOWN in the meantime,
 * which is the safe direction to fail.
 *
 * The saved value is READ FROM THE RADIO, never assumed to be the 11.5 V
 * factory default: an operator already running a reduced PA must never be
 * turned UP by us. */
#define WSPR_PA_FLOOR_X10  20   /* 2.0 V - below ~1 V the driver's own leakage
                                 * sets the floor and lowering does nothing */

/* ⭐ AN ABSOLUTE TARGET, NOT A FRACTION OF THE OPERATOR'S LIMIT.
 *
 * This halved whatever Max. PA voltage was set to, which is wrong whenever that
 * limit is ABOVE the supply. Measured on the bench: the limit read 15.0 on a
 * 12 V supply, so the PA was really seeing ~12 V and "halving" produced 7.5 V -
 * 62% of the real voltage, not 50%. Set a limit of 20 V and halving it would
 * change nothing at all, while the log cheerfully reported a reduction.
 *
 * 6.0 V is QRP Labs' own figure: the operating manual says setting Max. PA
 * voltage to 6.0 gives roughly 1 W, and names WSPR as a use for it. An absolute
 * target is meaningful regardless of what the operator's limit happens to be. */
#define WSPR_PA_TARGET_X10 60   /* 6.0 V - about 1 W, per the QMX manual */

/* Defined near wspr_rx_stop(); used by the task's own out-of-memory exit too. */
static void wspr_pa_guard_release(void);

static void wspr_pa_guard_update(const qmx_settings_t *ws)
{
    bool want_reduced = ws->wspr_tx_en && ws->wspr_pa_reduce && ws->wspr_duty_pct > 0;

    if (want_reduced && ws->wspr_pa_saved_x10 == 0) {
        int16_t cur = cat_get_pa_voltage_x10();
        if (cur < 0) {
            /* ⛔ "Two minutes late is fine" - what this comment used to say -
             * IS EXACTLY ONE FULL-POWER TRANSMISSION, which is the thing the
             * guard exists to prevent. Roy KI0ER, 2026-09-01, on a 12 V QMX fed
             * from a 9 V supply: "first WSPR TX went out at full power for the
             * first 2 minutes... I checked QMX Menu, and PA voltage was still
             * at 11.5 volts. Subsequent WSPR TX cycles were then at 6.0 Volts."
             *
             * Asking and returning is still right - guessing the operator's
             * value would be worse - but the ARM must now wait for the answer.
             * See wspr_pa_guard_ready(), which the duty-cycle roll consults. */
            cat_query_pa_voltage();
            return;
        }
        uint16_t target = WSPR_PA_TARGET_X10;
        if (target < WSPR_PA_FLOOR_X10) target = WSPR_PA_FLOOR_X10;
        /* NEVER RAISE IT. An operator already running a reduced PA has made a
         * deliberate choice, and a guard that turns someone's power UP is the
         * opposite of a guard. */
        if (target >= (uint16_t)cur) {
            ESP_LOGI(TAG, "PA guard: Max. PA voltage is already %d.%d V, at or below "
                          "the %u.%u V target - leaving it alone",
                     cur / 10, cur % 10, target / 10, target % 10);
            return;
        }
        settings_set_wspr_pa_saved_x10((uint16_t)cur);   /* remember BEFORE writing */
        cat_request_pa_voltage_x10(target);
        ESP_LOGW(TAG, "PA guard: WSPR TX on - Max. PA voltage %d.%d -> %u.%u V "
                      "(about 1 W, per the QMX manual - protects the finals over "
                      "a ~110 s key-down)",
                 cur / 10, cur % 10, target / 10, target % 10);
    } else if (!want_reduced && ws->wspr_pa_saved_x10 != 0) {
        uint16_t back = ws->wspr_pa_saved_x10;
        cat_request_pa_voltage_x10(back);
        settings_set_wspr_pa_saved_x10(0);
        ESP_LOGW(TAG, "PA guard: WSPR TX off - Max. PA voltage restored to %u.%u V",
                 back / 10, back % 10);
    }
}

/* ⛔ MAY A BURST BE ARMED YET?
 *
 * The guard is asynchronous: it asks the radio for Max. PA voltage over CAT,
 * and the answer arrives a poll or two later. Until then it cannot reduce
 * anything - and the duty-cycle roll immediately below it used to fire anyway,
 * so the FIRST burst of a WSPR session went out at whatever the radio was
 * already set to. Every later cycle was correctly reduced, which is exactly why
 * it reads as intermittent and why it survived a release.
 *
 * The test is a MEASUREMENT, not a flag: the radio's own reported Max. PA
 * voltage must actually be at or below the target. That covers all three ways
 * this can be satisfied - the guard reduced it, the operator was already
 * running lower (the guard never raises anyone's power), or the read-back after
 * the write has confirmed it landed. A flag saying "we sent the write" would
 * not, and the write is the part that goes through MM + MU; + a Q9 re-assert.
 *
 * Skipping a transmit cycle costs nothing: the duty cycle is random anyway, so
 * a skipped slot is indistinguishable from an unlucky roll. Transmitting at
 * four times the intended power is not so cheap. */
static bool wspr_pa_guard_ready(const qmx_settings_t *ws)
{
    if (!(ws->wspr_tx_en && ws->wspr_pa_reduce && ws->wspr_duty_pct > 0))
        return true;                    /* protection not wanted - nothing to wait for */
    int16_t cur = cat_get_pa_voltage_x10();
    return cur >= 0 && (uint16_t)cur <= WSPR_PA_TARGET_X10;
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
    /* ⛔ RETRIED, BECAUSE THE PREVIOUS PAGE MAY STILL BE HOLDING THE MEMORY.
     *
     * Swiping FT8 -> WSPR asks for 11.25 MB about three MILLISECONDS after the
     * FT8 view is told to hide - and hiding it does not free anything.
     * ft8_task tests `ui_mode_get() == UI_MODE_FT8` at the top of its SLOT
     * loop, so it can hold its own multi-megabyte pool for up to a full slot,
     * fifteen seconds, after the mode has already changed. The allocation
     * failed, the loop stopped, and the operator saw the waterfall freeze
     * mid-cycle with nothing on screen to say why.
     *
     * ⚠ This path only became reachable today: before the edge strips were
     * raised over the WSPR container there was no way to swipe FT8 -> WSPR at
     * all, so the conflict existed and could not be met.
     *
     * Retrying is the right fix rather than reaching into FT8's teardown -
     * CLAUDE.md puts the double-spawn and worker-join crashes squarely in that
     * lifecycle, which is not somewhere to make a casual change. The memory
     * genuinely does arrive; this just waits for it, and says so. */
    float *cap = NULL;
    bool   pcm_ok = false;
    for (int attempt = 0; attempt < WSPR_ALLOC_TRIES; attempt++) {
        if (!cap) cap = heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(float),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* TWO int16 windows - the ping-pong. Only the int16 side is doubled:
         * the float buffer is fully consumed by the conversion before the next
         * capture opens, so a second would be 5.6 MB bought for nothing. */
        pcm_ok = true;
        for (int i = 0; i < WSPR_PCM_SLOTS; i++) {
            if (!s_pcm[i])
                s_pcm[i] = heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_pcm_busy[i] = false;
            if (!s_pcm[i]) pcm_ok = false;
        }
        if (cap && pcm_ok) break;
        if (!s_run) break;                      /* asked to stop while waiting */
        if (attempt == 0)
            ESP_LOGW(TAG, "capture buffers not available yet (%u KB free) - "
                          "waiting for the previous page to release",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        set_status("waiting for memory");
        vTaskDelay(pdMS_TO_TICKS(WSPR_ALLOC_WAIT_MS));
    }
    if (!cap || !pcm_ok) {
        ESP_LOGE(TAG, "could not allocate the capture buffers (%u KB + %d x %u KB)",
                 (unsigned)(CAP_SAMPLES * sizeof(float) / 1024),
                 WSPR_PCM_SLOTS,
                 (unsigned)(CAP_SAMPLES * sizeof(int16_t) / 1024));
        free(cap);
        for (int i = 0; i < WSPR_PCM_SLOTS; i++) { free(s_pcm[i]); s_pcm[i] = NULL; }
        /* And the decoder's shared 12000 -> 1500 Hz stream, ~1.4 MB of PSRAM
         * that is otherwise held until the next capture happens to land on a
         * different buffer. Freed HERE, with the buffers it was derived from,
         * so leaving the page really does give the memory back. */
        wspr_decode_capture_changed();
        set_status("out of memory");
        /* This exit bypasses wspr_rx_stop() entirely, so the PA would stay
         * reduced for every later mode. See the note there. */
        wspr_pa_guard_release();
        s_run = false; s_task = NULL;
        psram_task_park();
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

    /* ---- WHY THE BUFFER ADDRESSES ARE LOGGED --------------------------
     *
     * A decode costs ~35 % more on some boots than others, and the difference
     * is PER BOOT and constant within it - measured across six boots of four
     * builds, where mix_decimate's mean was 792 ms on one build and 1054-1071
     * on both the build before it and the merge of it. Same code, same fixed
     * amount of work, so it is not a regression and not the candidate load
     * (correlation with how busy a cycle is: 0.14).
     *
     * What is left is where these buffers happened to land. They are ~8.6 MB
     * of PSRAM claimed at page entry, so their addresses depend on the heap's
     * state at that moment, and mix_decimate is dominated by streaming through
     * them. Alignment relative to the cache line is the obvious suspect.
     *
     * ⚠ THAT IS A HYPOTHESIS, NOT A FINDING. It is logged rather than acted on
     * precisely so the next few boots can settle it: if fast boots share an
     * alignment that slow boots do not, it is confirmed; if they do not, the
     * cause is elsewhere and this line costs nothing. */
    ESP_LOGI(TAG, "buffers: cap=%p pcm0=%p pcm1=%p (align %u/%u/%u)",
             (void *)cap, (void *)s_pcm[0], (void *)s_pcm[1],
             (unsigned)((uintptr_t)cap      & 63u),
             (unsigned)((uintptr_t)s_pcm[0] & 63u),
             (unsigned)((uintptr_t)s_pcm[1] & 63u));

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

        /* Turn the PA down before any burst can be armed, and put it back as
         * soon as transmitting is switched off. Edge-triggered inside. */
        wspr_pa_guard_update(&ws);

        char txtext[64];

        /* A burst still running from the previous cycle owns the radio. */
        if (wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
            set_status("transmitting");
            ESP_LOGW(TAG, "cycle %lld: TX still busy - receiver stood down",
                     (long long)cycle_utc);
            /* No carpet mark - see the note at the other stand-down. */
            while (s_run && wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        /* ---- is THIS the cycle the schedule picked? --------------------
         * The roll itself happened earlier (see roll_next_tx_cycle) so that
         * the TX button can count down to a real burst instead of to the next
         * mere opportunity. All that is left here is to act on it, and to roll
         * the following one - whether this cycle transmitted or not, so a held
         * or refused burst moves the countdown on rather than leaving it at
         * zero promising something that is not coming. */
        const bool tx_possible = ws.wspr_tx_en && ws.wspr_duty_pct > 0;
        if (!tx_possible) {
            s_next_tx_cycle = -1;
            s_sched_duty    = 0;
        } else if (s_next_tx_cycle < last_cycle_idx ||
                   s_sched_duty != ws.wspr_duty_pct) {
            /* Nothing scheduled, the schedule was overtaken (a stalled cycle,
             * a clock step), or the operator changed the duty. Rolled from
             * last_cycle_idx - 1 so THIS cycle is the first candidate, which
             * keeps the old behaviour that a burst can happen in the very
             * first cycle after transmitting is switched on. */
            s_next_tx_cycle = roll_next_tx_cycle(last_cycle_idx - 1, ws.wspr_duty_pct);
            s_sched_duty    = ws.wspr_duty_pct;
        }
        const bool tx_this_cycle = tx_possible && s_next_tx_cycle == last_cycle_idx;
        if (tx_this_cycle) {
            /* Roll the next one now, before anything below can fail. */
            s_next_tx_cycle = roll_next_tx_cycle(last_cycle_idx, ws.wspr_duty_pct);
        }

        if (tx_this_cycle && !wspr_pa_guard_ready(&ws)) {
            /* Loud, and only while it is actually holding something up. */
            /* -1 means "not answered yet", and printing that as tenths gave
             * "0.-1 V" on the very first run of this line. Say which it is. */
            int16_t pav = cat_get_pa_voltage_x10();
            char pavs[24];
            if (pav < 0) snprintf(pavs, sizeof(pavs), "not answered yet");
            else         snprintf(pavs, sizeof(pavs), "%d.%d V", pav / 10, pav % 10);
            ESP_LOGW(TAG, "cycle %lld: holding TX - the finals guard has not "
                          "confirmed the PA is turned down yet (radio says %s, "
                          "target %u.%u V)",
                     (long long)cycle_utc, pavs,
                     (unsigned)(WSPR_PA_TARGET_X10 / 10),
                     (unsigned)(WSPR_PA_TARGET_X10 % 10));
        } else if (tx_this_cycle) {
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

        /* Re-read AFTER the arm - see the ordering note above.
         *
         * ⭐ ONLY STAND DOWN IF THE BURST IS IN *THIS* CYCLE (Dirk, 2026-09-01:
         * "the waterfall is already frozen the cycle before the transmit... so
         * the waterfall is off for 2 cycles. Not just for the transmit one,
         * which is unavoidable").
         *
         * ARMED and ACTIVE were treated alike here, so an arm that landed on a
         * slot up to 119 s away stood the receiver down for the ENTIRE waiting
         * cycle as well as the transmitting one. Two dead cycles for one burst,
         * and the second of them was pure waste: nothing was on the air, the
         * radio was free, and the band was simply not being listened to.
         *
         * The grace window in wspr_tx_seconds_until_next_slot() should make a
         * far-off arm rare now, but this is the guard that makes it harmless
         * whenever it still happens - an odd-minute arm, or one more than the
         * grace late. Receive normally and let the NEXT pass through this loop
         * stand down, by which time the burst really is imminent. */
        int secs_to_slot = wspr_tx_seconds_until_next_slot();
        if (wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE &&
            secs_to_slot > WSPR_RX_TX_IMMINENT_S) {
            ESP_LOGI(TAG, "cycle %lld: TX armed but %d s away - receiving this "
                          "cycle instead of standing down",
                     (long long)cycle_utc, secs_to_slot);
        } else if (wspr_tx_get_status(txtext, sizeof(txtext), NULL) != WSPR_TX_IDLE) {
            set_status("transmitting");
            ESP_LOGW(TAG, "cycle %lld: TX cycle - receiver stood down",
                     (long long)cycle_utc);

            /* ⛔ THE CARPET DOES NOT MOVE DURING A TRANSMIT CYCLE (operator,
             * 2026-09-02: "I need it to not move at all - its obvious that
             * nothing can be rx'ed while tx'ing - this is not a duplex
             * machine").
             *
             * ⚠ THIS REVERSES A DELIBERATE DECISION, so here is why it is safe
             * now. The mark was added on 2026-08-31 for Dirk DK7CVD, whose
             * complaint was that the waterfall went dead around a transmission
             * with nothing to say why: two dashed rows made a stalled carpet
             * distinguishable from a hung display.
             *
             * What has changed is that the state is now stated plainly twice
             * elsewhere on the same screen - the TX button reads ON AIR, and the
             * empty spot list says "Transmitting - not receiving this cycle"
             * (Roy KI0ER, this release). And Dirk's real bug was never the
             * missing mark: it was that a burst cost TWO receive cycles, which
             * is fixed. One dark cycle out of two minutes, clearly labelled, is
             * what a half-duplex radio actually does.
             *
             * The mark also cost what it was meant to preserve: two rows of a
             * 200 px pane published per transmit, so the carpet crept upward as
             * if it were receiving something blank.
             *
             * Marks at the end of a RECEIVE cycle stay - those separate real
             * cycles from each other and are not a stall. */

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
            wspr_dec_job_t sjob = { .slot = sslot, .cycle_utc = cycle_utc,
                                    .dial_hz = wspr_rx_cycle_dial_hz() };
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
        /* ⭐ THE DIAL IS READ HERE, WHERE THE SAMPLES START - NOT WHERE THE
         * WINDOW IS QUEUED (Kevin KQ4DTX, 2026-09-03: "when the software
         * decodes a station(s) it labels the band it heard that station on
         * incorrectly, it uses the current listening band").
         *
         * The job already carried the dial rather than letting the decode task
         * read it, precisely so a decode finishing after a hop could not file
         * spots under the band we had moved to. But it was read at QUEUE time,
         * which is after the capture, after the waterfall build and after an
         * optional SD dump - and hop_maybe() retunes in the last HOP_LEAD_SEC
         * (3 s) of the cycle. Anything that pushed the queue past that point
         * recorded the band we were about to listen on instead of the one the
         * samples came from.
         *
         * Reading it at capture-begin removes the window entirely: the value is
         * taken before the first sample and cannot be overtaken by a retune. */
        const uint32_t cap_dial_hz = wspr_rx_cycle_dial_hz();

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
            /* "captur." not "capturing": the line also carries "| dec n/20"
             * and the full word ran past the left panel's right edge. */
            set_status("captur. %d/120 s", got / (int)WSPR_SAMPLE_RATE_HZ);
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

        /* ⛔ NEW AUDIO IN AN OLD BUFFER - INVALIDATE, OR EVERY CYCLE AFTER THE
         * FIRST DECODES THE FIRST ONE'S AUDIO.
         *
         * The decoder caches its shared 12000 -> 1500 Hz stream against the
         * sample buffer's ADDRESS AND LENGTH, and claim_pcm_slot() returns the
         * first FREE slot - so as soon as the decoder keeps up (which it now
         * does: ~33 s of a 120 s cycle), slot 0 is always free and every cycle
         * lands in the SAME buffer. Same pointer, same length, completely
         * different audio: the cache hit every time and the stream was never
         * rebuilt.
         *
         * Observed on air within an hour of shipping the two-stage front end:
         * the first cycle decoded four stations, and every cycle after it
         * decoded ZERO with all twenty candidates failing the sync gate -
         * because they were re-reading cycle one's audio AFTER its own four
         * signals had been subtracted out of it.
         *
         * ⚠ Note the speed-up is what exposed it. While a cycle took most of
         * its 120 s the slots really did alternate and the pointer changed
         * every time, so the address check looked sufficient. It never was. */
        wspr_decode_capture_changed();

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
            wspr_dec_job_t job = { .slot = slot, .cycle_utc = cycle_utc,
                                   .dial_hz = cap_dial_hz };
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
        /* And the decoder's shared 12000 -> 1500 Hz stream, ~1.4 MB of PSRAM
         * that is otherwise held until the next capture happens to land on a
         * different buffer. Freed HERE, with the buffers it was derived from,
         * so leaving the page really does give the memory back. */
        wspr_decode_capture_changed();
    }
    if (s_dec_q) { vQueueDelete(s_dec_q); s_dec_q = NULL; }
    s_dec_task = NULL;
    set_status("idle");
    set_dec_status("%s", "");
    ESP_LOGI(TAG, "slot loop stopped");
    s_task = NULL;
    psram_task_park();
}

/* The single answer to "is WSPR reachable" - see wspr_rx.h for why every gate
 * asks this rather than reading the setting for itself. */
bool wspr_feature_enabled(void)
{
    qmx_settings_t c;
    settings_load_all(&c);
    return c.wspr_en;
}

bool wspr_rx_start(void)
{
    if (s_run) return true;

    /* Last line of defence, not the only one. The UI and the web handler both
     * refuse before getting here; this exists so a future caller cannot start
     * an 8.6 MB decode loop for a page the operator has never enabled. */
    if (!wspr_feature_enabled()) {
        ESP_LOGW(TAG, "RX start refused - the WSPR page is not enabled "
                      "(/api/cmd {\"action\":\"wspr_enable\",\"on\":true})");
        return false;
    }

    /* ⛔ TRANSMIT ALWAYS STARTS OFF (Roy KI0ER, 2026-09-01; operator's call:
     * "It is not okay to have it transmitting automatically if the user were
     * not aware how it was left last time").
     *
     * wspr_tx_en persists, so entering WSPR - including the page being restored
     * at boot - used to resume beaconing on its own. That is a transmitter
     * keying up because of a decision made in a previous session, possibly days
     * ago, and possibly with a different antenna or supply connected. Roy hit
     * exactly that: he came back to WSPR and found TX already enabled from
     * before, on a 12 V radio running from a 9 V supply.
     *
     * ⚠ This narrows v1.10.0's "the Tab5 wakes on the page it was left on", and
     * only that far: the PAGE still comes back, so a station set up for WSPR
     * finds itself on WSPR after a reboot. It just does not transmit until
     * somebody says so. Those two were only ever bundled together by the fact
     * that one setting carried both.
     *
     * Written through settings_set_wspr_tx_en() rather than poked, so the
     * drawer, the web page and the config export all see it. */
    {
        qmx_settings_t st;
        settings_load_all(&st);
        if (st.wspr_tx_en) {
            settings_set_wspr_tx_en(false);
            ESP_LOGW(TAG, "entering WSPR: transmit was left ON from a previous "
                          "session - defaulting it OFF; enable it deliberately");
        }
    }

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
    /* Reap the pair from the PREVIOUS WSPR visit before creating another.
     * They parked on their way out and are still holding 32 KB of PSRAM each;
     * without this every Panadapter/WSPR round trip lost a clean 64 KB (#279),
     * which is what the measurement showed. Here is the safe place: we are on
     * the LVGL task and last time's tasks are provably parked. */
    int reaped = psram_task_reap();
    if (reaped) ESP_LOGI(TAG, "reaped %d parked task(s) from the last visit", reaped);

    s_dec_task = psram_task_create_reapable(wspr_dec_task, "wspr_dec", 32768, NULL,
                                   tskIDLE_PRIORITY + 1, tskNO_AFFINITY);
    if (!s_dec_task) {
        ESP_LOGE(TAG, "could not create the decode task");
        vQueueDelete(s_dec_q); s_dec_q = NULL;
        s_run = false;
        ui_mode_set(UI_MODE_PANADAPTER);
        return false;
    }

    s_task = psram_task_create_reapable(wspr_rx_task, "wspr_rx", 32768, NULL,
                               tskIDLE_PRIORITY + 1, tskNO_AFFINITY);
    if (!s_task) {
        ESP_LOGE(TAG, "could not create the slot-loop task");
        s_run = false;   /* stands the decode task down too */
        ui_mode_set(UI_MODE_PANADAPTER);
        return false;
    }
    return true;
}

/* Give the radio its power back, whatever the reason we are leaving.
 *
 * ⛔ WITHOUT THIS THE GUARD IS WORSE THAN NOT HAVING IT. Max. PA voltage is a
 * GLOBAL radio setting, not a per-mode one, and wspr_rx_stop() ends the slot
 * loop - so the periodic guard in the loop never runs again. Enable WSPR TX,
 * swipe to FT8, and every FT8 burst goes out at a quarter power, silently,
 * with the radio's STORED configuration left changed. The operator would have
 * no way to see it except on the radio's own Protection menu.
 *
 * Called from wspr_rx_stop() rather than only from the loop precisely because
 * leaving the page is the case the loop cannot cover. */
static void wspr_pa_guard_release(void)
{
    wspr_pa_guard_release_pending("leaving WSPR");
}

/* ⭐ THE RESTORE HAS TO BE REACHABLE FROM OUTSIDE WSPR, because the case that
 * needs it is the one where WSPR never gets to run again.
 *
 * Roy KI0ER, 2026-09-02: WSPR ran overnight, his supply switched off on a timer
 * mid-session, and in the morning he went straight to FT8 and found the radio
 * still capped at 6.0 V. Leaving the WSPR page restores correctly and always
 * has - but a power cut is not leaving the page. wspr_rx_stop() never ran, the
 * value to restore sat in NVS, and nothing on the FT8 or panadapter path ever
 * looks at it. Every subsequent transmission on any mode was at about 1 W.
 *
 * ⚠ He guessed the cause differently - that the radio had reported 6.0 V as its
 * own full-power setting and the guard had adopted it. That would be the
 * "a guard inherits whatever the last session left" hazard already recorded in
 * CLAUDE.md, and it is real, but it is not this: the capture only runs when the
 * stored value is 0, and his was 11.5. The stored value was right the whole
 * time; nothing was asking for it.
 *
 * So this is called at CAT link-up too. Idempotent - it returns at once when
 * there is nothing outstanding, which is the normal case on almost every boot.
 *
 * ⚠ Uses the narrow accessor, never settings_load_all(): one caller is the CAT
 * link task on a 5 KB stack, and the settings struct is multi-kilobyte. That is
 * the bug class that has boot-looped this board four times. */
void wspr_pa_guard_release_pending(const char *why)
{
    uint16_t back = settings_get_wspr_pa_saved_x10();
    if (back == 0) return;                       /* nothing outstanding */
    cat_request_pa_voltage_x10(back);
    settings_set_wspr_pa_saved_x10(0);
    ESP_LOGW(TAG, "PA guard: %s - Max. PA voltage restored to %u.%u V",
             why ? why : "restoring", back / 10, back % 10);
}

/* ⭐ RESTORE ONLY WHAT WE OURSELVES REDUCED - which is also the answer to the
 * two-radio problem, and a better one than remembering a value per radio.
 *
 * Michael KZ4LY, 2026-09-02: "I have built both 9V QMX, 12V QMX+ so far... if
 * the tab5 keeps track of it in configuration, it should be per unit ID, not
 * generic across all devices."
 *
 * He is right, and the hazard is in THIS code, not hypothetical: the outstanding
 * value is a single NVS number with no radio attached to it. Reduce a 12 V unit
 * to 6.0, unplug it, plug in the 9 V unit, and the link-up restore would push
 * the first radio's 11.5 V onto the second - raising a cap the operator never
 * set, on a radio that never asked. That is exactly what the capture path
 * already refuses to do ("a guard that turns someone's power UP is the opposite
 * of a guard"), and the restore path was not holding to it.
 *
 * ⚠ A per-unit table is not available and that is measured, not assumed: the
 * QMX answers no CAT command with a serial, and its USB descriptor reports
 * iSerialNumber 0 - checked in the enumeration log rather than guessed. Both
 * doors are shut.
 *
 * But identity was never the question worth asking. The question is "is this
 * radio still sitting at the value we put there?" - and the radio can answer
 * that itself. If it reports the WSPR target, this is our own reduction and
 * restoring is right. If it reports anything else, it is either a different
 * radio or one the operator has since set by hand, and in both cases the stored
 * value is not ours to write. The value is kept rather than dropped, so the
 * original radio still gets it back when it returns.
 *
 * Blocks briefly for the answer. Called once per link-up from the poll task,
 * which is the first place a query can actually go out. */
void wspr_pa_guard_reclaim_on_link(void)
{
    uint16_t back = settings_get_wspr_pa_saved_x10();
    if (back == 0) return;                       /* the normal case, every boot */

    cat_query_pa_voltage();
    int16_t cur = -1;
    for (int i = 0; i < 25 && cur < 0; i++) {    /* up to ~500 ms */
        vTaskDelay(pdMS_TO_TICKS(20));
        cur = cat_get_pa_voltage_x10();
    }
    if (cur < 0) {
        ESP_LOGW(TAG, "PA guard: %u.%u V still owed, but the radio has not "
                      "reported its Max. PA voltage - leaving it for now",
                 back / 10, back % 10);
        return;
    }
    if ((uint16_t)cur != WSPR_PA_TARGET_X10) {
        ESP_LOGW(TAG, "PA guard: %u.%u V is owed, but this radio is at %d.%d V, "
                      "not the %u.%u V we reduce to - so it is not our doing "
                      "(a different radio, or you set it yourself). Leaving it "
                      "alone and keeping the value for the radio it belongs to",
                 back / 10, back % 10, cur / 10, cur % 10,
                 WSPR_PA_TARGET_X10 / 10, WSPR_PA_TARGET_X10 % 10);
        return;
    }
    cat_request_pa_voltage_x10(back);
    settings_set_wspr_pa_saved_x10(0);
    ESP_LOGW(TAG, "PA guard: radio reconnected still reduced - Max. PA voltage "
                  "restored to %u.%u V", back / 10, back % 10);
}

void wspr_rx_stop(void)
{
    /* ⛔ STOP THE RADIO TRANSMITTING BEFORE GIVING IT ITS POWER BACK, AND
     * RELEASE EVEN IF THE LOOP HAS ALREADY GONE (Roy KI0ER, 2026-09-01: "if
     * mid TX in WSPR mode, and the user changes to FT8 or to the Panadapter
     * waterfall, TX should immediately halt, and Max. PA should be set back").
     *
     * Two faults, and the ORDER is the dangerous one. Leaving WSPR mid-burst
     * used to restore the PA voltage while the burst was still keyed - so the
     * finals' supply went from 6.0 V back to 11.5 V IN THE MIDDLE OF A ~110 s
     * KEY-DOWN, which is precisely the stress the guard exists to prevent. It
     * had to abort first and restore second.
     *
     * The second is why Roy saw 6.0 V persist into CW and FT8 at all: the
     * release sat behind `if (!s_run) return;`, so any path that had already
     * cleared s_run - the task's own out-of-memory exit, or a second call -
     * skipped it silently and left the radio's STORED configuration reduced,
     * visible only on the QMX's own Protection menu.
     *
     * wspr_pa_guard_release() is idempotent (it returns immediately when
     * nothing is outstanding), so running it unconditionally is safe. */
    char t[64];
    wspr_tx_state_t tst = wspr_tx_get_status(t, sizeof(t), NULL);
    if (tst == WSPR_TX_ACTIVE) {
        ESP_LOGW(TAG, "leaving WSPR while ON AIR - aborting the burst before "
                      "restoring the PA voltage");
        wspr_tx_request_abort();
    } else if (tst == WSPR_TX_ARMED) {
        wspr_tx_disarm();
    }
    /* Wait for the radio to actually stop keying. run_burst() always runs its
     * TA0;/RX; tail, so this is bounded by the settle delay, not by the burst -
     * but it is bounded explicitly regardless, because blocking the UI thread
     * for ever on a stuck TX task would be worse than a raised PA voltage. */
    for (int i = 0; i < 40 && wspr_tx_get_status(t, sizeof(t), NULL) != WSPR_TX_IDLE; i++)
        vTaskDelay(pdMS_TO_TICKS(50));

    s_run = false;
    wspr_pa_guard_release();
    /* The task frees its own buffers and clears s_task; the mode goes back here
     * so the panadapter starts drawing again immediately rather than waiting for
     * a capture in flight to finish.
     *
     * ⛔ ONLY IF NOBODY HAS ALREADY CHOSEN THE NEXT MODE. Unconditionally, this
     * was a restore that clobbered a newer value: ui_set_base_mode() sets the
     * destination FIRST and stands the old page down SECOND, so a WSPR -> FT8
     * swipe set FT8 and then this line put it back to PANADAPTER. ft8_task's
     * `while (ui_mode_get() == UI_MODE_FT8)` therefore exited on its first
     * test - "ft8_task exiting; processed 0 slots", ~30 ms after every start.
     *
     * The respawn watchdog then made that permanent rather than transient,
     * restarting it once a second for ever; each cycle leaks three TCBs and
     * their stacks, because a WithCaps task that deletes ITSELF leaks by IDF's
     * own admission (see vTaskDeleteWithCaps in idf_additions.c). Internal
     * heap went 38 KB -> 4 KB with a 0 KB largest block, which on this board
     * is also the state that breaks TLS, USB endpoint allocation and SD.
     *
     * Measured 2026-08-27. The whole failure looked like a memory bug and was
     * a mode bug; the heap was the symptom, not the cause. */
    if (ui_mode_get() == UI_MODE_WSPR) ui_mode_set(UI_MODE_PANADAPTER);
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
