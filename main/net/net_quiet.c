// See net_quiet.h.

#include "net_quiet.h"
#include "esp_log.h"

static volatile bool s_quiet = false;

void net_quiet_set(bool quiet)
{
    if (s_quiet == quiet) return;
    s_quiet = quiet;
    ESP_LOGW("net_quiet", "%s starting new network work", quiet ? "HOLDING" : "resuming");
}

bool net_quiet_active(void) { return s_quiet; }
