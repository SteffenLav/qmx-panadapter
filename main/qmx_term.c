#include "qmx_term.h"
#include "cat/cat.h"

#include "usb/cdc_acm_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static cdc_acm_dev_hdl_t s_dev;
static ansi_term_t      *s_scr;          /* PSRAM: ~4 KB of cells */
static bool              s_open;

static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    if (s_scr) ansi_term_feed(s_scr, data, len);
    return true;
}

static bool tx(const char *s, int len)
{
    if (!s_dev) return false;
    return cdc_acm_host_data_tx_blocking(s_dev, (const uint8_t *)s, len, 300) == ESP_OK;
}

bool qmx_term_is_open(void) { return s_open; }

const ansi_term_t *qmx_term_screen(void) { return s_open ? s_scr : NULL; }

bool qmx_term_open(void)
{
    if (s_open) return true;

    if (!s_scr) {
        s_scr = heap_caps_malloc(sizeof(*s_scr), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scr) { ESP_LOGE(TAG, "no PSRAM for the screen model"); return false; }
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
        return false;
    }
    const cdc_acm_line_coding_t lc = {
        .dwDTERate = QMX_TERM_BAUD, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8,
    };
    cdc_acm_host_line_coding_set(s_dev, &lc);
    cdc_acm_host_set_control_line_state(s_dev, true, true);

    s_open = true;
    /* A bare CR is what switches the radio out of CAT-command mode into the
     * terminal application (manual 8.2). It answers with a full repaint. */
    tx("\r", 1);
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGI(TAG, "terminal session open on interface %d", QMX_TERM_INTERFACE);
    return true;
}

/* Find the row whose text contains `needle`, or -1. */
static int find_row(const char *needle)
{
    if (!s_scr) return -1;
    char line[ANSI_COLS + 1];
    for (int r = 0; r < ANSI_ROWS; r++) {
        ansi_term_row_text(s_scr, r, line);
        if (strstr(line, needle)) return r;
    }
    return -1;
}

/* The row currently drawn in reverse video, i.e. the selected menu item, or -1.
 * Row 1 is skipped because the menu TITLE is also reverse video and lives in the
 * top border - measured, and it would otherwise always look like the selection. */
static int find_selected_row(void)
{
    if (!s_scr) return -1;
    for (int r = 2; r < ANSI_ROWS; r++) {
        for (int c = 0; c < ANSI_COLS; c++) {
            if (s_scr->cell[r][c].reverse && s_scr->cell[r][c].ch != ' ') return r;
        }
    }
    return -1;
}

/* Leave terminal mode the way the radio wants: select "Exit terminal" and press
 * Enter. Driven off the SCREEN rather than a fixed key count, so it survives a
 * different menu length or a different starting selection. */
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

void qmx_term_close(void)
{
    if (!s_open) return;
    exit_terminal_mode();
    if (s_dev) { cdc_acm_host_close(s_dev); s_dev = NULL; }
    s_open = false;
    ESP_LOGI(TAG, "terminal session closed");
}

bool qmx_term_key(const char *name)
{
    if (!s_open || !name || !name[0]) return false;

    if (!strcmp(name, "up"))      return tx("\x1b[A", 3);
    if (!strcmp(name, "down"))    return tx("\x1b[B", 3);
    if (!strcmp(name, "right"))   return tx("\x1b[C", 3);
    if (!strcmp(name, "left"))    return tx("\x1b[D", 3);
    if (!strcmp(name, "enter"))   return tx("\r", 1);
    if (!strcmp(name, "esc"))     return tx("\x1b", 1);
    if (!strcmp(name, "ctrl-q"))  return tx("\x11", 1);
    if (!strcmp(name, "bksp"))    return tx("\b", 1);
    if (name[1] == '\0')          return tx(name, 1);   /* a literal character */
    ESP_LOGW(TAG, "unknown key '%s'", name);
    return false;
}
