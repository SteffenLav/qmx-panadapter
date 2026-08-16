#include "qmx_term.h"

#include "usb/cdc_acm_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "util/psram_task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "qmx_term";

/* The QMX's second CDC function. Measured on hardware, and NOT guessable: the
 * audio function occupies interfaces 2-4, so the second serial port starts at 5
 * rather than at 1. An earlier probe assumed the CDC functions were contiguous,
 * stopped at the first gap, and reported a two-port radio as having one. */
#define QMX_TERM_INTERFACE 5

/* Same radio, same identifiers as cat.c - kept local rather than exported so the
 * CAT layer's own copies stay private to it. */
#define QMX_VID       0x0483
#define QMX_PID       0xA34C
#define QMX_TERM_BAUD 38400

/* Close the session if nobody has touched it for this long.
 *
 * This is a SAFETY property, not tidiness. The operator's browser can close its
 * tab, lose WiFi, or go flat at any moment, and a session abandoned that way
 * would leave the radio sitting in terminal applications mode with nothing left
 * to walk it out. The watchdog is the only thing that guarantees the exit the
 * QMX manual insists on. Two minutes is longer than anyone reads one menu for,
 * and every screen poll counts as activity - so it only fires when the far end
 * really has gone away. */
#define QMX_TERM_IDLE_MS 120000

static cdc_acm_dev_hdl_t s_dev;
static ansi_term_t      *s_scr;          /* PSRAM: ~4 KB of cells */
static bool              s_open;
static int64_t           s_last_activity_us;
static volatile int64_t  s_last_rx_us;
static TaskHandle_t      s_idle_task;

/* Two locks, deliberately.
 *
 * s_lock covers the SESSION (open/close/key) and is held across the blocking
 * CDC writes. s_scr_lock covers only the screen model, and is what the driver's
 * RX callback takes to feed bytes in. They are separate because a single lock
 * would mean a 300 ms blocking TX stalls the CDC driver task that is trying to
 * deliver the reply to that very write. Nothing ever holds both at once - the
 * screen reads inside exit_terminal_mode() take s_scr_lock and give it straight
 * back before any byte goes out. */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_scr_lock;

static void ensure_locks(void)
{
    if (!s_lock)     s_lock     = xSemaphoreCreateMutex();
    if (!s_scr_lock) s_scr_lock = xSemaphoreCreateMutex();
}

/* Runs on the CDC driver's own client task.
 *
 * ⛔ IT MUST NEVER BLOCK, and the timeout below is zero for a reason that cost a
 * device reboot to find. That task is also what reaps flushed URBs during
 * cdc_acm_host_close(), and `usb_host_interface_release()` allows it exactly
 * vTaskDelay(10) to do so - which at this project's CONFIG_FREERTOS_HZ=1000 is
 * 10 MILLISECONDS, not the 100 ms the vendor comment reads like. An earlier
 * version waited up to 200 ms here for the screen lock; during the exit walk the
 * radio repaints after every keystroke, so the callback was in flight when close
 * ran, the URB was never reaped, release returned ESP_ERR_INVALID_STATE, and the
 * driver's own ESP_ERROR_CHECK called abort(). Serial-captured, cdc_acm_host.c
 * line 717.
 *
 * Dropping a chunk when the lock is momentarily held is harmless: readers hold
 * it for microseconds, and the QMX repaints the whole screen on every keystroke
 * anyway, so anything missed is overwritten within one key. */
static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    s_last_rx_us = esp_timer_get_time();
    if (s_scr && s_scr_lock && xSemaphoreTake(s_scr_lock, 0) == pdTRUE) {
        ansi_term_feed(s_scr, data, len);
        xSemaphoreGive(s_scr_lock);
    }
    return true;
}

static bool tx(const char *s, int len)
{
    if (!s_dev) return false;
    return cdc_acm_host_data_tx_blocking(s_dev, (const uint8_t *)s, len, 300) == ESP_OK;
}

bool qmx_term_is_open(void) { return s_open; }

const ansi_term_t *qmx_term_lock_screen(void)
{
    if (!s_open || !s_scr || !s_scr_lock) return NULL;
    if (xSemaphoreTake(s_scr_lock, pdMS_TO_TICKS(300)) != pdTRUE) return NULL;
    if (!s_open) { xSemaphoreGive(s_scr_lock); return NULL; }
    s_last_activity_us = esp_timer_get_time();
    return s_scr;
}

void qmx_term_unlock_screen(void)
{
    if (s_scr_lock) xSemaphoreGive(s_scr_lock);
}

/* ---- screen queries, used by the exit walk. Caller must NOT hold s_scr_lock. */

/* Find the row whose text contains `needle`, or -1. */
static int find_row(const char *needle)
{
    int found = -1;
    if (!s_scr || xSemaphoreTake(s_scr_lock, pdMS_TO_TICKS(300)) != pdTRUE) return -1;
    char line[ANSI_COLS + 1];
    for (int r = 0; r < ANSI_ROWS && found < 0; r++) {
        ansi_term_row_text(s_scr, r, line);
        if (strstr(line, needle)) found = r;
    }
    xSemaphoreGive(s_scr_lock);
    return found;
}

/* The row currently drawn in reverse video, i.e. the selected menu item, or -1.
 * Rows 0-1 are skipped because the menu TITLE is also reverse video and is drawn
 * INTO the top border ("+---Main menu------+") - measured, not reasoned. Without
 * the skip this finds the title every time and the exit walk never converges. */
static int find_selected_row(void)
{
    int found = -1;
    if (!s_scr || xSemaphoreTake(s_scr_lock, pdMS_TO_TICKS(300)) != pdTRUE) return -1;
    for (int r = 2; r < ANSI_ROWS && found < 0; r++) {
        for (int c = 0; c < ANSI_COLS; c++) {
            if (s_scr->cell[r][c].reverse && s_scr->cell[r][c].ch != ' ') { found = r; break; }
        }
    }
    xSemaphoreGive(s_scr_lock);
    return found;
}

/* Leave terminal mode the way the radio wants: select "Exit terminal" and press
 * Enter. Driven off the SCREEN rather than a fixed key count, so it survives a
 * different menu length or a different starting selection. Caller holds s_lock. */
static void exit_terminal_mode(void)
{
    /* Back out of any nested application first (manual: Ctrl-Q returns to the
     * main menu). Repeated, because we may be more than one level deep. */
    for (int i = 0; i < 3; i++) { tx("\x11", 1); vTaskDelay(pdMS_TO_TICKS(150)); }

    for (int attempt = 0; attempt < 12; attempt++) {
        int target = find_row("Exit terminal");
        int sel    = find_selected_row();
        if (target < 0) {
            ESP_LOGW(TAG, "exit: no 'Exit terminal' row visible (attempt %d)", attempt);
            break;
        }
        if (sel == target) {
            tx("\r", 1);                       /* select it */
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP_LOGI(TAG, "terminal exited cleanly - radio is back on CAT commands");
            return;
        }
        /* Step one row toward it and re-read; one key at a time so a wrapping
         * menu cannot run away from us. */
        const char *key = (sel < 0 || sel < target) ? "\x1b[B" : "\x1b[A";
        tx(key, 3);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGW(TAG, "exit: could not reach 'Exit terminal' - the radio may still be "
                  "in terminal mode on port 2 (CAT on port 1 is unaffected)");
}

/* ---- session ---- */

/* Wait for the radio to stop sending before tearing the port down.
 *
 * The exit walk ends with a keystroke, and the QMX answers every keystroke with
 * a repaint. Closing into the middle of that is what leaves a URB in flight when
 * usb_host_interface_release() checks - see the note on on_rx(). This does not
 * make the driver's 10 ms window bigger; it makes sure we are not asking for it
 * at the one moment it cannot be met. */
static void wait_for_quiet(void)
{
    const int64_t QUIET_US = 250000;   /* no bytes for this long = settled */
    int64_t deadline = esp_timer_get_time() + 2000000;
    while (esp_timer_get_time() < deadline) {
        if (esp_timer_get_time() - s_last_rx_us > QUIET_US) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "port still busy after 2 s - closing anyway");
}

static void close_locked(void)
{
    if (!s_open) return;
    exit_terminal_mode();
    wait_for_quiet();
    if (s_scr_lock && xSemaphoreTake(s_scr_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_open = false;                 /* under the screen lock, so a reader in
                                           flight cannot see a closed session */
        xSemaphoreGive(s_scr_lock);
    } else {
        s_open = false;
    }
    if (s_dev) { cdc_acm_host_close(s_dev); s_dev = NULL; }
    ESP_LOGI(TAG, "terminal session closed");
}

static void idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_open) break;
        if ((esp_timer_get_time() - s_last_activity_us) / 1000 < QMX_TERM_IDLE_MS) continue;

        ESP_LOGW(TAG, "no activity for %d s - closing the terminal session so the "
                      "radio is not left in terminal mode", QMX_TERM_IDLE_MS / 1000);
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
            close_locked();
            xSemaphoreGive(s_lock);
        }
        break;
    }
    s_idle_task = NULL;
    vTaskDelete(NULL);
}

bool qmx_term_open(void)
{
    ensure_locks();
    if (!s_lock || !s_scr_lock) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;

    if (s_open) { s_last_activity_us = esp_timer_get_time();
                  xSemaphoreGive(s_lock); return true; }

    if (!s_scr) {
        s_scr = heap_caps_malloc(sizeof(*s_scr), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scr) {
            ESP_LOGE(TAG, "no PSRAM for the screen model");
            xSemaphoreGive(s_lock);
            return false;
        }
    }
    ansi_term_reset(s_scr);

    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 64,
        .in_buffer_size  = 512,
        .event_cb = NULL,
        .data_cb  = on_rx,
        .user_arg = NULL,
    };
    esp_err_t e = cdc_acm_host_open(QMX_VID, QMX_PID, QMX_TERM_INTERFACE, &cfg, &s_dev);
    if (e != ESP_OK || !s_dev) {
        ESP_LOGW(TAG, "port 2 (interface %d) would not open (0x%x) - is the radio "
                      "set to 2 USB serial ports?", QMX_TERM_INTERFACE, e);
        s_dev = NULL;
        xSemaphoreGive(s_lock);
        return false;
    }
    const cdc_acm_line_coding_t lc = {
        .dwDTERate = QMX_TERM_BAUD, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8,
    };
    cdc_acm_host_line_coding_set(s_dev, &lc);
    cdc_acm_host_set_control_line_state(s_dev, true, true);

    s_open = true;
    s_last_activity_us = esp_timer_get_time();

    /* A bare CR is what switches the radio out of CAT-command mode into the
     * terminal application (manual 8.2). It answers with a full repaint.
     *
     * ⚠ AND IT IS SOMETIMES LOST, so it is RETRIED. Measured: 2 of 6 opens came
     * up with a completely blank screen, and the operator found the same thing
     * by hand - "sometimes there is a blank screen and only if I press enter
     * then it populates". Pressing Enter sends exactly this byte, which is the
     * tell. The radio's CDC port is evidently not ready the instant after we
     * assert DTR, so the first CR goes nowhere.
     *
     * A single lost CR was unrecoverable rather than merely slow: nothing else
     * ever writes to the port unprompted, so the screen model stayed empty, and
     * the UI's own poll skips a repaint while dirty_seq is unchanged. Blank
     * forever, until a keystroke happened to supply the missing CR.
     *
     * So: give the port a moment, then send a CR and wait for ANY byte back,
     * up to three times. s_last_rx_us is written by the RX callback, so this
     * tests what the radio actually did rather than what we sent. */
    vTaskDelay(pdMS_TO_TICKS(150));
    bool answered = false;
    for (int attempt = 1; attempt <= 3 && !answered; attempt++) {
        int64_t sent_at = esp_timer_get_time();
        tx("\r", 1);
        for (int waited = 0; waited < 600; waited += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (s_last_rx_us > sent_at) { answered = true; break; }
        }
        if (!answered) ESP_LOGW(TAG, "no reply to the opening CR (attempt %d/3)", attempt);
    }
    if (answered) {
        /* The repaint is several packets; let it finish before anyone draws. */
        vTaskDelay(pdMS_TO_TICKS(250));

        /* ⭐ STEP OFF "Exit terminal" - a trap of our OWN making.
         *
         * The radio remembers the selected item between sessions, and the last
         * thing our close does is select "Exit terminal" and press Enter. So
         * every session after the first opens with that item highlighted, and
         * the operator's first Enter - the most natural key on a menu - drops
         * them straight back out. Measured: after open, the only selection was
         * row 7, ' Exit terminal  '.
         *
         * One Down wraps to the first item (measured: row 7 -> row 3), so the
         * operator arrives somewhere useful instead of one keypress from the
         * door. */
        int exit_row = find_row("Exit terminal");
        if (exit_row >= 0 && find_selected_row() == exit_row) {
            tx("\x1b[B", 3);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    } else {
        ESP_LOGW(TAG, "the radio never answered - the port is open but the screen "
                      "will be blank until a key is pressed");
    }

    /* Background housekeeping, and the strings it writes are literals rather
     * than stack buffers, so a PSRAM stack is safe here. */
    if (!s_idle_task) {
        s_idle_task = psram_task_create(idle_task, "qmx_term_idle", 3072, NULL, 2,
                                        tskNO_AFFINITY);
    }
    ESP_LOGI(TAG, "terminal session open on interface %d", QMX_TERM_INTERFACE);
    xSemaphoreGive(s_lock);
    return true;
}

void qmx_term_close(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "close: session busy, not closing");
        return;
    }
    close_locked();
    xSemaphoreGive(s_lock);
}

bool qmx_term_key(const char *name)
{
    if (!s_lock || !name || !name[0]) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) return false;

    bool ok = false;
    if (s_open) {
        s_last_activity_us = esp_timer_get_time();
        if      (!strcmp(name, "up"))     ok = tx("\x1b[A", 3);
        else if (!strcmp(name, "down"))   ok = tx("\x1b[B", 3);
        else if (!strcmp(name, "right"))  ok = tx("\x1b[C", 3);
        else if (!strcmp(name, "left"))   ok = tx("\x1b[D", 3);
        else if (!strcmp(name, "enter"))  ok = tx("\r", 1);
        else if (!strcmp(name, "esc"))    ok = tx("\x1b", 1);
        else if (!strcmp(name, "ctrl-q")) ok = tx("\x11", 1);
        else if (!strcmp(name, "bksp"))   ok = tx("\b", 1);
        else if (name[1] == '\0')         ok = tx(name, 1);   /* a literal character */
        else ESP_LOGW(TAG, "unknown key '%s'", name);
    }
    xSemaphoreGive(s_lock);
    return ok;
}
