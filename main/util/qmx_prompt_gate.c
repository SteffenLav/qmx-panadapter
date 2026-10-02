#include "qmx_prompt_gate.h"

bool qmx_prompt_gate_tick(qmx_prompt_gate_t *g, bool boot_complete,
                          uint32_t now_ms, bool wifi_enabled, bool wifi_connected)
{
    if (!g) return false;
    if (g->latched) return true;        // never un-latches, by design

    // Nothing may invite a power-on before app_main has finished. This is the
    // hard half of the gate and has no timeout: the RAM hazard is real for the
    // whole of start-up.
    if (!boot_complete) return false;

    if (!g->boot_seen) {                // first tick after start-up finished
        g->boot_seen = true;
        g->boot_ms   = now_ms;
    }

    // WiFi off entirely, or associated: settled either way.
    bool settled = !wifi_enabled || wifi_connected;

    // ... or it has had long enough and is not going to. Wrap-safe subtraction:
    // the difference of two uint32 tick counts is correct across the 49.7-day
    // roll-over, which a (now > boot + grace) comparison is not.
    if (!settled && (uint32_t)(now_ms - g->boot_ms) >= QMX_PROMPT_WIFI_GRACE_MS)
        settled = true;

    if (settled) g->latched = true;
    return settled;
}
