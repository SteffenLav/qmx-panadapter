/* Host shim implementation - see test/shim/README.md.
 *
 * ⛔ This file must NOT be compiled with shim_paths.h forced in: it calls the
 * real fopen/stat itself. The harness build applies -include only to the
 * firmware source under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "sd_archive.h"
#include "settings.h"
#include "ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

int shim_log_errors     = 0;
int shim_log_quiet      = 0;
int shim_heap_fail_after = -1;
int shim_lock_depth     = 0;

const char *esp_err_to_name(esp_err_t e)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "ESP_ERR(%d)", e);
    return buf;
}

/* ---- heap ------------------------------------------------------------- */

static bool heap_should_fail(void)
{
    if (shim_heap_fail_after < 0) return false;
    if (shim_heap_fail_after == 0) return true;
    shim_heap_fail_after--;
    return false;
}

void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    return heap_should_fail() ? NULL : malloc(n);
}

void *heap_caps_calloc(size_t n, size_t sz, unsigned caps)
{
    (void)caps;
    return heap_should_fail() ? NULL : calloc(n, sz);
}

void *heap_caps_realloc(void *p, size_t n, unsigned caps)
{
    (void)caps;
    /* realloc() must leave the original block intact on failure, and the
       caller relies on exactly that - it keeps using the old table. */
    return heap_should_fail() ? NULL : realloc(p, n);
}

/* ---- path redirect ----------------------------------------------------- */

static char s_spiffs_dir[512] = ".";

void shim_set_spiffs_dir(const char *dir)
{
    snprintf(s_spiffs_dir, sizeof(s_spiffs_dir), "%s", dir);
}

const char *shim_path(const char *p)
{
    /* Rotating buffers: rename(a, b) evaluates both in one expression. */
    static char buf[4][1024];
    static int  turn = 0;
    char *out = buf[turn]; turn = (turn + 1) & 3;

    if (p && strncmp(p, "/spiffs", 7) == 0)
        snprintf(out, sizeof(buf[0]), "%s%s", s_spiffs_dir, p + 7);
    else
        snprintf(out, sizeof(buf[0]), "%s", p ? p : "");
    return out;
}

/* ---- "SPIFFS" ---------------------------------------------------------- */

esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf)
{
    (void)conf;
    return ESP_OK;      /* the harness creates the scratch dir itself */
}

esp_err_t esp_spiffs_info(const char *label, size_t *total, size_t *used)
{
    (void)label;
    if (total) *total = 1024 * 1024;    /* the real `storage` partition */
    if (used)  *used  = 0;
    return ESP_OK;
}

esp_err_t esp_spiffs_check(const char *label) { (void)label; return ESP_OK; }
esp_err_t esp_spiffs_gc(const char *label, size_t size)
{
    (void)label; (void)size;
    return ESP_OK;
}

/* ---- the rest ---------------------------------------------------------- */

void sd_archive_mark_adif_dirty(void) { }

char *sd_archive_read_adif_file(bool previous, size_t *out_len)
{
    (void)previous;
    if (out_len) *out_len = 0;
    return NULL;            /* no SD on the host */
}

void ui_toast(const char *msg) { printf("[toast] %s\n", msg ? msg : ""); }

static qmx_settings_t s_set;

void settings_load_all(qmx_settings_t *out) { if (out) *out = s_set; }
void settings_get_upload_cursors(uint32_t *qrz, uint32_t *eqsl, uint32_t *lotw)
{
    if (qrz)  *qrz  = s_set.qrz_uploaded_n;
    if (eqsl) *eqsl = s_set.eqsl_uploaded_n;
    if (lotw) *lotw = s_set.lotw_uploaded_n;
}
void settings_set_qrz_uploaded_n(uint32_t n)  { s_set.qrz_uploaded_n  = n; }
void settings_set_eqsl_uploaded_n(uint32_t n) { s_set.eqsl_uploaded_n = n; }
void settings_set_lotw_uploaded_n(uint32_t n) { s_set.lotw_uploaded_n = n; }

const char *settings_activation_sig_name(void) { return NULL; }
bool settings_get_activation_ref(char *out, size_t out_sz)
{
    (void)out; (void)out_sz;
    return false;
}

/* ---- mutex ------------------------------------------------------------- */

static int s_mutex_token;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex_token; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t)
{
    (void)h; (void)t;
    shim_lock_depth++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    (void)h;
    shim_lock_depth--;
    return pdTRUE;
}
