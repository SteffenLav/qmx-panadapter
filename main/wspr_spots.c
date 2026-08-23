/* WSPR spot store. See wspr_spots.h for why this is a ring of individual spots
 * rather than a merged-by-callsign table like ft8_screen.c's. */

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wspr_spots.h"

static const char *TAG = "wspr_spots";

/* PSRAM, not .bss. 256 spots is ~10 KB, and CLAUDE.md records what putting
 * arrays of exactly this size in .bss did to this board: three static tables
 * (a 22.5 KB worked-call cache and two 11.25 KB snapshots) were occupying the
 * DMA-capable internal region, and moving them to PSRAM took MALLOC_CAP_DMA
 * from ~311 B free to 40 KB. A new 10 KB static array is the same mistake in
 * miniature. */
static wspr_spot_t  *s_ring;
static int           s_count;    /* how many valid entries (<= WSPR_SPOT_RING) */
static int           s_head;     /* next write index */
static SemaphoreHandle_t s_mtx;

void wspr_spots_init(void)
{
    if (s_ring) return;
    s_ring = (wspr_spot_t *)heap_caps_calloc(WSPR_SPOT_RING, sizeof(wspr_spot_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mtx = xSemaphoreCreateMutex();
    if (!s_ring || !s_mtx) {
        ESP_LOGE(TAG, "init failed - spots will not be recorded");
        return;
    }
    s_count = s_head = 0;
    ESP_LOGI(TAG, "spot store ready (%d slots, %u bytes, PSRAM)",
             WSPR_SPOT_RING, (unsigned)(WSPR_SPOT_RING * sizeof(wspr_spot_t)));
}

static inline bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) == pdTRUE;
}
static inline void unlock(void) { if (s_mtx) xSemaphoreGive(s_mtx); }

void wspr_spots_add(const wspr_spot_t *spot)
{
    if (!s_ring || !spot) return;
    if (!lock()) return;
    s_ring[s_head] = *spot;
    s_head = (s_head + 1) % WSPR_SPOT_RING;
    if (s_count < WSPR_SPOT_RING) s_count++;
    unlock();
}

int wspr_spots_get(wspr_spot_t *out, int max)
{
    if (!s_ring || !out || max <= 0) return 0;
    if (!lock()) return 0;
    int n = s_count < max ? s_count : max;
    /* newest first: walk backwards from the most recent write */
    for (int k = 0; k < n; k++) {
        int idx = (s_head - 1 - k + WSPR_SPOT_RING * 2) % WSPR_SPOT_RING;
        out[k] = s_ring[idx];
    }
    unlock();
    return n;
}

int wspr_spots_count(void)
{
    if (!s_ring) return 0;
    if (!lock()) return 0;
    int n = s_count;
    unlock();
    return n;
}

int wspr_spots_unique_calls(void)
{
    if (!s_ring) return 0;
    if (!lock()) return 0;
    /* O(n^2) over at most 256 entries, called at most once a second from a UI
     * timer. Deliberately not a hash set: the constant factor of a set would
     * cost more than the 32 K comparisons this can ever do, and it would need
     * somewhere to live. */
    int unique = 0;
    for (int a = 0; a < s_count; a++) {
        int ia = (s_head - 1 - a + WSPR_SPOT_RING * 2) % WSPR_SPOT_RING;
        bool seen = false;
        for (int b = 0; b < a && !seen; b++) {
            int ib = (s_head - 1 - b + WSPR_SPOT_RING * 2) % WSPR_SPOT_RING;
            if (strcmp(s_ring[ia].call, s_ring[ib].call) == 0) seen = true;
        }
        if (!seen) unique++;
    }
    unlock();
    return unique;
}

void wspr_spots_clear(void)
{
    if (!s_ring) return;
    if (!lock()) return;
    s_count = s_head = 0;
    unlock();
}
