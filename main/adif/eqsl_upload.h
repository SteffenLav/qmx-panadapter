#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  uploaded;   // records eQSL accepted this run (batches, not 1:1 with requests)
    int  failed;     // records left un-uploaded after a stopping error
    char error[96];  // reason for the last failure, or a precondition error
} eqsl_upload_result_t;

// Uploads every ADIF record logged since the last successful eQSL upload
// (tracked via settings_set_eqsl_uploaded_n / eqsl_uploaded_n), in batches of
// up to EQSL_BATCH_MAX records per HTTP request - eQSL's real-time interface
// accepts multiple QSOs in one ADIF blob, unlike QRZ's one-record-per-request
// API. Requires eQSL username+password to already be saved (eQSL has no
// API-key scheme). Blocks on the network for the whole run - call from a
// worker context (e.g. an HTTP handler), never from the LVGL thread.
bool eqsl_upload_pending(eqsl_upload_result_t *result);

#ifdef __cplusplus
}
#endif
