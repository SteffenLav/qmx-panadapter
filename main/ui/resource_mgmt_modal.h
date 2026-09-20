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
