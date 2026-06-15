#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the WiFi configuration modal at boot. Call once from ui_init()
// when internal heap is at its boot-time maximum (~199 KB free). The
// modal is hidden until wifi_config_modal_show() unhides it. Pre-building
// at boot avoids the fragmentation cliff that breaks runtime modal_build()
// when only ~70 KB internal heap is free (post-WiFi/audio/services).
void wifi_config_modal_init(void);

// Show the WiFi configuration modal as a full-screen overlay.
// User enters SSID and password, then presses Save (calls
// panadapter_wifi_reconnect and closes) or Cancel (closes without
// changes). Safe to call repeatedly; no-op if already open.
void wifi_config_modal_show(void);

// Same as wifi_config_modal_show(), but invokes on_close() once when the modal
// is dismissed (Save or Cancel). Used by the first-boot onboarding flow to
// chain into the callsign/grid prompt after the WiFi step. Pass NULL for none.
void wifi_config_modal_show_then(void (*on_close)(void));

#ifdef __cplusplus
}
#endif