#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Show the WiFi configuration modal as a full-screen overlay.
// User enters SSID and password, then presses Save (calls
// panadapter_wifi_reconnect and closes) or Cancel (closes without
// changes). Safe to call repeatedly; no-op if already open.
void wifi_config_modal_show(void);

#ifdef __cplusplus
}
#endif