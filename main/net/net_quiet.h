// net_quiet - "stop starting new network work for a moment".
//
// Originally built for the OTA verify. MEASURED across six downloads: internal
// heap at verify time tracks how LONG the download ran, not what else was
// computing. A 61 s download left 22.8 KB free / 6.9 KB largest and the verify
// passed; a ~345 s download left ~10 KB / 4 KB and esp_image_verify() wedged
// inside segment 0, taking the hardware watchdog. Healthy idle is 48.9 KB /
// 15.4 KB.
//
// What decays over those minutes is the periodic network work: the POTA fetch
// builds and tears down a full TLS session every ~70 s, PSK Reporter batches,
// and the RBN / DX-cluster loops reconnect. Each one churns internal heap while
// the OTA's own TLS session is already holding its share.
//
// So this flag says only: DO NOT START anything new. It deliberately does not
// close live sockets - tearing down RBN and the cluster mid-update would cost
// the operator their spot lane and buy back memory that was already allocated.
// Feeds check it where they would otherwise begin a fetch or a reconnect.
//
// ⭐ Second holder, 2026-09-20: rx_audio. The operator's own call - CW/SSB
// audio out of the Tab5's speaker is a field feature, and the background
// feeds (RBN, DX cluster, POTA/SOTA, PSK Reporter RX/self-spotting, WSPR
// self, QRZ coords, band conditions, the update-check fallback) are not part
// of that scenario, so they stand down for as long as RX audio is enabled
// and the room is theirs. Same 9 KB/1 KB-largest-block collapse this comment
// already measured for OTA turns out to be the STEADY-STATE with everything
// running at once - not just a download-time spike.
//
// REFCOUNTED, not a bare bool, because it now has two INDEPENDENT holders
// that must not be able to clobber each other: OTA holding it across a
// verify and the operator toggling RX audio off partway through must not
// silently let go of OTA's own hold (or vice versa, if RX audio is toggled
// while an OTA also happens to be running). Every net_quiet_hold() must be
// paired with exactly one net_quiet_release().
//
// ⚠ Advisory, not enforced. A feed that ignores it still works; it just keeps
// its share of the heap. Adding a new periodic network task? Check
// net_quiet_active() at the top of its loop - and if the flag can go active
// again AFTER a task has already opened a session, gate the CONTINUATION of
// that session too, not just its start (see pskr_self.c's own comment on
// this - the first version of this fix only prevented the START).

#pragma once
#include <stdbool.h>

void net_quiet_hold(void);      // one more reason to stay quiet
void net_quiet_release(void);   // one fewer reason - active() clears at 0
bool net_quiet_active(void);
