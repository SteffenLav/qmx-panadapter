// See net_quiet.h.

#include "net_quiet.h"
#include "esp_log.h"

static volatile int s_holds = 0;

void net_quiet_hold(void)
{
    int n = ++s_holds;
    if (n == 1) ESP_LOGW("net_quiet", "HOLDING starting new network work");
}

void net_quiet_release(void)
{
    if (s_holds <= 0) {
        // A release with no matching hold is a bug in the CALLER, not here -
        // log it rather than going negative and reporting "active" wrong for
        // ever after a single stray call.
        ESP_LOGE("net_quiet", "release() with no outstanding hold - ignored");
        return;
    }
    int n = --s_holds;
    if (n == 0) ESP_LOGW("net_quiet", "resuming starting new network work");
}

bool net_quiet_active(void) { return s_holds > 0; }
