// sock_probe - how many LWIP sockets are left.
//
// ⛔ WRITTEN BECAUSE #313 IS INVISIBLE WHILE IT IS HAPPENING. On 2026-09-01 the
// Tab5 stopped answering on port 80 AND port 4532 while `ping` replied, WiFi
// logged "online" every 30 s, CAT kept polling and FT8 kept decoding at ~48,000
// pairs/s. It stayed that way for SIX HOURS and cleared only on a reboot. Two
// INDEPENDENT listeners died together with the network stack alive underneath
// them, which is the signature of the socket table being full - and nothing in
// the firmware could see that, so the whole event produced no evidence at all.
//
// The six-hour persistence is itself a measurement: `CONFIG_LWIP_TCP_MSL` is
// 10 s, so TIME_WAIT cannot hold a socket for more than a few tens of seconds.
// Whatever held them was not waiting to be released.
//
// ⚠ The arithmetic that made this suspicious is in webserver.c's own comment:
// "LWIP_MAX_SOCKETS is 16; httpd reserves 3, so up to 13 sessions are safe."
// 13 + 3 = 16, i.e. the ENTIRE table, which is only "safe" if httpd is the only
// user. It is not - rigctld's listener on 4532, mDNS, SNTP and every outbound
// feed (RBN, DX cluster, POTA, SOTA, PSK Reporter, wsprnet, the logbook uploads,
// OTA) draw on the same 16.
//
// ⛔ BUT THAT IS A HYPOTHESIS, NOT THE DIAGNOSIS. Saturating httpd with 15
// concurrent connections from a PC was tried on 2026-09-01 and port 4532 stayed
// OPEN throughout, so simple saturation does NOT reproduce it. This file exists
// to make the next occurrence measurable rather than to prove that theory.
//
// Cost: one socket() + one close() per call. No heap walk, no interrupts-off
// window, so it is safe on a periodic path - unlike the largest-free-block query
// the heap watchdog had to stop doing (the FT4 cyan flash).

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True when a single TCP socket cannot be allocated - the table is full and
// every listener on the device has stopped accepting. One socket() + close().
bool sock_probe_exhausted(void);

// How many sockets can still be allocated, up to `cap`. Opens that many, counts,
// and closes them all again.
//
// ⚠ Briefly HOLDS the sockets it is counting, so a large cap momentarily starves
// the very thing being measured. On demand only - never on a periodic path.
int sock_probe_free(int cap);

/* Same figure, but at most one real probe per `max_age_ms`; in between it
 * returns the last one taken.
 *
 * ⛔ USE THIS FROM ANYTHING POLLED. sock_probe_free() holds up to `cap` sockets
 * AT ONCE, and /api/status called it with cap=6 on every request - a browser
 * polls that at 1 Hz, and this bench idles with FIVE sockets free, so every
 * poll took the entire remaining table for the duration of the probe. Anything
 * arriving in that window gets ENFILE, which is the exact #313 signature
 * (`httpd_accept_conn: error in accept (23)`).
 *
 * Whether that is all of #313 is NOT established - the reproduction ran for
 * 10 h before failing and then failed continuously, which a momentary window
 * does not obviously explain. But a diagnostic must not consume the resource
 * it is measuring, which is the same rule task_stacks.h states for the heap. */
int sock_probe_free_cached(int cap, int max_age_ms);

// Called from the 10 s heap watchdog. Runs the cheap one-socket canary and logs
// ONLY on a change of state, so a healthy device stays silent and an exhausted
// one says so once, loudly, in the diag log that a field report carries.
void sock_probe_report(void);

#ifdef __cplusplus
}
#endif
