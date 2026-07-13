#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  uploaded;   // records QRZ accepted this run
    int  failed;     // records QRZ rejected (batch stops on the first one)
    char error[80];  // reason for the last failure, or a precondition error
} qrz_upload_result_t;

// Uploads every ADIF record logged since the last successful QRZ upload
// (tracked via settings_set_qrz_uploaded_n / qrz_uploaded_n). Requires a QRZ
// API key to already be saved (settings_set_qrz_api_key). Blocks on the
// network for the whole batch - call from a worker context (e.g. an HTTP
// handler), never from the LVGL thread.
//
// Stops at the first record QRZ rejects (result->failed becomes 1, error is
// set) rather than skipping past it, since QRZ's reasons (bad key, quota,
// etc.) usually apply to every subsequent record too - retrying the same
// batch after fixing the cause will pick up where it left off.
// EXCEPTION: a "duplicate" rejection is skipped and the batch continues -
// the record is already in the QRZ logbook (typically after a reboot lost
// the advanced cursor before the debounced NVS flush), so stopping on it
// would block every newer record forever (field-hit 2026-07-13).
bool qrz_upload_pending(qrz_upload_result_t *result);

#ifdef __cplusplus
}
#endif
