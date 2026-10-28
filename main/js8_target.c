/* See js8_target.h for why a tap in JS8 selects instead of transmitting. */

#include "js8_target.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "ui/ft8_screen.h"   /* FT8_CALL_MAX_LEN - one definition of a callsign */
#include "storage/settings.h"

static const char *TAG = "js8_target";

static char              s_call[FT8_CALL_MAX_LEN];
static SemaphoreHandle_t s_mutex;

/* Created on first use rather than in an init call: this is reached from a
 * touch callback and from the HTTP task, and an init that has to be remembered
 * is an init that gets forgotten on the second surface. */
static SemaphoreHandle_t lock_get(void)
{
    if (!s_mutex) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (!m) return NULL;
        /* Whoever lost the race frees its own and uses the winner's. */
        if (s_mutex) vSemaphoreDelete(m);
        else         s_mutex = m;
    }
    return s_mutex;
}

void js8_target_set(const char *call)
{
    SemaphoreHandle_t m = lock_get();
    if (m) xSemaphoreTake(m, portMAX_DELAY);
    if (call && call[0]) snprintf(s_call, sizeof(s_call), "%s", call);
    else                 s_call[0] = '\0';
    if (m) xSemaphoreGive(m);
    ESP_LOGI(TAG, "%s", (call && call[0]) ? call : "(selection cleared)");
}

void js8_target_get(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    SemaphoreHandle_t m = lock_get();
    if (m) xSemaphoreTake(m, portMAX_DELAY);
    snprintf(out, out_len, "%s", s_call);
    if (m) xSemaphoreGive(m);
}

bool js8_target_any(void)
{
    char c[FT8_CALL_MAX_LEN];
    js8_target_get(c, sizeof(c));
    return c[0] != '\0';
}

bool js8_target_is(const char *call)
{
    if (!call || !call[0]) return false;
    char c[FT8_CALL_MAX_LEN];
    js8_target_get(c, sizeof(c));
    return c[0] && strcmp(c, call) == 0;
}

/* ---- A station has asked us for a report - see js8_target.h -------------- */

static char    s_req_call[FT8_CALL_MAX_LEN];
static int     s_req_freq_hz;
static int64_t s_req_utc;

void js8_request_note(const char *call, int freq_hz, int64_t utc)
{
    if (!call || !call[0]) return;
    SemaphoreHandle_t m = lock_get();
    if (m) xSemaphoreTake(m, portMAX_DELAY);
    bool same = (strcmp(s_req_call, call) == 0);
    snprintf(s_req_call, sizeof(s_req_call), "%s", call);
    s_req_freq_hz = freq_hz;
    s_req_utc     = utc;
    if (m) xSemaphoreGive(m);
    /* Not once per repeat: JS8 stations re-send a directed command until they
     * get an answer, and one line per repeat is what turns an overnight
     * capture into something nobody reads. */
    if (!same) ESP_LOGI(TAG, "%s sent SNR? - a report is owed (tone %d Hz)", call, freq_hz);
}

bool js8_request_get(char *call, size_t out_len, int *freq_hz, int *age_s)
{
    if (call && out_len) call[0] = '\0';
    SemaphoreHandle_t m = lock_get();
    if (m) xSemaphoreTake(m, portMAX_DELAY);
    bool live = false;
    if (s_req_call[0]) {
        int64_t age = (int64_t)time(NULL) - s_req_utc;
        /* A clock just stepped by SNTP can put the note in the future. That is
         * "it has only just arrived", not "it has expired": the frame was
         * decoded whatever the clock then did. */
        if (age < 0) age = 0;
        if (age <= JS8_REQUEST_WINDOW_SEC) {
            live = true;
            if (call && out_len) snprintf(call, out_len, "%s", s_req_call);
            if (freq_hz) *freq_hz = s_req_freq_hz;
            if (age_s)   *age_s   = (int)age;
        } else {
            s_req_call[0] = '\0';   /* lapsed - stop offering it */
        }
    }
    if (m) xSemaphoreGive(m);
    return live;
}

void js8_request_clear(void)
{
    SemaphoreHandle_t m = lock_get();
    if (m) xSemaphoreTake(m, portMAX_DELAY);
    s_req_call[0] = '\0';
    if (m) xSemaphoreGive(m);
}

void js8_text_prefix(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    out[0] = '\0';

    char me[20];
    settings_get_my_callsign(me, sizeof(me));
    if (!me[0]) return;              /* nothing to address it from yet */

    char them[FT8_CALL_MAX_LEN];
    js8_target_get(them, sizeof(them));
    if (them[0]) snprintf(out, out_len, "%s %s ", them, me);
    else         snprintf(out, out_len, "%s ", me);
}
