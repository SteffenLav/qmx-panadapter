#include "hosted_watchdog.h"

hosted_wd_action_t hosted_wd_tick(hosted_wd_t *w, bool watching, bool probe_ok)
{
    if (!w) return HOSTED_WD_NOTHING;

    // Nothing to conclude from a probe we are not entitled to make.
    if (!watching) { w->fail_streak = 0; return HOSTED_WD_NOTHING; }

    if (probe_ok) {
        if (w->fail_streak) {
            w->last_missed  = w->fail_streak;
            w->fail_streak  = 0;
            return HOSTED_WD_RECOVERED;
        }
        return HOSTED_WD_NOTHING;
    }

    if (++w->fail_streak < HOSTED_WD_FAILS_BEFORE_RELINK) return HOSTED_WD_NOTHING;

    // The streak resets whether or not we act, so an exhausted watchdog does not
    // re-announce itself every 30 s - it waits out another full streak first.
    w->fail_streak = 0;

    if (w->relink_count >= HOSTED_WD_MAX_RELINKS) return HOSTED_WD_EXHAUSTED;
    w->relink_count++;
    return HOSTED_WD_RELINK;
}
