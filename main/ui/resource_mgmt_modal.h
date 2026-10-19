#pragma once
#include <stdbool.h>

// Resource Management panel - double-tap the spectrum to open (repurposed
// from the old "double-tap resets zoom+pan" gesture; that reset is still
// reachable from the top-bar Zoom menu, so nothing was lost - see the
// operator's own call, 2026-09-20).
//
// Lists the background feeds that compete with RX audio for the same
// scarce internal-RAM/DMA pool (see net/net_quiet.h) as plain on/off
// toggles, and enforces ONE rule: while RX audio is on, every other row
// here is held off and cannot be turned on - matching what net_quiet
// already does at runtime, made visible and directly controllable instead
// of only reachable through the RX audio drawer switch.

void resource_mgmt_modal_open(void);
bool resource_mgmt_modal_is_open(void);

/* True ONCE if the last restart was Save switching to a source whose input
 * level has to be set (the 3.5 mm jack). Boot calls this and, if it is true,
 * opens this window so the level meter is in front of the operator rather
 * than three taps away on a board that has just rebooted.
 *
 * Backed by RTC RAM: it survives esp_restart() and NOT a power cycle, and it
 * is consumed by the call, so it can never reopen twice or loop. */
bool resource_mgmt_modal_reopen_pending(void);
