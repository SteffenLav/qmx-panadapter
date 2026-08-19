#include "qmx_term.h"

#include "usb/cdc_acm_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "util/psram_task.h"
#include "cat.h"          /* cat_request_iq_reassert - see close_locked */
#include "ui.h"           /* ui_flat_mode_reset                        */
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

/* Opening the second port is retried before the failure is blamed on the radio's
 * serial-port setting (#198). Deliberately short: a radio that only has one port
 * fails all of these in ~0.6 s, which is not a delay anyone notices, while a
 * device still finishing an enumeration usually needs only one more try. */
#define QMX_TERM_OPEN_ATTEMPTS  4
#define QMX_TERM_OPEN_RETRY_MS  200

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

/* The row currently drawn in reverse video that is the SELECTED MENU ITEM, or -1.
 *
 * Reverse video marks more than the selection: every open box's TITLE is reverse
 * too, drawn into its top border. The discriminator is the border itself - a
 * title's row contains the box's "+-" corner/edge characters, an item's row does
 * not. Measured in a nested menu, which is the case that matters:
 *
 *   row 1  '+---Main menu------+'      reverse "Main menu"      <- title
 *   row 2  '|+---Configuration--+'     reverse "Configuration"  <- title
 *   row 5  '|  CW              |'      reverse " CW "           <- THE SELECTION
 *
 * ⛔ The first version skipped rows 0-1 instead, on the strength of the MAIN
 * menu, where the only title is on row 1. That is wrong the moment a submenu
 * opens, because the nested box puts its title on row 2 - so this returned the
 * submenu's title and the exit walk would have chased it. It never bit only
 * because exit_terminal_mode() sends Ctrl-Q three times first and so is usually
 * back on the main menu by the time it looks. Do not reintroduce a row-number
 * rule; the border test holds at any depth. */
static bool row_is_box_border(int r)
{
    for (int c = 0; c + 1 < ANSI_COLS; c++) {
        if (s_scr->cell[r][c].ch == '+' &&
            (s_scr->cell[r][c + 1].ch == '-' || (c > 0 && s_scr->cell[r][c - 1].ch == '-')))
            return true;
    }
    return false;
}

static int find_selected_row(void)
{
    int found = -1;
    if (!s_scr || xSemaphoreTake(s_scr_lock, pdMS_TO_TICKS(300)) != pdTRUE) return -1;
    for (int r = 0; r < ANSI_ROWS && found < 0; r++) {
        if (row_is_box_border(r)) continue;          /* a title, not the selection */
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

/* The port IS closed when a session ends, and that matters: holding a second
 * CDC interface claimed across a QMX power cycle abort()s in IDF's hub layer
 * ("dev_tree_node_dev_gone(NULL, 0)", hub.c:435). Keeping it open was tried and
 * traded one crash for a worse one, since power-cycling the radio is routine.
 *
 * The close itself is only safe because of the standing patch
 * tools/patches/apply_cdc_acm_close_tolerant.ps1 - stock cdc_acm_host_close()
 * feeds usb_host_interface_release() into ESP_ERROR_CHECK, and that returns
 * ESP_ERR_INVALID_STATE whenever an endpoint still has a URB in flight. If this
 * ever starts abort()ing again at cdc_acm_host.c:717, that patch is missing:
 * managed_components/ is git-ignored and wiped by fullclean. */
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

    /* ⭐ Hand the radio back the way the operator-pause resume does, because a
     * terminal session IS a trip through the radio's own menus and has the same
     * two after-effects (#175, Roy KI0ER):
     *
     *   "when I exit the QMX menu on the Tab5 after changing my sidetone volume,
     *    I still need to select 'Let me use the QMX menus' followed immediately by
     *    'Done - Tab5 takes over' in order to clear the waterfall back to its
     *    normal state because the waterfall starts doing the phantom thing."
     *
     * His workaround is literally this code path, which is what identified the
     * cause: resume re-runs the IQ handshake and re-seeds the flat floor, and a
     * terminal close did neither.
     *
     *   - Q9 (IQ mode) is SESSION state on the QMX and a menu visit can drop it.
     *     The radio then keeps streaming audio at full rate while it is no longer
     *     I/Q, so CAT answers normally and the spectrum is wrong - exactly the
     *     shape of the symptom. Queued for the poll task, which owns the pipe.
     *   - Anything changed in there (sidetone, RF gain, a band setting) moves the
     *     noise floor the panadapter is calibrated against.
     *
     * Both are free when nothing changed: the handshake is a no-op if IQ was
     * already on, and a floor re-seed costs one frame. ui_flat_mode_reset() only
     * touches two statics plus the waterfall floor - no LVGL objects - so it is
     * safe from this task without the display lock. */
    cat_request_iq_reassert();
    ui_flat_mode_reset();

    ESP_LOGI(TAG, "terminal session closed - IQ re-assert queued, flat floor re-seeded");
}

/* The QMX going away mid-session. Without this the handle is stale and the next
 * open() thinks a session is still up. cdc_acm_host requires the user to close
 * the device from here - it will not do it for us. */
static void on_cdc_event(const cdc_acm_host_dev_event_data_t *ev, void *arg)
{
    (void)arg;
    if (!ev || ev->type != CDC_ACM_HOST_DEVICE_DISCONNECTED) return;
    ESP_LOGW(TAG, "the radio disconnected with a terminal session open");
    s_open = false;
    if (s_dev) { cdc_acm_host_close(s_dev); s_dev = NULL; }
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
        .event_cb = on_cdc_event,
        .data_cb  = on_rx,
        .user_arg = NULL,
    };
    // RETRY BEFORE BLAMING THE RADIO'S CONFIGURATION (#198, Samuel W7STF).
    // He got "set the radio to two USB serial ports" once after swapping cables,
    // on a radio that was already set correctly. This open used to be one shot, and
    // ANY failure produced that wording - so a device still coming back up after a
    // re-enumeration was reported as a settings mistake, sending the operator into
    // the radio's menus to change something that was already right.
    //
    // The two cases genuinely differ in how they fail: a radio set to ONE serial
    // port has no interface 5 and will never have one, so it fails identically
    // every time; a device mid-enumeration fails now and succeeds a moment later.
    // Retrying is what separates them. Same shape as the IQ-mode handshake in cat.c
    // and the opening-CR retry below, both of which exist for the same reason.
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < QMX_TERM_OPEN_ATTEMPTS; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(QMX_TERM_OPEN_RETRY_MS));
        e = cdc_acm_host_open(QMX_VID, QMX_PID, QMX_TERM_INTERFACE, &cfg, &s_dev);
        if (e == ESP_OK && s_dev) break;
        s_dev = NULL;
    }
    if (e != ESP_OK || !s_dev) {
        // Only name the setting when the radio is demonstrably THERE. CAT lives on
        // interface 0 of the same device, so a live CAT link proves it enumerated
        // and the missing interface really is a configuration matter. Without it we
        // cannot tell a one-port radio from an absent/rebooting one, and saying so
        // is better than guessing wrong in the direction of "go change your menus".
        if (cat_is_ready())
            ESP_LOGW(TAG, "port 2 (interface %d) would not open (0x%x) after %d tries - "
                          "is the radio set to 2 USB serial ports?",
                     QMX_TERM_INTERFACE, e, QMX_TERM_OPEN_ATTEMPTS);
        else
            ESP_LOGW(TAG, "port 2 (interface %d) would not open (0x%x) after %d tries, "
                          "and CAT is not up either - the radio is not connected or is "
                          "still starting, so this is NOT a serial-port setting problem",
                     QMX_TERM_INTERFACE, e, QMX_TERM_OPEN_ATTEMPTS);
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
        /* ⭐ THE DELETE BYTE IS 0x7F, MEASURED ON THE RADIO - not 0x08.
         *
         * This was got wrong twice, so the evidence is written down rather than
         * the conclusion. Randy N4OPI answered the open question from PuTTY:
         * "landing in a numerical field puts the cursor at the right most digit,
         * Backspace deletes leftward and then you can type in the desired values,
         * Del does nothing." I read "Backspace" as 0x08, shipped 0x08 as the only
         * key and deleted 0x7F.
         *
         * PuTTY's Backspace sends 0x7F by default (its "Backspace key" option
         * defaults to Control-?), so his Backspace WAS 0x7F all along, and his
         * "Del does nothing" refers to PuTTY's Delete, which sends ESC[3~.
         *
         * Measured on 1_04_004, in Configuration -> Protection -> Max. PA
         * voltage, with up/down as the control to prove the key path works:
         *
         *   up / down        selection moves, screen updates      (transport OK)
         *   left / right     NOTHING - the radio does not answer
         *   Enter            NOTHING
         *   0x08 (BS)        NOTHING
         *   0x7F (DEL)       "11.5" -> "11." -> "11"   deletes leftward ✓
         *   then '9'         "11" -> "119"              typing appends ✓
         *
         * So the key stays NAMED "bksp" and LABELLED BS - backspace is what it
         * does to the operator - and sends 0x7F, which is what the radio acts on.
         *
         * ⚠ The editing model is backspace-and-retype, NOT arrow-adjust: L/R do
         * nothing on a field like this, and per Randy the arrows moving between
         * columns in a TABLE is correct behaviour. Do not "fix" that.
         *
         * ⚠ And do not conclude anything about numeric fields from the Messages
         * field: my earlier probe there found 0x08 moving the cursor without
         * deleting, which is a text field behaving differently and proved nothing.
         */
        else if (!strcmp(name, "bksp"))   ok = tx("\x7f", 1);   /* 0x7F - see above */
        else if (name[1] == '\0')         ok = tx(name, 1);   /* a literal character */
        else ESP_LOGW(TAG, "unknown key '%s'", name);
    }
    xSemaphoreGive(s_lock);
    return ok;
}
