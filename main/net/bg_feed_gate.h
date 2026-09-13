// bg_feed_gate - "stop starting new PANADAPTER-adjacent network work while a
// full-screen overlay is up".
//
// Distinct from net_quiet.h on purpose. net_quiet is a global advisory flag
// set only for the OTA verify window, and it is already checked by
// SelfSpotter's OWN feeds (pskr_self.c, wspr_self.c, qrz_coords.c,
// band_conditions.c) - folding "any overlay is open" into that SAME flag was
// tried and reverted the same session it was written: the moment SelfSpotter
// itself opened, it would have gone quiet on its own reason for existing.
//
// This gate is for the OTHER feeds - DX cluster, the POTA/SOTA spot lane
// (spots.c) and psk_rx.c (all feed the PANADAPTER's spot lane), plus
// update_check's GitHub fallback - none of which draw anything on SelfSpotter,
// the Reader, the QMX terminal or the "Need guidance?" panel.
// ⛔ NOT RBN: rbn.c also captures the SelfSpotter's CW self-spots. It was gated
// here first, inside its session READ loop, and the map received no RBN spots
// at all while it was open (2026-09-13).
// Those screens are exactly where core 0 and the LWIP socket table are
// tightest (sock_owners dump 2026-09-13: 11 of 16 sockets already held by
// RBN+DXcluster+MQTT+httpd+rigctld alone, before SelfSpotter's own map/list
// drawing gets a turn), so holding these five back while any overlay is open
// buys back exactly the headroom the overlay's own screen cannot use anyway.
//
// Same contract as net_quiet: advisory only, never closes a live socket -
// a feed already connected stays connected; this only stops it from opening
// or reconnecting one while the overlay is up. Check it where a feed would
// otherwise begin a fetch or a reconnect, same as net_quiet_active().
#pragma once
#include <stdbool.h>

bool bg_feed_gate_active(void);
