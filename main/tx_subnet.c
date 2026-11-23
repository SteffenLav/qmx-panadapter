#include "tx_subnet.h"

#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"
#include "esp_log.h"

#include "storage/settings.h"
#include "util/net_guard.h"
#include "util/net_ifaddr.h"

static const char *TAG = "tx_subnet";

/* -1 not yet seen, 0 off, 1 on. Logged on CHANGE rather than once per boot, so
 * a toggle made after the first transmit is in the capture too. */
static int s_state_logged = -1;

/* One sentence for every refusal, because the operator and the user who hits
 * it have to be able to connect the two. It says what to do about it, and it
 * deliberately does NOT claim that anything was authenticated. */
static void refusal_text(char *err, size_t err_len, const char *what,
                         const char *peer)
{
    if (!err || !err_len) return;
    snprintf(err, err_len,
             "%s was refused: this request came from %s, which is not on the "
             "Tab5's own network. Reach the Tab5 through a VPN or tunnel, or "
             "turn the TX interlock off in Settings.",
             what ? what : "Transmit", peer);
}

static void fmt_ipv4(char *out, size_t n, uint32_t host_order)
{
    snprintf(out, n, "%u.%u.%u.%u",
             (unsigned)((host_order >> 24) & 0xFFu),
             (unsigned)((host_order >> 16) & 0xFFu),
             (unsigned)((host_order >> 8)  & 0xFFu),
             (unsigned)( host_order        & 0xFFu));
}

bool tx_subnet_allows_fd(int sockfd, const char *what, char *err, size_t err_len)
{
    if (err && err_len) err[0] = 0;

    /* ⛔ EVERY DECISION LEAVES A LINE, INCLUDING THE ONES THAT ALLOW.
     *
     * This used to be silent whenever it said yes, and that cost a diagnosis.
     * Randy N4OPI reported TX going out from off-subnet on v2.1.0 (2026-11-23)
     * with the interlock ticked. His diag holds the keying - "web requested CQ
     * start", key down, 3.0 W - and not one word about this decision. Three
     * explanations fitted that log equally well:
     *   - the setting was not actually on at the moment of the request;
     *   - a router between his two segments NATs, so the peer we see is the
     *     router's own on-subnet address and the interlock allows it correctly;
     *   - the guard is broken.
     * Nothing in the capture separated them. A guard that only speaks when it
     * refuses is an instrument that reads the same whether it is working or
     * switched off - the same fault as a silent capture (see the serial capture
     * rules). It now says what it decided, who asked, and against what.
     *
     * Off is still read first, so the mechanism costs nothing else on a device
     * that is not using it. */
    const bool on = settings_get_tx_subnet_interlock();
    if (s_state_logged != (int)on) {
        s_state_logged = (int)on;
        ESP_LOGW(TAG, "TX interlock is %s",
                 on ? "ON - off-subnet requests may not key"
                    : "OFF - any caller that reaches the API may key");
    }
    if (!on) {
        ESP_LOGI(TAG, "%s allowed: TX interlock is off", what ? what : "TX");
        return true;
    }

    if (sockfd < 0) {
        ESP_LOGW(TAG, "%s REFUSED: no socket for this request", what ? what : "TX");
        refusal_text(err, err_len, what, "an unknown address");
        return false;
    }

    /* getpeername(), NOT a header. The address a caller supplies is worth
     * nothing when the caller is the thing being checked. */
    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &plen) != 0) {
        ESP_LOGW(TAG, "%s REFUSED: getpeername(fd=%d) failed", what ? what : "TX", sockfd);
        refusal_text(err, err_len, what, "an unknown address");
        return false;
    }

    uint32_t peer_ip = 0;
    char peer_s[46];
    snprintf(peer_s, sizeof(peer_s), "an unknown address");
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)&peer;
        peer_ip = net_ipv4_from_network_order(in->sin_addr.s_addr);
        fmt_ipv4(peer_s, sizeof(peer_s), peer_ip);
    } else if (peer.ss_family == AF_INET6) {
        /* httpd binds a v6 listener when LWIP_IPV6 is on, and a v4 client then
         * arrives as ::ffff:a.b.c.d. Pull the embedded v4 out. A REAL v6 peer
         * has no address that can be compared against an IPv4 netmask, so it
         * is refused - the safe answer and an honest one, because this
         * interlock only understands IPv4 subnets. */
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&peer;
        const uint8_t *b = (const uint8_t *)&in6->sin6_addr;
        static const uint8_t v4map[12] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF};
        if (memcmp(b, v4map, sizeof(v4map)) == 0) {
            peer_ip = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                      ((uint32_t)b[14] << 8)  |  (uint32_t)b[15];
            fmt_ipv4(peer_s, sizeof(peer_s), peer_ip);
        } else {
            ESP_LOGW(TAG, "%s REFUSED: IPv6 peer, no IPv4 subnet to compare it with",
                     what ? what : "TX");
            refusal_text(err, err_len, what, "an IPv6 address");
            return false;
        }
    } else {
        ESP_LOGW(TAG, "%s REFUSED: peer family %d", what ? what : "TX", (int)peer.ss_family);
        refusal_text(err, err_len, what, "an unknown address");
        return false;
    }

    uint32_t ours = 0, mask = 0;
    if (!net_own_ipv4(&ours, &mask)) {
        /* No address of our own means no subnet to compare, so there is no
         * answer available except no. It also means WiFi is down, in which
         * case nothing arrived over it anyway. */
        ESP_LOGW(TAG, "%s REFUSED: our own address is not known", what ? what : "TX");
        refusal_text(err, err_len, what, peer_s);
        return false;
    }

    if (!net_tx_peer_allowed(peer_ip, ours, mask)) {
        char ours_s[20], mask_s[20];
        fmt_ipv4(ours_s, sizeof(ours_s), ours);
        fmt_ipv4(mask_s, sizeof(mask_s), mask);
        ESP_LOGW(TAG, "%s REFUSED by the TX interlock: peer %s is off-subnet "
                      "(ours %s mask %s)",
                 what ? what : "TX", peer_s, ours_s, mask_s);
        refusal_text(err, err_len, what, peer_s);
        return false;
    }

    {
        /* The line that was missing from Randy's capture. It is W, beside the
         * refusals, because a filtered log that keeps only the refusals cannot
         * answer "did this request reach the guard at all?". */
        char ours_s[20], mask_s[20];
        fmt_ipv4(ours_s, sizeof ours_s, ours);
        fmt_ipv4(mask_s, sizeof mask_s, mask);
        ESP_LOGW(TAG, "%s ALLOWED by the TX interlock: peer %s is on-subnet "
                      "(ours %s mask %s)",
                 what ? what : "TX", peer_s, ours_s, mask_s);
    }
    return true;
}

bool tx_subnet_allows_req(httpd_req_t *req, const char *what,
                          char *err, size_t err_len)
{
    /* httpd_req_to_sockfd() returns the fd of the connection this request
     * arrived on. A missing req, or one httpd cannot map to a socket, falls
     * into the negative-fd refusal above. */
    return tx_subnet_allows_fd(req ? httpd_req_to_sockfd(req) : -1,
                               what, err, err_len);
}
