#pragma once

// Manual CW/SSB QSO entry (Brian KB9WFS, 2026-10-27).
//
// The first request to log a mode the Tab5 does not decode. His framing is the
// right one: the operator types the callsign and the two reports, and
// everything else - date, time, frequency, band, mode, our callsign, our grid
// and the running activation - is already known to the device and must not be
// asked for again.
//
// It writes through adif_log_record(), the same door FT8 uses, so a hand-typed
// contact is indistinguishable downstream: it counts towards the activation,
// it uploads to LoTW/QRZ/eQSL and it mirrors to the SD card.

void manual_log_modal_init(void);
void manual_log_modal_show(void);
