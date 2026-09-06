#include "sock_owners.h"

#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"

static const char *TAG = "sockown";

/* Named by the port, because "fd 53, peer 199.5.157.131:7000" is a puzzle and
 * "RBN" is an answer. Local port names a listener or a bound socket; peer port
 * names an outbound session. Anything unrecognised prints its number, so a new
 * feed added later is still legible - it just is not named yet. */
static const char *port_name(int port)
{
    switch (port) {
    case 80:   return "httpd";
    case 443:  return "TLS-feed";       /* POTA, SOTA, PSK Reporter query, uploads, OTA */
    case 4532: return "rigctld";
    case 4739: return "pskreporter";
    case 5353: return "mDNS";
    case 123:  return "SNTP";
    case 53:   return "DNS";
    case 7000: return "RBN-cw";
    case 7001: return "RBN-digi";
    case 8000: return "dxcluster";
    default:   return NULL;
    }
}

/* "192.168.1.209:80 (httpd)" - or "-" when the socket has no such address,
 * which is the normal answer for the peer end of a listener. */
static void fmt_addr(char *out, size_t n, const struct sockaddr_storage *sa, bool ok)
{
    if (!ok) { snprintf(out, n, "-"); return; }
    if (sa->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        char ip[16];
        inet_ntoa_r(in->sin_addr, ip, sizeof(ip));
        int port = ntohs(in->sin_port);
        const char *nm = port_name(port);
        if (nm) snprintf(out, n, "%s:%d(%s)", ip, port, nm);
        else    snprintf(out, n, "%s:%d", ip, port);
    } else {
        snprintf(out, n, "af%d", (int)sa->ss_family);
    }
}

/* One socket, one line. Returns false when the slot is free. */
static bool describe(int fd, char *line, size_t n)
{
    struct sockaddr_storage loc, rem;
    socklen_t ll = sizeof(loc), rl = sizeof(rem);

    /* getsockname is the allocation test: an unallocated index fails EBADF.
     * It allocates nothing, so the probe cannot perturb what it measures. */
    bool have_loc = (getsockname(fd, (struct sockaddr *)&loc, &ll) == 0);
    if (!have_loc) return false;

    bool have_rem = (getpeername(fd, (struct sockaddr *)&rem, &rl) == 0);

    int type = 0;
    socklen_t tl = sizeof(type);
    const char *tname = "?";
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0)
        tname = (type == SOCK_STREAM) ? "tcp" : (type == SOCK_DGRAM) ? "udp" : "raw";

    char ls[40], rs[40];
    fmt_addr(ls, sizeof(ls), &loc, true);
    fmt_addr(rs, sizeof(rs), &rem, have_rem);

    /* No peer on a TCP socket means it is listening (or was never connected) -
     * worth saying, since a leaked half-open session looks the same otherwise
     * apart from having a peer. */
    snprintf(line, n, "fd=%d idx=%d %s local=%s peer=%s%s",
             fd, fd - LWIP_SOCKET_OFFSET, tname, ls, rs,
             (!have_rem && type == SOCK_STREAM) ? " (listen)" : "");
    return true;
}

int sock_owners_dump(char *out, size_t out_sz)
{
    size_t used = 0;
    int n_used = 0;

    if (out && out_sz) out[0] = '\0';

    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++) {
        char line[160];
        if (!describe(LWIP_SOCKET_OFFSET + i, line, sizeof(line))) continue;
        n_used++;
        if (!out || out_sz == 0) continue;
        size_t l = strlen(line);
        if (used + l + 2 >= out_sz) continue;      /* truncate, never overrun */
        memcpy(out + used, line, l);
        used += l;
        out[used++] = '\n';
        out[used] = '\0';
    }
    return n_used;
}

void sock_owners_report(const char *why)
{
    /* ⛔ NO ARRAY OF LINES HERE. Sixteen 128-byte lines is 2 KB, and the caller
     * is the heap watchdog on a 3072-byte stack - the exact shape of the four
     * stack-protection faults CLAUDE.md records. One line at a time, 128 bytes,
     * and the table is walked twice so the count can lead. */
    int n_used = 0;
    char line[160];

    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++)
        if (describe(LWIP_SOCKET_OFFSET + i, line, sizeof(line))) n_used++;

    ESP_LOGW(TAG, "LWIP socket table (%s): %d of %d in use, %d free",
             why ? why : "on demand", n_used, CONFIG_LWIP_MAX_SOCKETS,
             CONFIG_LWIP_MAX_SOCKETS - n_used);

    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++)
        if (describe(LWIP_SOCKET_OFFSET + i, line, sizeof(line)))
            ESP_LOGW(TAG, "  %s", line);
}

void sock_owners_highwater_check(void)
{
    static int s_high = 0;

    int n_used = 0;
    char line[160];
    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++)
        if (describe(LWIP_SOCKET_OFFSET + i, line, sizeof(line))) n_used++;

    if (n_used <= s_high) return;
    s_high = n_used;

    /* ⚠ A new high is NOT by itself a leak - three feeds fetching at once is a
     * legitimate step. What distinguishes them is whether the step comes back
     * down, and only the owner list can say. A leaked socket keeps its peer
     * address; a busy one is gone by the next report. */
    sock_owners_report("new high-water");
}
