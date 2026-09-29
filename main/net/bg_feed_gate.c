// See bg_feed_gate.h.

#include "bg_feed_gate.h"
#include "ui.h"

bool bg_feed_gate_active(void) { return ui_any_overlay_active(); }
