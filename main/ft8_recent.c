#include "ft8_recent.h"

#include <string.h>
#include <stdio.h>

typedef struct {
    char    call[16];
    char    band[8];
    int64_t ts;
} recent_t;

static recent_t s_recent[FT8_RECENT_MAX];
static int      s_head;

void ft8_recent_reset(void)
{
    memset(s_recent, 0, sizeof(s_recent));
    s_head = 0;
}

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void ft8_recent_note(const char *call, const char *band, int64_t now)
{
    if (!call || !call[0] || !band) return;

    // Refresh an existing entry. Without this, a station worked repeatedly
    // would fill the whole ring on its own and evict everyone else.
    for (int i = 0; i < FT8_RECENT_MAX; i++) {
        if (s_recent[i].call[0] &&
            strcmp(s_recent[i].call, call) == 0 &&
            strcmp(s_recent[i].band, band) == 0) {
            s_recent[i].ts = now;
            return;
        }
    }

    int i = s_head;
    s_head = (s_head + 1) % FT8_RECENT_MAX;
    copy_bounded(s_recent[i].call, sizeof(s_recent[i].call), call);
    copy_bounded(s_recent[i].band, sizeof(s_recent[i].band), band);
    s_recent[i].ts = now;
}

bool ft8_recent_worked(const char *call, const char *band, int64_t now)
{
    if (!call || !call[0] || !band) return false;
    if (now < FT8_RECENT_EPOCH_MIN)  return false;   // clock not set - no grace

    for (int i = 0; i < FT8_RECENT_MAX; i++) {
        if (!s_recent[i].call[0]) continue;
        if (strcmp(s_recent[i].call, call) != 0) continue;
        if (strcmp(s_recent[i].band, band) != 0) continue;
        int64_t age = now - s_recent[i].ts;
        // age < 0 means the clock stepped backwards (an SNTP or FT8-derived
        // correction can do that). Treat it as "not recent" rather than as a
        // huge positive age, which is what an unsigned comparison would give.
        if (age >= 0 && age < FT8_RECENT_GRACE_SEC) return true;
    }
    return false;
}
