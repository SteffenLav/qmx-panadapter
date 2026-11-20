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
#include "esp_timer.h"
#include "sd_archive.h"
#include "settings.h"
#include "ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

int shim_log_errors     = 0;
int shim_log_quiet      = 0;
int shim_heap_fail_after = -1;
int shim_lock_depth     = 0;
int64_t shim_now_us     = 0;

int64_t esp_timer_get_time(void) { return shim_now_us; }

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

/* No card and no competing task on the host, so the lock always succeeds.
 * It must still EXIST: adif_log.c refuses the rewrite on a failed lock, and
 * a stub that returned false would make the harness test the refusal path
 * instead of the cache. */
bool sd_archive_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void sd_archive_unlock(void) { }

/* No card on the host. adif_log_export_activation() returns -1 on this, so
 * the harness never exercises the SD export path - which is correct: it
 * tests the worked-call cache, not the card. */
bool sd_archive_is_mounted(void) { return false; }

char *sd_archive_read_adif_file(bool previous, size_t *out_len)
{
    (void)previous;
    if (out_len) *out_len = 0;
    return NULL;            /* no SD on the host */
}

bool sd_archive_save_flash_diag(void) { return false; }
/* diag_log.c is the whole logging subsystem; the rotation path only asks it
 * to drop its persisted copy to make room. Nothing persisted on the host. */
bool diag_log_persist_drop(void) { return false; }

/* Link-level breadcrumbs. adif_log.c sees test/shim/panic_hook.h (inline, no
 * symbol), but main/util/spiffs_guard.c finds the REAL main/util/panic_hook.h
 * first - a quoted #include searches the including file's own directory
 * before any -I - and that one declares them extern. */
void panic_hook_crumb_enter(const char *site) { (void)site; }
void panic_hook_crumb_exit(void) { }

/* No LoTW certificate on the host, so the LoTW cursor never limits the
 * rotation's upload floor here - the same as a station without LoTW set up. */
bool lotw_cert_present(void) { return false; }
bool sd_archive_archive_info(long *bytes, uint32_t *records)
{
    if (bytes)   *bytes   = 0;
    if (records) *records = 0;
    return false;
}
bool sd_archive_adif_has_archive_prefix(bool previous, long arc_bytes)
{
    (void)previous; (void)arc_bytes;
    return false;
}
char *sd_archive_read_adif_file_at(bool previous, size_t offset, size_t *out_len)
{
    (void)previous; (void)offset;
    if (out_len) *out_len = 0;
    return NULL;
}

void ui_toast(const char *msg) { printf("[toast] %s\n", msg ? msg : ""); }

static qmx_settings_t s_set;

void settings_load_all(qmx_settings_t *out) { if (out) *out = s_set; }
void settings_get_upload_cursors(uint32_t *qrz, uint32_t *eqsl, uint32_t *lotw,
                                 uint32_t *cloudlog)
{
    if (qrz)      *qrz      = s_set.qrz_uploaded_n;
    if (eqsl)     *eqsl     = s_set.eqsl_uploaded_n;
    if (lotw)     *lotw     = s_set.lotw_uploaded_n;
    if (cloudlog) *cloudlog = s_set.cloudlog_uploaded_n;
}
void settings_set_cloudlog_uploaded_n(uint32_t n) { s_set.cloudlog_uploaded_n = n; }

/* Same arithmetic as settings_shift_upload_cursors() in settings.c: a rotation
 * that drops d records moves every cursor back by d, floored at 0. */
static uint32_t s_rot_base;
uint32_t settings_get_rot_base(void) { return s_rot_base; }
void settings_set_rot_base(uint32_t base) { s_rot_base = base; }
void settings_shift_upload_cursors(uint32_t to_base)
{
    if (to_base == s_rot_base) return;
    if (to_base > s_rot_base) {
        uint32_t d = to_base - s_rot_base;
        uint32_t *c[4] = { &s_set.qrz_uploaded_n, &s_set.eqsl_uploaded_n,
                           &s_set.lotw_uploaded_n, &s_set.cloudlog_uploaded_n };
        for (int i = 0; i < 4; i++) *c[i] = (*c[i] > d) ? *c[i] - d : 0;
    }
    s_rot_base = to_base;   /* either way the base follows, as in settings.c */
}
void settings_flush(void) { }
static bool s_log_seen;
bool settings_qso_log_seen(void) { return s_log_seen; }
void settings_mark_qso_log_seen(void) { s_log_seen = true; }
void settings_set_qrz_uploaded_n(uint32_t n)  { s_set.qrz_uploaded_n  = n; }
void settings_set_eqsl_uploaded_n(uint32_t n) { s_set.eqsl_uploaded_n = n; }
void settings_set_lotw_uploaded_n(uint32_t n) { s_set.lotw_uploaded_n = n; }

const char *settings_activation_sig_name(void) { return NULL; }
const char *settings_activation_adif_field(void) { return NULL; }
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

static int s_rmutex_token;
int shim_rlock_depth;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return &s_rmutex_token; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t t)
{
    (void)h; (void)t;
    shim_rlock_depth++;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h)
{
    (void)h;
    shim_rlock_depth--;
    return pdTRUE;
}
