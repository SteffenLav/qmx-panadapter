// Uploads logged QSOs to a self-hosted Cloudlog or Wavelog instance (#171).
#pragma once

#include <stdbool.h>

typedef struct {
    int  uploaded;      // records accepted this run
    int  failed;        // 1 if the run stopped on a failure
    // Human-readable reason, empty on success. Sized to match the web layer's
    // own 80-byte field (webserver.c s_last_upload.error) - a longer message
    // would just be truncated there, where nobody would see that it had been.
    char error[80];
} cloudlog_upload_result_t;

// Uploads everything logged since the stored cursor. Returns false only when
// nothing could be attempted (no URL/key configured, or the address is refused
// by the plaintext rule); `result->error` says why in words meant for the
// operator, not for a log.
bool cloudlog_upload_pending(cloudlog_upload_result_t *result);
