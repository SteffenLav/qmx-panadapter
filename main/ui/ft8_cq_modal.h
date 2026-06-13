#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// CQ message preset editor. Long-press the "Call CQ" button to open it.
// Three editable text fields, each with a radio button; the selected one is
// what a short Call CQ tap transmits. Persisted to NVS via the settings layer.

// Build the modal (idempotent). Optional - show() builds lazily anyway.
void ft8_cq_modal_init(void);

// Show the modal, pre-filled from NVS (slot 0 defaults to "CQ <call> <grid>").
void ft8_cq_modal_show(void);

// Resolve the currently-selected CQ message into *out (NUL-terminated).
// Falls back to "CQ <call> <grid>" (or "CQ" if identity unset) when the
// selected slot is empty. Safe to call any time.
void ft8_cq_get_active_text(char *out, size_t len);

#ifdef __cplusplus
}
#endif
