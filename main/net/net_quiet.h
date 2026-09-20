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
// ⚠ 2026-09-20: RX audio was briefly wired to hold this too, then backed
// out the SAME session. Reason: this flag is a blunt "nothing new starts",
// and RX audio's actual requirement is narrower - only the feeds with a
// STANDING task/connection (SelfSpotter's MQTT client, RBN, DX cluster, PSK
// Reporter's "who's hearing me" query) meaningfully compete with it.
// POTA/SOTA and PSK Reporter's TX reports are periodic/batched with no
// standing cost between fetches, and the operator asked to keep those
// running once the Resource Management panel made the blanket rule
// visible. Those feeds now check rx_audio_is_enabled() directly instead
// (see audio/rx_audio.h) - this file went back to OTA-only, a single
// holder, bare bool in spirit even though the refcount plumbing below is
// left in place (harmless, and cheap insurance against a future second
// legitimate holder needing the same "don't clobber each other's hold"
// guarantee this briefly needed).
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
