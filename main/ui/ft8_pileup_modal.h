#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// On-device pileup list viewer/actioner - see ft8_pileup.h for what this
// tracks (everyone who's called during a CQ-run and hasn't been worked yet,
// even after they've stopped transmitting and aged out of the live decode
// list). Unlike adif_view_modal.c (read-only), rows here are tappable: tap
// the row to work that station - opens the same TX confirmation modal a
// live decode-list row tap does (ft8_tx_modal_show()), so "Auto Pounce" /
// "Transmit" / "Cancel" behave identically either way. Tap the small X to
// dismiss without working them. Nothing here ever arms a TX on its own.

// Build the modal (idempotent). Optional - show() builds lazily anyway.
void ft8_pileup_modal_init(void);

// Re-reads the pileup list and shows the modal.
void ft8_pileup_modal_show(void);

#ifdef __cplusplus
}
#endif
