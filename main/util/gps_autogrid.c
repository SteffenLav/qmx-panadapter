#include "gps_autogrid.h"

#include <string.h>

void gps_autogrid_init(gps_autogrid_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
}

/* Maidenhead is conventionally FIELD upper, SQUARE digits, SUBSQUARE lower,
 * but a hand-typed grid may be all upper or all lower. Comparing case-
 * sensitively would make "JO65MR" and "JO65mr" different squares and rewrite
 * the setting every minute forever. */
static bool same_grid(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t i = 0;
    for (; a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return a[i] == 0 && b[i] == 0;
}

bool gps_autogrid_step(gps_autogrid_t *st, const char *gps_grid,
                       const char *stored_grid)
{
    if (!st) return false;

    /* No fix: forget what we were counting. A grid seen for 50 s before the
     * fix dropped must not be accepted on the strength of one reading after
     * it comes back somewhere else. */
    if (!gps_grid || !gps_grid[0]) {
        st->last[0] = 0;
        st->stable_s = 0;
        return false;
    }

    if (!same_grid(st->last, gps_grid)) {
        size_t i = 0;
        for (; gps_grid[i] && i + 1 < sizeof(st->last); i++) st->last[i] = gps_grid[i];
        st->last[i] = 0;
        st->stable_s = 1;
        return false;
    }

    /* The call that first saw this grid counted as second 1, so advance and
     * THEN compare - otherwise the Nth call is still one short and the grid
     * is accepted a second late. Caught by the harness, which asserts the
     * fire happens on call GPS_AUTOGRID_STABLE_S exactly. */
    if (st->stable_s < GPS_AUTOGRID_STABLE_S) st->stable_s++;
    if (st->stable_s < GPS_AUTOGRID_STABLE_S) return false;

    /* Stable. Write it only if it says something different from what is
     * stored - including the case where nothing is stored at all. */
    return !same_grid(stored_grid ? stored_grid : "", gps_grid);
}
