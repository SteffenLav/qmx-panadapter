#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// On-device ADIF log viewer. Read-only list of every logged QSO (newest
// first) for at-a-glance dupe checking in the field - "did I already work
// this station on this band" without pulling the file off the device.
// Opened by long-pressing the "Active: N" label on the FT8 screen.

// Build the modal (idempotent). Optional - show() builds lazily anyway.
void adif_view_modal_init(void);

// Re-reads the ADIF log and shows the modal.
void adif_view_modal_show(void);

#ifdef __cplusplus
}
#endif
