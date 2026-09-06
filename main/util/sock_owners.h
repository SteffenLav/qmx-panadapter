// sock_owners - WHO is holding the LWIP socket table.
//
// ⛔ WRITTEN BECAUSE #313 WAS DIAGNOSED AS FAR AS "THE TABLE IS FULL" AND NO
// FURTHER. sock_probe.c answers "is there a socket left"; it cannot answer
// "which of the sixteen are gone, and to whom", and without that the leak can
// only be guessed at file by file.
//
// ⭐ The overnight capture of 2026-09-06 shows why that second question is the
// one that matters. LWIP's alloc_socket() hands out the LOWEST free index, so
// the fd the web UI's WebSocket lands on is a direct readout of how many lower
// slots are permanently held. Over that night it went 52 -> 54 -> 57 and never
// came back down (LWIP_SOCKET_OFFSET is 48 here, so index 4 -> 6 -> 9), while
// the table is 16 deep. Headroom fell from 12 to 6, and the first burst of
// three concurrent TLS feed fetches then exhausted it: ENFILE out of
// httpd_accept_conn, the web server stops answering, and ping/CAT/FT8 all keep
// working so nothing else looks wrong.
//
// Two of those five went at a single WiFi drop (t=8878 s: httpd stopped and
// restarted, RBN and the DX cluster both reconnected). The other three went in
// a burst of WebSocket reconnects with no WiFi event at all. That is as far as
// a log without owner information can take it - hence this file.
//
// Cost: getsockname/getpeername/getsockopt per fd. No allocation, no blocking,
// no interrupts-off window, and - the rule sock_probe.h states - it does NOT
// consume the resource it measures. It never opens a socket.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log one line per socket in the table: index, type, local and peer address.
// `why` is echoed in the header so the reason it fired is in the log next to
// it ("exhausted", "low", "on demand").
void sock_owners_report(const char *why);

// Cheap tick for a periodic path. Counts the table and reports the FULL owner
// list only when the number in use sets a new high for this boot - so a healthy
// device is silent, a burst says so once, and a leak announces itself one step
// at a time with the holders named. Bounded by construction: it can fire at
// most CONFIG_LWIP_MAX_SOCKETS times in a session.
void sock_owners_highwater_check(void);

// Same walk, into a caller's buffer, newline-separated. Returns the number of
// ALLOCATED sockets found (not the bytes written).
int sock_owners_dump(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
