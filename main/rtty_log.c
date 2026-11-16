/* RTTY QSO logging and contest mode - see rtty_log.h. R030. */
#include "rtty_log.h"
#include "adif/adif_log.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rtty_log";

/* The Tab5 page (LVGL task) and the web page (httpd task) both log and both
 * ask about dupes, so the list and the serial's read-modify-write are under
 * one mutex. Made on first use; the spinlock only guards that making. */
static SemaphoreHandle_t s_mx;
static portMUX_TYPE      s_mx_init = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_mx_buf;
static void lock(void)
{
    if (!s_mx) {
        taskENTER_CRITICAL(&s_mx_init);
        if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
        taskEXIT_CRITICAL(&s_mx_init);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mx); }

/* What the Tab5's QSO card shows, for the web page (rtty_log_set_heard). */
static portMUX_TYPE   s_heard_mux = portMUX_INITIALIZER_UNLOCKED;
static rtty_log_heard_t s_heard;

/* Dupe list: this contest's RTTY QSOs, call + band. PSRAM - the internal heap
 * is the scarce one (T198). 2000 x 20 B = 40 KB at most, grown in steps. */
typedef struct { char call[14]; char band[6]; } dupe_t;
#define DUPE_STEP 256
#define DUPE_MAX  2000
static dupe_t *s_dupes;
static int     s_ndupes, s_capdupes;
static char    s_loaded_for[24];   /* contest name the list was built for */
static bool    s_loaded;

/* The archive's part (rtty_log_scan_archive): written once by the SD task,
 * then s_arc_ready is set and nothing writes it again. Readers check the
 * flag first and the name second, so a list for an older contest is ignored. */
static dupe_t       *s_arc;
static int           s_narc, s_caparc;
static char          s_arc_for[24];
static volatile bool s_arc_ready;

static void dupe_add(const char *call, const char *band)
{
    if (s_ndupes >= s_capdupes) {
        if (s_capdupes >= DUPE_MAX) return;
        dupe_t *p = heap_caps_realloc(s_dupes, (size_t)(s_capdupes + DUPE_STEP) * sizeof(dupe_t),
                                      MALLOC_CAP_SPIRAM);
        if (!p) return;
        s_dupes = p;
        s_capdupes += DUPE_STEP;
    }
    snprintf(s_dupes[s_ndupes].call, sizeof s_dupes[0].call, "%s", call);
    snprintf(s_dupes[s_ndupes].band, sizeof s_dupes[0].band, "%s", band ? band : "");
    s_ndupes++;
}

static void scan_cb(const char *call, const char *band, void *ctx)
{
    (void)ctx;
    dupe_add(call, band);
}

/* Build the list for the current contest, once per contest name. */
static void dupes_ensure(const settings_rtty_contest_t *c)
{
    if (s_loaded && !strcmp(s_loaded_for, c->name)) return;
    s_ndupes = 0;
    snprintf(s_loaded_for, sizeof s_loaded_for, "%s", c->name);
    s_loaded = true;
    if (!c->name[0]) return;   /* no name, no CONTEST_ID to match */
    uint32_t t0 = esp_log_timestamp();
    int n = adif_log_scan_contest(c->name, "RTTY", scan_cb, NULL);
    ESP_LOGI(TAG, "dupe list for '%s': %d QSOs from the device log, %lu ms", c->name, n,
             (unsigned long)(esp_log_timestamp() - t0));
}

void rtty_log_contest_changed(void) { lock(); s_loaded = false; unlock(); }

void rtty_log_set_heard(const rtty_log_heard_t *h)
{
    taskENTER_CRITICAL(&s_heard_mux);
    s_heard = *h;
    taskEXIT_CRITICAL(&s_heard_mux);
}

void rtty_log_get_heard(rtty_log_heard_t *h)
{
    taskENTER_CRITICAL(&s_heard_mux);
    *h = s_heard;
    taskEXIT_CRITICAL(&s_heard_mux);
}

static void arc_cb(const char *call, const char *band, void *ctx)
{
    (void)ctx;
    if (s_narc >= s_caparc) {
        if (s_caparc >= DUPE_MAX) return;
        dupe_t *p = heap_caps_realloc(s_arc, (size_t)(s_caparc + DUPE_STEP) * sizeof(dupe_t),
                                      MALLOC_CAP_SPIRAM);
        if (!p) return;
        s_arc = p;
        s_caparc += DUPE_STEP;
    }
    snprintf(s_arc[s_narc].call, sizeof s_arc[0].call, "%s", call);
    snprintf(s_arc[s_narc].band, sizeof s_arc[0].band, "%s", band ? band : "");
    s_narc++;
}

void rtty_log_scan_archive(const char *arc_path)
{
    if (s_arc_ready) return;   /* once per boot */
    settings_rtty_contest_t c;
    settings_get_rtty_contest(&c);
    if (!c.on || !c.name[0]) return;
    uint32_t t0 = esp_log_timestamp();
    snprintf(s_arc_for, sizeof s_arc_for, "%s", c.name);
    int n = adif_log_scan_contest_file(arc_path, c.name, "RTTY", arc_cb, NULL);
    s_arc_ready = true;
    ESP_LOGI(TAG, "archive: %d QSOs of '%s' on the card, %lu ms", n, c.name,
             (unsigned long)(esp_log_timestamp() - t0));
}

static bool in_list(const dupe_t *l, int n, const char *call, const char *band)
{
    for (int i = 0; i < n; i++)
        if (!strcasecmp(l[i].call, call) && (!band[0] || !strcasecmp(l[i].band, band))) return true;
    return false;
}

void rtty_log_sent_exch(char *out, size_t out_sz)
{
    settings_rtty_contest_t c;
    settings_get_rtty_contest(&c);
    if (c.exch[0]) snprintf(out, out_sz, "%s", c.exch);
    else           snprintf(out, out_sz, "%03u", (unsigned)c.serial_next);
}

bool rtty_log_is_dupe(const char *call, uint32_t freq_hz)
{
    if (!call || !call[0]) return false;
    settings_rtty_contest_t c;
    settings_get_rtty_contest(&c);
    if (!c.on) return false;
    const char *band = adif_log_band_for_freq(freq_hz);
    lock();
    dupes_ensure(&c);
    bool d = in_list(s_dupes, s_ndupes, call, band);
    unlock();
    return d || (s_arc_ready && !strcmp(s_arc_for, c.name) && in_list(s_arc, s_narc, call, band));
}

bool rtty_log_worked_on_band(const char *call, uint32_t freq_hz)
{
    return call && call[0] && adif_log_contains_call_on_band(call, freq_hz);
}

bool rtty_log_record(const char *call, const char *rst_sent, const char *rst_rcvd,
                     const char *stx, const char *srx, char *err, size_t err_sz)
{
    if (err && err_sz) err[0] = 0;
    uint32_t hz = cat_get_frequency();
    if (!call || !call[0]) { snprintf(err, err_sz, "No callsign"); return false; }
    /* No CAT: no frequency, so no band. A record on the wrong band is worse
     * than one not made (manual_log_modal.c's rule). */
    if (hz == 0) { snprintf(err, err_sz, "No CAT - the radio must be connected to log"); return false; }

    lock();   /* to the end: the serial is read here and moved on below */
    settings_rtty_contest_t c;
    settings_get_rtty_contest(&c);
    char my[16], grid[12];
    settings_get_my_callsign(my, sizeof my);
    settings_get_my_grid(grid, sizeof grid);
#define NZ(p) (((p) && (p)[0]) ? (p) : NULL)
    adif_qso_t q = {
        .their_call   = call,
        .my_call      = my,
        .my_grid      = grid,
        .their_grid   = NULL,
        .freq_hz      = hz,
        .mode         = "RTTY",
        .rst_sent     = NZ(rst_sent),
        .rst_rcvd     = NZ(rst_rcvd),
        .qso_time     = time(NULL),
        /* Only in contest mode, and only with a name: an ordinary QSO never
         * carries a stale CONTEST_ID (the CW page's rule, cw_qso.c). */
        .contest_name = (c.on && c.name[0]) ? c.name : NULL,
        .stx_string   = c.on ? NZ(stx) : NULL,
        .srx_string   = c.on ? NZ(srx) : NULL,
    };
#undef NZ
    if (!adif_log_record(&q)) {
        snprintf(err, err_sz, "NOT logged - log full: download it, then clear");
        unlock();
        return false;
    }
    ESP_LOGI(TAG, "logged %s RTTY %lu Hz rst %s/%s stx '%s' srx '%s'%s%s", call, (unsigned long)hz,
             q.rst_sent ? q.rst_sent : "-", q.rst_rcvd ? q.rst_rcvd : "-",
             q.stx_string ? q.stx_string : "", q.srx_string ? q.srx_string : "",
             q.contest_name ? " contest " : "", q.contest_name ? q.contest_name : "");
    if (c.on) {
        dupes_ensure(&c);
        dupe_add(call, adif_log_band_for_freq(hz));
        /* The serial moves on only when it was what we sent. A fixed exchange
         * (zone) has no counter to move. */
        char ser[8];
        snprintf(ser, sizeof ser, "%03u", (unsigned)c.serial_next);
        if (!c.exch[0] && stx && !strcmp(stx, ser)) {
            c.serial_next = (uint16_t)(c.serial_next >= 9999 ? 1 : c.serial_next + 1);
            settings_set_rtty_contest(&c);
        }
    }
    unlock();
    return true;
}
