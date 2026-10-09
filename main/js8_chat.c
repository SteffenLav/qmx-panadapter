#include "js8_chat.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "js8_chat";
#endif

/* The ring, newest at s_head - 1. Kept as a plain array rather than a list:
 * the page walks it backwards every repaint and nothing ever removes from the
 * middle. */
typedef struct {
    js8_chat_msg_t msg[JS8_CHAT_MAX_MSGS];
    int            n;      /* messages held, <= JS8_CHAT_MAX_MSGS */
    int            head;   /* next slot to write */
} js8_chat_store_t;

static js8_chat_store_t *s_store;

bool js8_chat_init(void)
{
    if (s_store) return true;
#if defined(ESP_PLATFORM)
    s_store = heap_caps_calloc(1, sizeof(*s_store), MALLOC_CAP_SPIRAM);
    if (!s_store) {
        ESP_LOGE(TAG, "no PSRAM for the JS8 chat store (%u B) - the page will "
                      "be empty, decoding is unaffected",
                 (unsigned)sizeof(*s_store));
        return false;
    }
#else
    s_store = calloc(1, sizeof(*s_store));
    if (!s_store) return false;
#endif
    return true;
}

void js8_chat_clear(void)
{
    if (!s_store) return;
    memset(s_store, 0, sizeof(*s_store));
}

/* Index into msg[] of the i-th newest, or -1. */
static int idx_newest(int i)
{
    if (!s_store || i < 0 || i >= s_store->n) return -1;
    int k = s_store->head - 1 - i;
    while (k < 0) k += JS8_CHAT_MAX_MSGS;
    return k;
}

static bool same_run(const js8_chat_msg_t *a, const js8_chat_msg_t *b)
{
    /* The audio offset of a run drifts by a few Hz between slots - the
     * candidate search does not land on the same bin every time - so the
     * frequency is matched with the same tolerance js8_reasm uses rather than
     * exactly. Without it a growing message becomes several messages. */
    const int TOL_HZ = 15;
    int d = a->freq_hz - b->freq_hz;
    if (d < 0) d = -d;
    return a->first_utc == b->first_utc && d <= TOL_HZ;
}

void js8_chat_add(const js8_chat_msg_t *m)
{
    if (!s_store || !m) return;

    /* Update in place if this run is already held. Walk newest first: a run
     * being extended was almost certainly the last thing written. */
    for (int i = 0; i < s_store->n; i++) {
        int k = idx_newest(i);
        if (k < 0) break;
        if (s_store->msg[k].kind != m->kind) continue;
        if (!same_run(&s_store->msg[k], m)) continue;
        s_store->msg[k] = *m;
        return;
    }

    s_store->msg[s_store->head] = *m;
    s_store->head = (s_store->head + 1) % JS8_CHAT_MAX_MSGS;
    if (s_store->n < JS8_CHAT_MAX_MSGS) s_store->n++;
}

void js8_chat_tick(int64_t now_utc, int quiet_s)
{
    if (!s_store) return;
    for (int i = 0; i < s_store->n; i++) {
        int k = idx_newest(i);
        if (k < 0) break;
        if (!s_store->msg[k].active) continue;
        if (now_utc - s_store->msg[k].last_utc >= quiet_s)
            s_store->msg[k].active = false;
    }
}

int js8_chat_count(void)
{
    return s_store ? s_store->n : 0;
}

bool js8_chat_at(int i, js8_chat_msg_t *out)
{
    int k = idx_newest(i);
    if (k < 0 || !out) return false;
    *out = s_store->msg[k];
    return true;
}
