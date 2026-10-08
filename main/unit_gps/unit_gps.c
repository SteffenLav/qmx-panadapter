#include "unit_gps.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "time_sync.h"    // time_sync_notify_unit_gps() - the ONE crossing call
#include "util/psram_task.h"

static const char *TAG = "unit_gps";

/* UART1: the console is UART0 and nothing else in main/ opens a uart_driver,
 * so this is the only claim on it.
 *
 * RX = GPIO54 with a pull-up. The port is a flying lead: an unplugged RX that
 * floats is read as random transitions, which would paint DEVICE forever and
 * make "no module attached" indistinguishable from "module present, no fix".
 *
 * TX is not connected to a pin (UART_PIN_NO_CHANGE). This is deliberate.
 * An idle UART TX line stays HIGH (mark). The default relay pin is 53,
 * so the relay harness reads the level of GPIO53. An active-high relay
 * treats HIGH as its active level. A HIGH level on GPIO53 energizes the
 * harness. The harness then holds the radio in power-cycle. The
 * developer's words: "leaving the relay harness plugged in while in GPS
 * mode would hold the radio in power-cycle." The code must not route
 * TX, whatever the stored polarity says. Version 1 never transmits, so
 * nothing is lost by the unrouted TX. The sequencer has released GPIO53
 * to hi-Z, and the TX signal does not drive it (qmx-panadapter
 * developer review, 2026-10-04). */
#define UNIT_GPS_UART_NUM    UART_NUM_1
#define UNIT_GPS_RX_GPIO     GPIO_NUM_54   /* PORT.A default - see unit_gps_start_on() */
static int s_rx_gpio = UNIT_GPS_RX_GPIO;    /* whichever pin this run is bound to */
#define UNIT_GPS_BAUD        115200
static int s_baud = UNIT_GPS_BAUD;          /* whichever rate this run is using */

/* ⭐ TEMPORARY INSTRUMENT, 2026-10-08. The Module GPS v2.1 on the M-Bus went
 * to LISTENING and stayed there. LISTENING means "running, no WELL-FORMED
 * NMEA", so it cannot tell a dead pin from bytes arriving at the wrong baud -
 * the DIP-switch-to-GPIO mapping is derived rather than documented, and this
 * driver's 115200 comes from the Unit GPS v1.1 while the ATGM336H-6N on the
 * v2.1 defaults to 9600. Both candidates read identically through the state
 * machine. This counts raw bytes and prints a sample, which separates them.
 * Remove it once the receiver is known good. */
static volatile uint32_t s_rx_bytes;        /* raw bytes since this start */
static volatile uint32_t s_rx_lines;        /* newline-terminated lines seen */

#define UNIT_GPS_TASK_STACK  4096
#define UNIT_GPS_TASK_PRIO   5
/* Core 1. Not tskNO_AFFINITY, and not core 0. Core 0 of this board is
 * busy: taskLVGL uses about 74% of it, plus audio_task and the USB
 * paths. Background UART polling belongs on core 1 with the other
 * non-UI work. */
#define UNIT_GPS_TASK_CORE   1
#define UNIT_GPS_READ_MS     100   // wake at 10 Hz: cheap, and it bounds state-change latency
#define UNIT_GPS_LINE_MAX    128

/* Shared with the UI task and /api/status, so the timestamps the state machine
 * reads are 32-bit milliseconds: a 32-bit write is atomic here, where a 64-bit
 * one is not, and a torn read would show a chip flickering between states for
 * one frame. Ages are differences of two values on the same clock, so the
 * 49-day wrap of uint32_t ms is harmless. */
static volatile bool     s_running     = false;
static volatile bool     s_stop_req    = false;
static volatile bool     s_have_fix    = false;
static volatile bool     s_have_sentence = false;
static volatile uint32_t s_last_fix_ms     = 0;
static volatile uint32_t s_last_sentence_ms = 0;

static TaskHandle_t       s_task = NULL;
static int                s_prev_sec = -1;          // flip detect, per session
static unit_gps_state_t   s_logged_state = UNIT_GPS_OFF;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool unit_gps_running(void) { return s_running; }

unit_gps_state_t unit_gps_state(void)
{
    if (!s_running) return UNIT_GPS_OFF;

    uint32_t now = now_ms();
    if (s_have_fix) {
        return ((now - s_last_fix_ms) >= UNIT_GPS_FRESH_MS) ? UNIT_GPS_LOST
                                                            : UNIT_GPS_LOCKED;
    }
    if (s_have_sentence && (now - s_last_sentence_ms) < UNIT_GPS_FRESH_MS) {
        return UNIT_GPS_DEVICE;
    }
    return UNIT_GPS_LISTENING;
}

uint32_t unit_gps_age_ms(void)
{
    if (!s_have_fix) return UINT32_MAX;
    return now_ms() - s_last_fix_ms;
}

bool unit_gps_is_live(void)
{
    return unit_gps_state() == UNIT_GPS_LOCKED && unit_gps_age_ms() < UNIT_GPS_FRESH_MS;
}

const char *unit_gps_state_name(unit_gps_state_t state)
{
    switch (state) {
    case UNIT_GPS_OFF:       return "OFF";
    case UNIT_GPS_LISTENING: return "LISTENING";
    case UNIT_GPS_DEVICE:    return "DEVICE";
    case UNIT_GPS_LOCKED:    return "LOCKED";
    case UNIT_GPS_LOST:      return "LOST";
    }
    return "OFF";
}

/* One well-formed RMC sentence, in arrival order.
 *
 * The flip stamp is taken when the LINE COMPLETED (the sentence end), not when
 * it was parsed: the second boundary the sentence reports on has already passed
 * by then, and the whole point of flip_us is to say how long ago. A sentence
 * that carries the N->N+1 edge passes its stamp on; every other sentence
 * passes 0, which means "no edge with this one" and yields a whole-second
 * apply rather than a phase claim it cannot support. */
static void on_rmc_line(const char *line, int64_t arrival_us)
{
    nmea_rmc_t r;
    if (!nmea_parse_rmc(line, &r)) return;    // not RMC / bad checksum / insane field

    uint32_t arr_ms = (uint32_t)(arrival_us / 1000);
    s_last_sentence_ms = arr_ms;
    s_have_sentence    = true;

    bool flipped = (s_prev_sec >= 0) && nmea_second_flipped(s_prev_sec, r.sec);
    s_prev_sec   = r.sec;

    /* Snapshot fields for the GPS page. Taken from EVERY well-formed RMC,
     * including a void one: a receiver with no lock still reports a time, and
     * a page that showed nothing until lock could not tell "searching" from
     * "not connected" - which is the whole reason the page exists. */
    info_lock();
    s_rmc_valid = r.valid;
    s_has_time  = true;
    s_year = r.year; s_mon = r.mon;  s_mday = r.mday;
    s_hour = r.hour; s_min = r.min;  s_sec  = r.sec;
    if (r.has_pos) {
        s_has_pos = true;
        s_lat_deg = r.lat_deg;
        s_lon_deg = r.lon_deg;
    }
    info_unlock();

    if (!r.valid) return;    // 'V': the receiver is talking but has no lock

    s_last_fix_ms = arr_ms;
    s_have_fix    = true;

    time_sync_notify_unit_gps(r.year, r.mon, r.mday,
                              r.hour, r.min, r.sec,
                              r.frac_us, flipped ? arrival_us : 0);
}


/* ---- Status snapshot (GGA / GSA / GSV) ---------------------------------
 *
 * Mirrors what the QMX's own GPS viewer shows, so the two pages can be the
 * same page with a different source. The CLOCK still reads RMC and only RMC:
 * a GGA carries a time of day with no date, and letting that near time_sync
 * is how a receiver silently sets the wrong day.
 *
 * GSV arrives in BURSTS, one per constellation, each "msg_num of msg_total".
 * The satellites of a talker are therefore replaced wholesale when that
 * talker's msg 1 arrives, not accumulated - appending every sentence would
 * grow the list without bound and show each satellite once per burst.
 *
 * in_view is likewise PER TALKER, so the total is the sum of the LATEST value
 * from each, held in s_in_view[] rather than added as sentences arrive.
 */
#define GPS_MAX_TALKERS 6

static unit_gps_sat_t s_sat[UNIT_GPS_MAX_SATS];
static int            s_n_sat;
static struct { char talker[3]; int in_view; } s_in_view[GPS_MAX_TALKERS];
static int            s_n_talkers;

static int   s_fix_type;      /* GSA: 1 none, 2 = 2D, 3 = 3D; 0 = not reported */
static uint8_t s_used_prn[12]; /* PRNs GSA says are IN the solution */
static int     s_n_used;
static float s_hdop = -1.0f;
static int   s_fix_sats;      /* GGA */
static float s_alt_m;
static bool  s_has_alt;

static int      s_year, s_mon, s_mday, s_hour, s_min, s_sec;
static bool     s_has_time;
static bool     s_has_pos;
static double   s_lat_deg, s_lon_deg;
static bool     s_rmc_valid;

/* The snapshot is written by the receive task and read by the UI and the web
 * handler. A mutex rather than a critical section: the write is a few hundred
 * bytes and the readers are not ISRs. */
static SemaphoreHandle_t s_info_lock;

static void info_lock(void)
{
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
}
static void info_unlock(void)
{
    if (s_info_lock) xSemaphoreGive(s_info_lock);
}


/* Drop every satellite belonging to `talker`. Called when that talker's msg 1
 * arrives, which is the start of a fresh picture of that constellation. */
static void sats_clear_talker(const char *talker)
{
    int w = 0;
    for (int r = 0; r < s_n_sat; r++) {
        if (s_sat[r].talker[0] == talker[0] && s_sat[r].talker[1] == talker[1]) continue;
        if (w != r) s_sat[w] = s_sat[r];
        w++;
    }
    s_n_sat = w;
}

static void note_in_view(const char *talker, int n)
{
    for (int i = 0; i < s_n_talkers; i++) {
        if (s_in_view[i].talker[0] == talker[0] && s_in_view[i].talker[1] == talker[1]) {
            s_in_view[i].in_view = n;
            return;
        }
    }
    if (s_n_talkers < GPS_MAX_TALKERS) {
        s_in_view[s_n_talkers].talker[0] = talker[0];
        s_in_view[s_n_talkers].talker[1] = talker[1];
        s_in_view[s_n_talkers].talker[2] = '\0';
        s_in_view[s_n_talkers].in_view   = n;
        s_n_talkers++;
    }
}

/* Any sentence that is not RMC. Each parser rejects the others' lines, so this
 * is three cheap tries rather than a dispatch on the sentence name. */
static void on_other_line(const char *line)
{
    nmea_gga_t g;
    nmea_gsa_t a;
    nmea_gsv_t v;

    if (nmea_parse_gga(line, &g)) {
        info_lock();
        s_fix_sats = g.sats_used;
        if (g.hdop >= 0.0f) s_hdop = g.hdop;
        if (g.has_alt) { s_alt_m = g.alt_m; s_has_alt = true; }
        info_unlock();
        return;
    }
    if (nmea_parse_gsa(line, &a)) {
        info_lock();
        s_fix_type = a.fix_type;
        if (a.hdop >= 0.0f) s_hdop = a.hdop;
        /* A multi-constellation receiver sends one GSA PER CONSTELLATION, so
         * the lists must be MERGED, not replaced - replacing leaves only the
         * last constellation marked as used. They are cleared when the fix is
         * lost (fix_type 1), which is the only moment the whole set is stale. */
        if (a.fix_type <= 1) s_n_used = 0;
        for (int i = 0; i < a.n_used; i++) {
            bool seen = false;
            for (int j = 0; j < s_n_used; j++) {
                if (s_used_prn[j] == (uint8_t)a.used[i]) { seen = true; break; }
            }
            if (!seen && s_n_used < (int)(sizeof(s_used_prn))) {
                s_used_prn[s_n_used++] = (uint8_t)a.used[i];
            }
        }
        info_unlock();
        return;
    }
    if (nmea_parse_gsv(line, &v)) {
        info_lock();
        if (v.msg_num == 1) sats_clear_talker(v.talker);
        note_in_view(v.talker, v.in_view);
        for (int i = 0; i < v.n_sats && s_n_sat < UNIT_GPS_MAX_SATS; i++) {
            unit_gps_sat_t *d = &s_sat[s_n_sat++];
            d->prn       = (uint8_t)v.sat[i].prn;
            d->elev_deg  = (int8_t)v.sat[i].elev_deg;
            d->azim_deg  = (int16_t)v.sat[i].azim_deg;
            d->snr_db    = (int8_t)v.sat[i].snr_db;
            d->talker[0] = v.talker[0];
            d->talker[1] = v.talker[1];
            d->talker[2] = '\0';
        }
        info_unlock();
    }
}

void unit_gps_get_info(unit_gps_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->state  = unit_gps_state();
    out->age_ms = unit_gps_age_ms();

    info_lock();
    out->valid    = s_rmc_valid;
    out->fix_type = s_fix_type;
    out->has_time = s_has_time;
    out->year = s_year; out->mon = s_mon; out->mday = s_mday;
    out->hour = s_hour; out->min = s_min; out->sec = s_sec;
    out->has_pos  = s_has_pos;
    out->lat_deg  = s_lat_deg;
    out->lon_deg  = s_lon_deg;
    out->has_alt  = s_has_alt;
    out->alt_m    = s_alt_m;
    out->fix_sats = s_fix_sats;
    out->hdop     = s_hdop;

    int tot = 0;
    for (int i = 0; i < s_n_talkers; i++) tot += s_in_view[i].in_view;
    out->tot_sats = tot;

    int sum = 0, n = 0;
    for (int i = 0; i < s_n_sat && i < UNIT_GPS_MAX_SATS; i++) {
        out->sat[i] = s_sat[i];
        /* The average is over TRACKED satellites only. Folding the in-view-but-
         * untracked ones in as zero drags it down and hides the difference
         * between a weak sky and a half-tracked one. */
        if (s_sat[i].snr_db > 0) { sum += s_sat[i].snr_db; n++; }
    }
    out->n_sats  = s_n_sat;
    out->avg_snr = n ? (sum + n / 2) / n : -1;
    info_unlock();
}

static void unit_gps_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    char    line[UNIT_GPS_LINE_MAX];
    size_t  line_len = 0;
    uint8_t sample[16];
    int     sample_len = 0;
    uint32_t last_report_ms = 0;
    uint32_t last_bytes = 0;

    while (!s_stop_req) {
        int n = uart_read_bytes(UNIT_GPS_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(UNIT_GPS_READ_MS));
        if (n > 0) {
            s_rx_bytes += (uint32_t)n;
            /* TEMPORARY INSTRUMENT: keep the first slice of the first read, so
             * the log can show what is on the wire even when nothing parses. */
            if (!sample_len) {
                sample_len = (n < (int)sizeof(sample)) ? n : (int)sizeof(sample);
                memcpy(sample, buf, (size_t)sample_len);
            }
            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    s_rx_lines++;
                    if (line_len) {
                        line[line_len] = '\0';
                        on_rmc_line(line, esp_timer_get_time());
                        on_other_line(line);
                    }
                    line_len = 0;
                } else if (line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    line_len = 0;   // over-long: not an NMEA sentence, drop it
                }
            }
        }

        /* TEMPORARY INSTRUMENT: every 2 s while nothing has parsed yet, say
         * how many raw bytes arrived and show the first ones. Three outcomes,
         * three different causes:
         *   0 bytes            - nothing on the pin: wrong GPIO, or no power.
         *   bytes, 0 lines     - wrong baud: framing noise, no newlines.
         *   bytes and lines    - right pin and baud, parser or checksum issue.
         * It stops reporting once a sentence has been accepted. */
        if (!s_have_sentence) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - last_report_ms >= 2000u) {
                last_report_ms = now_ms;
                char hex[16 * 3 + 1];
                char asc[16 + 1];
                int  hp = 0;
                for (int i = 0; i < sample_len; i++) {
                    hp += snprintf(hex + hp, sizeof(hex) - (size_t)hp, "%02X ", sample[i]);
                    asc[i] = (sample[i] >= 0x20 && sample[i] < 0x7F) ? (char)sample[i] : '.';
                }
                hex[hp > 0 ? hp - 1 : 0] = '\0';
                asc[sample_len] = '\0';
                ESP_LOGW(TAG, "no NMEA yet on GPIO%d @ %d: %u bytes total "
                              "(+%u in 2 s), %u lines, first bytes [%s] \"%s\"",
                         s_rx_gpio, s_baud, (unsigned)s_rx_bytes,
                         (unsigned)(s_rx_bytes - last_bytes), (unsigned)s_rx_lines,
                         hex, asc);
                last_bytes = s_rx_bytes;
            }
        }

        /* State transitions are time-based, so they are noticed here (10 Hz)
         * and not only when the next sentence happens to arrive - which is
         * exactly the cable-pull case, where no further data will ever come. */
        unit_gps_state_t st = unit_gps_state();
        if (st != s_logged_state) {
            if (st == UNIT_GPS_LOCKED) {
                ESP_LOGI(TAG, "pipeline %s -> LOCKED", unit_gps_state_name(s_logged_state));
            } else if (st == UNIT_GPS_LOST) {
                ESP_LOGW(TAG, "pipeline LOCKED -> LOST (no fix for %u ms)", UNIT_GPS_FRESH_MS);
            } else {
                ESP_LOGI(TAG, "pipeline %s -> %s",
                         unit_gps_state_name(s_logged_state), unit_gps_state_name(st));
            }
            s_logged_state = st;
        }
    }

    s_task = NULL;
    /* Never call vTaskDelete() for this task. It is created with
     * psram_task_create_reapable(), so its stack is ours to free and
     * FreeRTOS will not free it (#279). psram_task_park() stops the
     * task and keeps it to the side. The next unit_gps_start() deletes
     * the task and frees its stack (psram_task_reap()). */
    psram_task_park();
}

bool unit_gps_start(void)
{
    return unit_gps_start_on(UNIT_GPS_RX_GPIO);
}

bool unit_gps_start_on(int rx_gpio)
{
    return unit_gps_start_on_baud(rx_gpio, UNIT_GPS_BAUD);
}

bool unit_gps_start_on_baud(int rx_gpio, int baud)
{
    if (s_running) {
        /* Idempotent on the SAME pin, refused on a different one: there is one
         * UART and one parser, and silently rebinding would leave the caller
         * believing it had a second receiver. */
        if (rx_gpio == s_rx_gpio) return true;
        ESP_LOGE(TAG, "GNSS already running on GPIO%d - refusing to also start "
                      "on GPIO%d (one receiver at a time)", s_rx_gpio, rx_gpio);
        return false;
    }
    s_rx_gpio = rx_gpio;
    s_baud    = (baud > 0) ? baud : UNIT_GPS_BAUD;
    s_rx_bytes = 0;
    s_rx_lines = 0;

    /* Order matters: params and pins first, driver last. uart_driver_install()
     * refuses to run twice, so clear any driver a previous, partially-failed
     * start may have left - the error on a first boot is expected and ignored. */
    uart_driver_delete(UNIT_GPS_UART_NUM);

    uart_config_t cfg = {
        .baud_rate  = s_baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_param_config(UNIT_GPS_UART_NUM, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: 0x%x", e);
        return false;
    }
    /* TX = UART_PIN_NO_CHANGE: the TX signal does not go to a pin (see
     * the comment on UNIT_GPS_UART_NUM). An idle TX line is HIGH. A
     * relay harness left plugged in while in GPS mode would hold the
     * radio in power-cycle. GPIO53 stays at hi-Z after the sequencer
     * releases it. */
    e = uart_set_pin(UNIT_GPS_UART_NUM, UART_PIN_NO_CHANGE, s_rx_gpio,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: 0x%x", e);
        return false;
    }
    e = uart_driver_install(UNIT_GPS_UART_NUM, 1024, 0, 0, NULL, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: 0x%x", e);
        return false;
    }
    /* RX pull-up, after the pin is routed to the UART: without it a disconnected
     * lead floats and the line looks like traffic. */
    gpio_set_pull_mode((gpio_num_t)s_rx_gpio, GPIO_PULLUP_ONLY);

    if (!s_info_lock) s_info_lock = xSemaphoreCreateMutex();
    info_lock();
    s_n_sat = 0;
    s_n_talkers = 0;
    s_fix_type = 0;
    s_n_used = 0;
    s_hdop = -1.0f;
    s_fix_sats = 0;
    s_has_alt = false;
    s_has_time = false;
    s_has_pos = false;
    s_rmc_valid = false;
    info_unlock();

    s_stop_req = false;
    s_prev_sec = -1;
    s_logged_state = UNIT_GPS_OFF;
    s_running = true;

    /* Reap the task that parked at the last stop, before this start.
     * The task ends at every unit_gps -> relay change. Only this path
     * returns its PSRAM stack (#279). wspr_rx works the same way. */
    psram_task_reap();

    /* The stack is in PSRAM, because internal RAM is the scarce
     * resource on this board. The task is pinned to core 1
     * (UNIT_GPS_TASK_CORE). It is created "reapable" because this task
     * ends; see the psram_task_park() note in unit_gps_task(). */
    s_task = psram_task_create_reapable(unit_gps_task, "unit_gps", UNIT_GPS_TASK_STACK,
                                        NULL, UNIT_GPS_TASK_PRIO, UNIT_GPS_TASK_CORE);
    if (!s_task) {
        ESP_LOGE(TAG, "receive task could not be created");
        s_running = false;
        uart_driver_delete(UNIT_GPS_UART_NUM);
        return false;
    }

    /* Print the pin this run actually bound, not the PORT.A default. The old
     * line said GPIO54 unconditionally and read as a measurement while being
     * a constant - it claimed 54 during the whole M-Bus test on GPIO2. */
    ESP_LOGI(TAG, "UART1 up: RX=GPIO%d (pull-up) TX unrouted, %d 8N1 - pipeline LISTENING",
             s_rx_gpio, s_baud);
    return true;
}

void unit_gps_stop(void)
{
    if (!s_running) return;    // idempotent

    s_stop_req = true;
    /* The task reads with a 100 ms timeout and checks the flag on each
     * pass, so it clears s_task and calls psram_task_park() within one
     * pass. The wait is for that and not for more data. The next
     * unit_gps_start() frees the stack of the parked task. */
    for (int i = 0; i < 200 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "receive task had not exited after 2 s - releasing the UART anyway");
    }
    uart_driver_delete(UNIT_GPS_UART_NUM);

    s_running       = false;
    s_stop_req      = false;
    s_have_fix      = false;
    s_have_sentence = false;
    s_prev_sec      = -1;
    s_logged_state  = UNIT_GPS_OFF;
    ESP_LOGI(TAG, "stopped - pipeline OFF, UART released");
}

/* Instrument readouts - see the TEMPORARY INSTRUMENT note at s_rx_bytes. */
int unit_gps_rx_gpio(void)       { return s_rx_gpio; }
int unit_gps_baud(void)          { return s_baud; }
uint32_t unit_gps_rx_bytes(void) { return s_rx_bytes; }
uint32_t unit_gps_rx_lines(void) { return s_rx_lines; }
