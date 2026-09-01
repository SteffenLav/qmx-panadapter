#include "net_guard.h"

#include <string.h>

bool net_ipv4_parse(const char *s, uint32_t *out)
{
    if (!s || !out) return false;

    uint32_t v = 0;
    int octets = 0;

    for (;;) {
        // Each octet: 1-3 digits, no leading '+'/'-', no empty field.
        // Leading zeros are REJECTED rather than accepted as decimal: "010" is
        // octal to some resolvers and decimal to others, and a value that means
        // two different things to two pieces of software has no place in a
        // security decision.
        if (*s < '0' || *s > '9') return false;
        int digits = 0, val = 0;
        bool leading_zero = (*s == '0');
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
            if (++digits > 3) return false;
        }
        if (leading_zero && digits > 1) return false;
        if (val > 255) return false;
        v = (v << 8) | (uint32_t)val;
        if (++octets == 4) break;
        if (*s != '.') return false;
        s++;
    }
    if (*s != '\0') return false;   // trailing junk, e.g. "1.2.3.4x"

    *out = v;
    return true;
}

bool net_same_subnet(uint32_t a, uint32_t b, uint32_t netmask)
{
    return (a & netmask) == (b & netmask);
}

bool net_plaintext_allowed(uint32_t target_ip, uint32_t our_ip, uint32_t netmask)
{
    // Every "we don't know" answers NO. In particular a zero netmask makes
    // net_same_subnet() true for every address on earth, so it must be rejected
    // BEFORE the comparison, not left to it.
    if (our_ip == 0 || target_ip == 0) return false;
    if (netmask == 0) return false;

    return net_same_subnet(target_ip, our_ip, netmask);
}

bool net_url_parse(const char *url, net_scheme_t *scheme_out,
                   char *host_out, size_t host_sz, uint16_t *port_out)
{
    if (!url || !host_out || host_sz == 0) return false;
    if (scheme_out) *scheme_out = NET_SCHEME_UNKNOWN;
    if (port_out)   *port_out   = 0;
    host_out[0] = '\0';

    net_scheme_t scheme;
    const char *p;
    if (strncmp(url, "https://", 8) == 0)     { scheme = NET_SCHEME_HTTPS; p = url + 8; }
    else if (strncmp(url, "http://", 7) == 0) { scheme = NET_SCHEME_HTTP;  p = url + 7; }
    else return false;   // no scheme, or a scheme we will not speak

    // Reject credentials in the authority ("http://user:pw@host"). We have no
    // use for them, and silently dropping the part before '@' would change which
    // host is contacted versus what the operator believes they typed.
    for (const char *q = p; *q && *q != '/'; q++)
        if (*q == '@') return false;

    // Host ends at the first ':' (port), '/' (path) or end of string.
    size_t n = 0;
    while (p[n] && p[n] != ':' && p[n] != '/') n++;
    if (n == 0 || n >= host_sz) return false;
    memcpy(host_out, p, n);
    host_out[n] = '\0';

    if (p[n] == ':') {
        const char *ps = p + n + 1;
        if (*ps < '0' || *ps > '9') return false;
        uint32_t port = 0;
        while (*ps >= '0' && *ps <= '9') {
            port = port * 10 + (uint32_t)(*ps - '0');
            if (port > 65535) return false;
            ps++;
        }
        if (*ps != '\0' && *ps != '/') return false;
        if (port == 0) return false;
        if (port_out) *port_out = (uint16_t)port;
    }

    if (scheme_out) *scheme_out = scheme;
    return true;
}

uint32_t net_ipv4_from_network_order(uint32_t net_order)
{
    // Read the four octets out in wire order, then rebuild them the way
    // net_ipv4_parse() does. Deliberately byte-wise rather than a bit-shuffle or
    // ntohl(): this is portable to the host harness, where ntohl() would depend
    // on the test machine's own endianness and could hide the very bug it is
    // here to prevent.
    const uint8_t *b = (const uint8_t *)&net_order;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}
