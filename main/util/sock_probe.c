#include "sock_probe.h"

#include "esp_timer.h"

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include "esp_log.h"

static const char *TAG = "sock";

#define SOCK_PROBE_CAP_MAX 24   // LWIP_MAX_SOCKETS is 16; a little room above it

bool sock_probe_exhausted(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return true;
    close(fd);
    return false;
}

int sock_probe_free(int cap)
{
    if (cap < 1) return 0;
    if (cap > SOCK_PROBE_CAP_MAX) cap = SOCK_PROBE_CAP_MAX;

    int fds[SOCK_PROBE_CAP_MAX];
    int n = 0;
    while (n < cap) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        fds[n++] = fd;
    }
    // Give every one of them back, including on the early-exit path - a probe
    // that leaked would BE the fault it is looking for.
    for (int i = 0; i < n; i++) close(fds[i]);
    return n;
}

void sock_probe_report(void)
{
    static bool s_was_exhausted = false;
    bool now = sock_probe_exhausted();

    if (now == s_was_exhausted) return;   // change-detected: silence when healthy
    s_was_exhausted = now;

    if (now) {
        // The line #313 needed and did not have. Everything else on the device
        // keeps working in this state - ping answers, WiFi says online, CAT
        // polls, FT8 decodes - so without this the operator sees "the web UI
        // stopped" and the log shows nothing whatsoever.
        ESP_LOGE(TAG, "LWIP SOCKET TABLE EXHAUSTED - no socket could be "
                      "allocated. Every listener (web server on 80, rigctld on "
                      "4532) has stopped accepting; ping and the radio are "
                      "unaffected. This is #313.");
    } else {
        ESP_LOGW(TAG, "LWIP socket table recovered - sockets available again");
    }
}

/* See the header: the point is the RATE, not the probe. */
int sock_probe_free_cached(int cap, int max_age_ms)
{
    static int64_t last_us;
    static int     last_val = -1;

    int64_t now = esp_timer_get_time();
    if (last_val >= 0 && (now - last_us) < (int64_t)max_age_ms * 1000)
        return last_val;

    last_val = sock_probe_free(cap);
    last_us  = now;
    return last_val;
}
