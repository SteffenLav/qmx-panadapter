// net_quiet - "stop starting new network work for a moment".
//
// Exists for the OTA verify. MEASURED across six downloads: internal heap at
// verify time tracks how LONG the download ran, not what else was computing.
// A 61 s download left 22.8 KB free / 6.9 KB largest and the verify passed; a
// ~345 s download left ~10 KB / 4 KB and esp_image_verify() wedged inside
// segment 0, taking the hardware watchdog. Healthy idle is 48.9 KB / 15.4 KB.
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
// ⚠ Advisory, not enforced. A feed that ignores it still works; it just keeps
// its share of the heap. Adding a new periodic network task? Check this at the
// top of its loop.

#pragma once
#include <stdbool.h>

void net_quiet_set(bool quiet);
bool net_quiet_active(void);
