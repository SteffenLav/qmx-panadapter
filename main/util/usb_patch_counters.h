// Counters for the standing IDF USB patches that CANNOT log (TODO #189).
//
// Standing patches #7 (usb_dwc_hal.c: a channel error arriving without the halt
// bit) and #8 (hcd_dwc.c: a failed URB carrying a pipe event the error parser
// treats as impossible) both turn an abort() into a normal error report. Both
// run in the USB interrupt path, so neither may log - CLAUDE.md's cyan-flash
// rule forbids long or blocking work there, and a stalled MIPI-DSI frame-restart
// ISR blanks the panel.
//
// That left both patches UNVERIFIABLE: a clean log is ambiguous, because "the
// fault never happened" and "it happened and was handled" look identical. Each
// could only ever be failed-to-be-contradicted, never confirmed. A uint32_t
// increment is safe exactly where a log call is not, so the patches count and
// this module reports.
//
// The counters are defined HERE, in firmware, and merely declared extern inside
// the patched IDF files - deliberately that way round. If a patch is missing the
// count simply stays 0 instead of failing the link, so a build on a machine
// whose IDF tree has been reinstalled still works and tools/check_patches.py
// remains the thing that reports missing patches.
#pragma once

#include <stdint.h>

// Patch #7: usb_dwc_hal_chan_decode_intr() saw CHAN_INTRS_ERROR_MSK with no
// CHHLTD. Stock IDF asserts here; we report the error instead.
extern volatile uint32_t g_qmx_usb_chan_err_no_halt;

// Patch #8: _buffer_parse_error() saw a pipe_event other than NONE/XFER/
// OVERFLOW/STALL. Stock IDF abort()s here; we complete the URB with an error.
extern volatile uint32_t g_qmx_usb_pipe_event_unexpected;

// Patch #9: _buffer_parse() found the buffer at the parse index with no URB
// attached - a teardown race, which is what a QMX power-cycle with isochronous
// transfers in flight looks like. Stock IDF asserts here; we skip the buffer
// and advance the ring bookkeeping.
extern volatile uint32_t g_qmx_usb_buffer_parse_no_urb;

// Log the counters, but ONLY when one has changed since the last call. Called
// from the 10 s heap watchdog, so printing zeros every tick would be pure noise
// on a healthy device - and worse, would make the interesting case easy to miss.
// A line appearing at all is the positive evidence the patch fired.
void usb_patch_counters_report(void);
