// LoTW (ARRL Logbook of the World) upload — signs logged QSOs into a TQ8
// file on-device and POSTs it to https://lotw.arrl.org/lotw/upload.
//
// Unlike QRZ/eQSL there is no account login: the TQ8 payload is
// self-authenticating (each QSO carries an RSA-SHA1 signature made with the
// user's LoTW callsign-certificate private key). The cert + key arrive via
// the web UI's p12 import (parsed browser-side, POSTed as base64 DER) and
// live on SPIFFS. Progress is tracked the same way as qrz/eqsl_upload.c:
// a plain count (lotw_uploaded_n), since adif_log.c only ever appends.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  uploaded;   // QSOs accepted by LoTW this run
    int  skipped;    // permanently un-uploadable records skipped (e.g. no LoTW band)
    int  failed;     // 1 if the run stopped on an error
    char error[120]; // human-readable reason when failed
} lotw_upload_result_t;

// Store the callsign certificate / private key as single-line base64 DER
// (cert: X.509 DER; key: PKCS#8 DER). Written to SPIFFS + fsync'd.
// Pass NULL/empty to clear the respective file.
esp_err_t lotw_store_cert_b64(const char *cert_b64);
esp_err_t lotw_store_key_b64(const char *key_b64);

// True when both cert and key files are present and non-empty.
bool lotw_cert_present(void);

// Read back the stored single-line base64 (PSRAM malloc'd, caller free()s).
// NULL when absent. Used by the config export (full-backup decision).
char *lotw_read_cert_b64(void);
char *lotw_read_key_b64(void);

// Build the gzipped TQ8 for up to max_qsos records starting at log index
// `from`. Returns a PSRAM buffer (caller free()s) and its length; NULL on
// error with a message in err. *consumed_out = records consumed (signed +
// skipped), *signed_out = records actually signed into the file (0 means
// nothing uploadable in the range).
uint8_t *lotw_build_tq8_gz(int from, int max_qsos,
                           int *consumed_out, int *signed_out,
                           size_t *len_out, char *err, size_t errlen);

// Upload all pending records (from lotw_uploaded_n to the end of the ADIF
// log) in batches. Returns false only when nothing could even be attempted
// (no cert/station info); otherwise true with per-run counts in *result.
bool lotw_upload_pending(lotw_upload_result_t *result);

#ifdef __cplusplus
}
#endif
