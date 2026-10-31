#include "sock_probe.h"
#include "sock_owners.h"

#include "esp_timer.h"
#include "wifi/wifi.h"   /* wifi_tcpip_stack_up - see the guard below */

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include "esp_log.h"

static const char *TAG = "sock";

#define SOCK_PROBE_CAP_MAX 24   // LWIP_MAX_SOCKETS is 16; a little room above it

/* ⛔ EVERY ENTRY POINT IN THIS FILE MUST ASK FIRST. socket() is not a
 * function that returns an error when LWIP is absent - it ABORTS:
 *     assert failed: tcpip_send_msg_wait_sem tcpip.c:454 (Invalid mbox)
 *
 * And LWIP really can be absent. wifi_task() returns before esp_netif_init()
 * when esp_hosted_init() fails, so a boot where the C6 does not answer has no
 * tcpip thread at all. CRASHED bench1 on 2026-10-31, v1.23.0-4-g85e8f5e7:
 * "SDIO card init did not signal within 6000 ms" at 14.7 s, and this probe -
 * which the heap watchdog runs every 10 s from heap_wd, with no network gate
 * of its own - took the board down at 18.7 s. One reboot and it came back,
 * because the fault is in the diagnostic, not in what it diagnoses.
 *
 * ⚠ A DIAGNOSTIC MUST NOT BE ABLE TO KILL THE DEVICE IT WATCHES. "No stack"
 * is reported as "nothing is wrong with the socket table", which is true: a
 * device with no LWIP has no socket table to exhaust. */
bool sock_probe_exhausted(void)
{
    if (!wifi_tcpip_stack_up()) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return true;
    close(fd);
    return false;
}

int sock_probe_free(int cap)
{
    if (cap < 1) return 0;
    if (!wifi_tcpip_stack_up()) return 0;   /* no stack, no sockets to count */
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

    /* ⛔ BEFORE sock_owners too, not only before the probe: it walks LWIP's
     * own socket table, which does not exist either when the stack never
     * started. The guard belongs at the top of the function, so a new call
     * added below it is covered by default rather than by remembering. */
    if (!wifi_tcpip_stack_up()) return;

    /* The leak is visible long before the wedge - see sock_owners.h for the
     * fd-creep evidence - so look every tick and speak only on a new high. */
    sock_owners_highwater_check();

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
        /* ⭐ And say WHO has them. The 2026-09-06 reproduction proved this line
         * alone is not enough: it fired four times over the night and the log
         * still could not name a single holder, so the leak stayed a guess.
         * The dump costs no socket, which is the one thing a probe here may
         * not do. */
        sock_owners_report("exhausted");
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
