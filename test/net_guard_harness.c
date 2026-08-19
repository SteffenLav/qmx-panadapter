/* Host test for net_guard - the rule deciding whether a logbook upload may go
 * out in the CLEAR (#171, Mark G4MEM's self-hosted Cloudlog).
 *
 * Build (from the repo root):
 *   gcc -I main/util -o net_guard_harness test/net_guard_harness.c \
 *       main/util/net_guard.c && ./net_guard_harness
 *
 * Why this exists: this is the first time the firmware sends credentials to an
 * address the OPERATOR supplies, and the whole safety argument rests on one
 * masked comparison plus its refusals. Getting it wrong does not crash anything
 * and does not show up on the screen - it silently posts an API key in plain
 * text to a network the operator does not control. That is exactly the class of
 * bug that has to be caught here rather than in the field.
 *
 * It links the REAL functions, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "net_guard.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* Dotted quad -> uint32, for readable test data. Deliberately a SEPARATE
 * implementation from net_ipv4_parse() so the tests do not depend on the
 * function they are testing. */
static uint32_t ip(int a, int b, int c, int d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static void test_same_subnet(void)
{
    printf("net_same_subnet:\n");
    uint32_t m24 = ip(255,255,255,0);
    CHECK(net_same_subnet(ip(192,168,1,10), ip(192,168,1,50), m24), "same /24 should match\n");
    CHECK(!net_same_subnet(ip(192,168,1,10), ip(192,168,2,50), m24), "different /24 must not match\n");

    uint32_t m16 = ip(255,255,0,0);
    CHECK(net_same_subnet(ip(192,168,1,10), ip(192,168,99,50), m16), "same /16 should match\n");

    /* A /25 split: .10 and .200 share a /24 but NOT a /25. If this ever passes,
     * the mask is being ignored. */
    uint32_t m25 = ip(255,255,255,128);
    CHECK(!net_same_subnet(ip(192,168,1,10), ip(192,168,1,200), m25), "/25 halves must not match\n");
}

static void test_plaintext_allowed(void)
{
    printf("net_plaintext_allowed:\n");
    uint32_t m24 = ip(255,255,255,0);

    /* Mark's case: Cloudlog on his own LAN. */
    CHECK(net_plaintext_allowed(ip(192,168,1,20), ip(192,168,1,213), m24),
          "LAN target on our own subnet should be allowed\n");

    /* The case that must never be allowed: a public address. */
    CHECK(!net_plaintext_allowed(ip(93,184,216,34), ip(192,168,1,213), m24),
          "public target must be refused\n");

    /* Away from home: same private-looking address, different subnet. This is
     * the field-site scenario the per-upload re-check exists for. */
    CHECK(!net_plaintext_allowed(ip(192,168,1,20), ip(10,0,5,7), ip(255,0,0,0)),
          "target outside our current subnet must be refused\n");

    /* ⭐ THE DANGEROUS ONE. A zero netmask makes the masked comparison true for
     * EVERY address, so it has to be rejected before the comparison. Without
     * this guard the device would happily post credentials anywhere. */
    CHECK(!net_plaintext_allowed(ip(93,184,216,34), ip(192,168,1,213), 0),
          "zero netmask must refuse, not match the whole internet\n");

    /* "We don't know" must answer no, both directions. */
    CHECK(!net_plaintext_allowed(0, ip(192,168,1,213), m24), "unknown target must refuse\n");
    CHECK(!net_plaintext_allowed(ip(192,168,1,20), 0, m24),  "unknown own IP must refuse\n");

    /* ⚠ The two cases above pass even WITHOUT the explicit zero guards, because
     * the masked comparison happens to reject them anyway - a mutation run found
     * both tests passing for the wrong reason. These are the cases where the
     * guard is genuinely load-bearing: with our own IP still unset (0.0.0.0) and
     * a wide mask, a target that masks to zero compares EQUAL and would be sent
     * credentials in the clear. This is a real state - it is what the device
     * looks like before DHCP has answered. */
    CHECK(!net_plaintext_allowed(ip(0,1,2,3), 0, ip(255,0,0,0)),
          "target masking to zero must refuse while our IP is unset\n");
    CHECK(!net_plaintext_allowed(0, ip(0,0,0,9), ip(255,255,0,0)),
          "unknown target must refuse even when our IP also masks to zero\n");

    /* /32: only the address itself. */
    uint32_t m32 = 0xFFFFFFFFu;
    CHECK(net_plaintext_allowed(ip(192,168,1,213), ip(192,168,1,213), m32), "/32 self should match\n");
    CHECK(!net_plaintext_allowed(ip(192,168,1,20), ip(192,168,1,213), m32), "/32 other must refuse\n");
}

/* ⛔ THE TEST THAT WAS MISSING, and it is why v1.8.7 shipped a broken Cloudlog
   upload (Mark G4MEM). Everything else in this file builds BOTH sides of a
   comparison with ip() below, which produces host byte order - the same
   convention net_ipv4_parse() produces. So the suite compared a convention
   against itself and every case passed while the device failed.

   The defect lived at the BOUNDARY: esp_netif_get_ip_info() returns network byte
   order, and cloudlog_upload.c passed it straight into net_plaintext_allowed().
   Mutation testing could not find it either - net_guard.c was internally correct;
   the caller's conversion was the bug.

   These cases therefore start from the RAW BYTES esp_netif produces, laid out by
   hand, rather than from any helper of mine. */
static void test_network_byte_order(void)
{
    printf("N. network-order conversion (the v1.8.7 Cloudlog bug)\n");

    /* How lwip/esp_netif stores 192.168.1.8: byte 0 is the FIRST octet.
       Proven by esp_ip4_addr1(ipaddr) == esp_ip4_addr_get_byte(ipaddr, 0). */
    uint8_t wire[4] = { 192, 168, 1, 8 };
    uint32_t as_stored;
    memcpy(&as_stored, wire, 4);          /* exactly what the struct holds */

    CHECK(net_ipv4_from_network_order(as_stored) == ip(192,168,1,8),
          "network-order 192.168.1.8 must convert to the parsed form\n");

    uint8_t maskwire[4] = { 255, 255, 255, 0 };
    uint32_t mask_stored;
    memcpy(&mask_stored, maskwire, 4);
    CHECK(net_ipv4_from_network_order(mask_stored) == ip(255,255,255,0),
          "network-order netmask must convert to the parsed form\n");

    /* ⭐ The regression itself: Mark's exact setup. Server 192.168.1.8 on a
       device at 192.168.1.100/24. Converted, this must be allowed. */
    uint8_t ourwire[4] = { 192, 168, 1, 100 };
    uint32_t our_stored;
    memcpy(&our_stored, ourwire, 4);
    uint32_t target = 0;
    CHECK(net_ipv4_parse("192.168.1.8", &target), "parse target\n");
    CHECK(net_plaintext_allowed(target,
                                net_ipv4_from_network_order(our_stored),
                                net_ipv4_from_network_order(mask_stored)),
          "Mark's LAN server must be allowed once byte order is handled\n");

    /* And the failure as it actually shipped: feeding the raw stored values in
       without converting must NOT quietly appear to work. If this ever passes,
       the conversion has been dropped again. */
    CHECK(!net_plaintext_allowed(target, our_stored, mask_stored),
          "raw network-order values must NOT compare equal - that was the bug\n");
}

static void test_ipv4_parse(void)
{
    printf("net_ipv4_parse:\n");
    uint32_t v = 0;
    CHECK(net_ipv4_parse("192.168.1.20", &v) && v == ip(192,168,1,20), "plain dotted quad\n");
    CHECK(net_ipv4_parse("0.0.0.0", &v) && v == 0, "all zeros parses\n");
    CHECK(net_ipv4_parse("255.255.255.255", &v) && v == 0xFFFFFFFFu, "broadcast parses\n");

    CHECK(!net_ipv4_parse("cloudlog.local", &v), "hostname must not parse as an IP\n");
    CHECK(!net_ipv4_parse("192.168.1", &v),      "three octets must fail\n");
    CHECK(!net_ipv4_parse("192.168.1.2.3", &v),  "five octets must fail\n");
    CHECK(!net_ipv4_parse("192.168.1.256", &v),  "octet > 255 must fail\n");
    CHECK(!net_ipv4_parse("192.168.1.4x", &v),   "trailing junk must fail\n");
    CHECK(!net_ipv4_parse("192.168..4", &v),     "empty octet must fail\n");
    CHECK(!net_ipv4_parse(" 192.168.1.4", &v),   "leading space must fail\n");

    /* Leading zeros are ambiguous - octal to some resolvers, decimal to others.
     * A value meaning two different things to two pieces of software must not
     * feed a security decision. */
    CHECK(!net_ipv4_parse("192.168.001.4", &v), "leading zeros must be rejected as ambiguous\n");
    CHECK(!net_ipv4_parse("010.1.1.1", &v),     "octal-looking octet must be rejected\n");
}

static void test_url_parse(void)
{
    printf("net_url_parse:\n");
    char host[64];
    uint16_t port;
    net_scheme_t sc;

    CHECK(net_url_parse("http://192.168.1.20", &sc, host, sizeof(host), &port) &&
          sc == NET_SCHEME_HTTP && !strcmp(host, "192.168.1.20") && port == 0,
          "bare http host\n");

    CHECK(net_url_parse("https://log.example.com/", &sc, host, sizeof(host), &port) &&
          sc == NET_SCHEME_HTTPS && !strcmp(host, "log.example.com") && port == 0,
          "https with trailing slash\n");

    CHECK(net_url_parse("http://cloudlog.lan:8080/index.php", &sc, host, sizeof(host), &port) &&
          sc == NET_SCHEME_HTTP && !strcmp(host, "cloudlog.lan") && port == 8080,
          "explicit port with path\n");

    CHECK(!net_url_parse("ftp://x/", &sc, host, sizeof(host), &port), "unknown scheme must fail\n");
    CHECK(!net_url_parse("192.168.1.20", &sc, host, sizeof(host), &port), "scheme-less must fail\n");
    CHECK(!net_url_parse("http://", &sc, host, sizeof(host), &port), "empty host must fail\n");
    CHECK(!net_url_parse("http://host:0/", &sc, host, sizeof(host), &port), "port 0 must fail\n");
    CHECK(!net_url_parse("http://host:99999/", &sc, host, sizeof(host), &port), "port > 65535 must fail\n");
    CHECK(!net_url_parse("http://host:8o8/", &sc, host, sizeof(host), &port), "non-numeric port must fail\n");

    /* Credentials in the authority: dropping the part before '@' would change
     * WHICH host is contacted versus what the operator thinks they typed, so
     * the URL is refused instead. */
    CHECK(!net_url_parse("http://user:pw@evil.example.com/", &sc, host, sizeof(host), &port),
          "userinfo in authority must be refused\n");

    /* ⚠ The line above passes even WITHOUT the '@' check, because "pw@evil..."
     * fails the PORT parse instead - a mutation run caught it passing for the
     * wrong reason. This form has no colon, so nothing else rejects it: without
     * the guard the host silently becomes "user@evil.example.com". */
    CHECK(!net_url_parse("http://user@evil.example.com/", &sc, host, sizeof(host), &port),
          "userinfo without a password must be refused too\n");

    /* Buffer discipline: a host longer than the caller's buffer must fail
     * rather than truncate into a DIFFERENT hostname. */
    char tiny[8];
    CHECK(!net_url_parse("http://averylonghostname.example.com/", &sc, tiny, sizeof(tiny), &port),
          "over-long host must fail, not truncate\n");
}

int main(void)
{
    printf("net_guard harness\n\n");
    test_same_subnet();
    test_network_byte_order();
    test_plaintext_allowed();
    test_ipv4_parse();
    test_url_parse();
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
