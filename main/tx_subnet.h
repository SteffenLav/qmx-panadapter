#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/* DO NOT DELETE THE TX INTERLOCK - T206. REFUSE KEYING FOR A REQUEST THAT DID
 * NOT ARRIVE FROM THIS SUBNET.
 *
 * WHY IT EXISTS. The web API can transmit: /api/cmd reaches the radio through
 * cat_send_external_cmd(), and docs/unattended-tx-inventory.md counts the
 * arming sites behind it. The server is plain HTTP with no authentication of
 * any kind - no Authorization handling, no key, no token, no session. So the
 * access model today is "trusted LAN", and nothing enforces the LAN half of
 * it. Anyone who port-forwards the Tab5, or whose router does it for them, has
 * published a transmitter.
 *
 * SAME SHAPE AS THE BAND GUARD (tx_band_guard.h, T48): a door that refuses,
 * not a caller we trust. T48 exists because nothing checked and that shipped;
 * this is the same lesson applied to who is asking rather than to what they
 * ask for.
 *
 * WARNING - IT IS A SECOND LOCK, NOT A LOGIN, AND MUST NEVER BE WORDED AS ONE.
 * "On my LAN" is a weak identity. It stops the internet. It does NOT stop a
 * guest on the WiFi, a visitor's phone on the hotspot, or anybody holding the
 * WiFi key - every one of those is on the subnet and passes. Remote access
 * that needs real identity belongs at the network layer, in a tunnel
 * (WireGuard/Tailscale) or a TLS reverse proxy, because over plain HTTP a
 * password crosses in clear and a session token can be REPLAYED to key a
 * transmitter. A half-built auth layer on a device that transmits fails
 * towards "someone else keys your radio", which is worse than no remote
 * access at all. The tunnel is the first lock. This is the second.
 *
 * THE PEER ADDRESS COMES FROM THE SOCKET, NEVER FROM A HEADER.
 * X-Forwarded-For, X-Real-IP, Forwarded and the rest are supplied by the
 * CALLER - which is the thing being checked. Trusting one would let any
 * request claim to be local by typing a header, so the interlock would refuse
 * exactly nobody. getpeername() on the request's own socket is the only
 * address here that cannot be chosen by the sender. If a reverse proxy is ever
 * put in front of the Tab5 on purpose, the proxy is then the peer and it IS
 * local - which is the correct answer, because the proxy is where the login
 * lives.
 *
 * DEFAULT OFF. settings: tx_subnet_interlock, stored POSITIVE (true =
 * enforce), the opposite convention to tx_band_guard_off - see settings.h for
 * why. Two reasons it must not default on:
 *   - Jim KC9KKO's QMX-PanadapterShared fork exists specifically to drive this
 *     firmware's radio across a network. Defaulting on would silently remove
 *     TX from his users on upgrade, with no message they would connect to the
 *     change.
 *   - a deliberate remote station behind a tunnel legitimately arrives from
 *     outside the subnet.
 * DECIDED BY THE OPERATOR, 2026-11-18: it stays OFF. Opt-in, per device. Do not
 * flip the default, and do not re-open the question.
 */

/* May the request on this socket key the radio?
 *
 * sockfd: the request's own socket. A NEGATIVE fd is a REFUSAL when the
 *         interlock is on - not knowing who is asking is the case this exists
 *         for, and the band guard's own door refuses an unknown dial for the
 *         same reason.
 * what:   "RTTY TX", "Antenna Tune" etc, for the log and the refusal text.
 * err/err_len: optional; on a refusal, the sentence to send back to the
 *         caller. REPORT IT ON THE CHANNEL THE CALLER USED - a refusal only in
 *         the log is invisible to the browser that was refused (T94).
 *
 * Returns true when keying may proceed. Always true while the setting is off.
 */
bool tx_subnet_allows_fd(int sockfd, const char *what, char *err, size_t err_len);

/* The same decision for an httpd request. Resolves the socket with
 * httpd_req_to_sockfd() - the request's real peer, not anything it said. */
bool tx_subnet_allows_req(httpd_req_t *req, const char *what,
                          char *err, size_t err_len);
