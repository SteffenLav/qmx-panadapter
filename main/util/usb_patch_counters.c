// See usb_patch_counters.h.

#include "usb_patch_counters.h"

#include "esp_log.h"

static const char *TAG = "usbpatch";

// Incremented from the USB interrupt path by the patched IDF files. volatile
// because the writer is an ISR and the reader is a task; a plain ++ is not
// atomic on riscv32, but a lost increment on a rare error path costs one unit of
// a diagnostic count and is not worth an ISR-side lock. The number is evidence
// that the patch fired and roughly how often, not an audited ledger.
volatile uint32_t g_qmx_usb_chan_err_no_halt     = 0;
volatile uint32_t g_qmx_usb_pipe_event_unexpected = 0;
volatile uint32_t g_qmx_usb_buffer_parse_no_urb    = 0;

void usb_patch_counters_report(void)
{
    static uint32_t last_no_halt = 0;
    static uint32_t last_unexpected = 0;
    static uint32_t last_no_urb = 0;

    uint32_t no_halt     = g_qmx_usb_chan_err_no_halt;
    uint32_t unexpected  = g_qmx_usb_pipe_event_unexpected;
    uint32_t no_urb      = g_qmx_usb_buffer_parse_no_urb;

    if (no_halt == last_no_halt && unexpected == last_unexpected &&
        no_urb == last_no_urb) return;

    // WARN, not INFO: each of these is an abort() that standing patches #7/#8
    // converted into a survivable error. The device is fine - that is the point -
    // but the event is exactly what a crash report needs to contain, and on an
    // unpatched build it would have been a reboot plus a wedged QMX (TODO #74).
    ESP_LOGW(TAG, "IDF USB patches fired: chan-err-no-halt=%u (patch #7)  "
                  "unexpected-pipe-event=%u (patch #8)  buffer-parse-no-urb=%u "
                  "(patch #9)  - handled, device stayed up",
             (unsigned)no_halt, (unsigned)unexpected, (unsigned)no_urb);

    last_no_halt = no_halt;
    last_unexpected = unexpected;
    last_no_urb = no_urb;
}
