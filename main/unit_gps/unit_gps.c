#include "unit_gps.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "time_sync.h"    // time_sync_notify_unit_gps() - the ONE crossing call

static const char *TAG = "unit_gps";

/* UART1: the console is UART0 and nothing else in main/ opens a uart_driver,
 * so this is the only claim on it.
 *
 * RX = GPIO54 with a pull-up. The port is a flying lead: an unplugged RX that
 * floats is read as random transitions, which would paint DEVICE forever and
 * make "no module attached" indistinguishable from "module present, no fix".
 * TX = GPIO53, left idle (mark) - v1 sends no configuration to the receiver,
 * so this side never transmits (CASIC config commands are a non-goal). */
#define UNIT_GPS_UART_NUM    UART_NUM_1
#define UNIT_GPS_RX_GPIO     GPIO_NUM_54
#define UNIT_GPS_TX_GPIO     GPIO_NUM_53
#define UNIT_GPS_BAUD        115200

#define UNIT_GPS_TASK_STACK  4096
#define UNIT_GPS_TASK_PRIO   5
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

    if (!r.valid) return;    // 'V': the receiver is talking but has no lock

    s_last_fix_ms = arr_ms;
    s_have_fix    = true;

    time_sync_notify_unit_gps(r.year, r.mon, r.mday,
                              r.hour, r.min, r.sec,
                              r.frac_us, flipped ? arrival_us : 0);
}

static void unit_gps_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    char    line[UNIT_GPS_LINE_MAX];
    size_t  line_len = 0;

    while (!s_stop_req) {
        int n = uart_read_bytes(UNIT_GPS_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(UNIT_GPS_READ_MS));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    if (line_len) {
                        line[line_len] = '\0';
                        on_rmc_line(line, esp_timer_get_time());
                    }
                    line_len = 0;
                } else if (line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    line_len = 0;   // over-long: not an NMEA sentence, drop it
                }
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
    vTaskDelete(NULL);
}

bool unit_gps_start(void)
{
    if (s_running) return true;    // idempotent

    /* Order matters: params and pins first, driver last. uart_driver_install()
     * refuses to run twice, so clear any driver a previous, partially-failed
     * start may have left - the error on a first boot is expected and ignored. */
    uart_driver_delete(UNIT_GPS_UART_NUM);

    uart_config_t cfg = {
        .baud_rate  = UNIT_GPS_BAUD,
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
    e = uart_set_pin(UNIT_GPS_UART_NUM, UNIT_GPS_TX_GPIO, UNIT_GPS_RX_GPIO,
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
    gpio_set_pull_mode(UNIT_GPS_RX_GPIO, GPIO_PULLUP_ONLY);

    s_stop_req = false;
    s_prev_sec = -1;
    s_logged_state = UNIT_GPS_OFF;
    s_running = true;

    if (xTaskCreate(unit_gps_task, "unit_gps", UNIT_GPS_TASK_STACK, NULL,
                    UNIT_GPS_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "receive task could not be created");
        s_running = false;
        uart_driver_delete(UNIT_GPS_UART_NUM);
        return false;
    }

    ESP_LOGI(TAG, "UART1 up: RX=GPIO54 (pull-up) TX=GPIO53 idle, %d 8N1 - pipeline LISTENING",
             UNIT_GPS_BAUD);
    return true;
}

void unit_gps_stop(void)
{
    if (!s_running) return;    // idempotent

    s_stop_req = true;
    /* The task reads with a 100 ms timeout and checks the flag each pass, so
     * it is gone within one pass; the wait is for that, not for more data. */
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
