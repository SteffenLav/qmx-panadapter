#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// FT8 CQ-run reply filters. Opened via the "Filter" button (above Call CQ).
// Currently: exclude-prefix list for the auto-reply target picker.
// Persisted to NVS via the settings layer.

// Build the modal (idempotent). Optional - show() builds lazily anyway.
void ft8_filter_modal_init(void);

// Show the modal, pre-filled from NVS.
void ft8_filter_modal_show(void);

#ifdef __cplusplus
}
#endif
