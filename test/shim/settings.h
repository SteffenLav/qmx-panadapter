/* Host shim: ONLY the three upload cursors and the activation fields that
   adif_log.c touches. Deliberately not the real multi-kilobyte struct - a
   shim that tracked it would rot against it. */
#ifndef SHIM_SETTINGS_H
#define SHIM_SETTINGS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Fields adif_log.c reads, sizes as in main/storage/settings.h. Brought up to
 * date 2026-11-21: Cloudlog, the uploader credentials and lotw_dxcc had been
 * added to adif_log.c since this shim was written, and the harness had not
 * compiled since. */
typedef struct {
    uint32_t qrz_uploaded_n;
    uint32_t eqsl_uploaded_n;
    uint32_t lotw_uploaded_n;
    uint32_t cloudlog_uploaded_n;
    char     qrz_api_key[40];
    char     eqsl_user[16];
    char     eqsl_pswd[32];
    char     cloudlog_url[96];
    char     cloudlog_key[64];
    char     lotw_dxcc[8];
} qmx_settings_t;
void settings_load_all(qmx_settings_t *out);
void settings_get_upload_cursors(uint32_t *qrz, uint32_t *eqsl, uint32_t *lotw,
                                 uint32_t *cloudlog);
void settings_set_cloudlog_uploaded_n(uint32_t n);
void settings_shift_upload_cursors(uint32_t to_base);
void settings_set_rot_base(uint32_t base);
uint32_t settings_get_rot_base(void);
void settings_flush(void);
bool settings_qso_log_seen(void);
void settings_mark_qso_log_seen(void);
void settings_set_qrz_uploaded_n(uint32_t n);
void settings_set_eqsl_uploaded_n(uint32_t n);
void settings_set_lotw_uploaded_n(uint32_t n);
const char *settings_activation_sig_name(void);
/* ⛔ MUST be declared, not left implicit. Without it C assumed int and the
 * harness failed on the pointer conversion at adif_log.c's MY_SIG field -
 * which is the one that decides whether an activation credits at all. */
const char *settings_activation_adif_field(void);
bool settings_get_activation_ref(char *out, size_t out_sz);
#endif
