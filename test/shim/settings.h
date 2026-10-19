/* Host shim: ONLY the three upload cursors and the activation fields that
   adif_log.c touches. Deliberately not the real multi-kilobyte struct - a
   shim that tracked it would rot against it. */
#ifndef SHIM_SETTINGS_H
#define SHIM_SETTINGS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    uint32_t qrz_uploaded_n;
    uint32_t eqsl_uploaded_n;
    uint32_t lotw_uploaded_n;
} qmx_settings_t;
void settings_load_all(qmx_settings_t *out);
void settings_get_upload_cursors(uint32_t *qrz, uint32_t *eqsl, uint32_t *lotw);
void settings_set_qrz_uploaded_n(uint32_t n);
void settings_set_eqsl_uploaded_n(uint32_t n);
void settings_set_lotw_uploaded_n(uint32_t n);
const char *settings_activation_sig_name(void);
bool settings_get_activation_ref(char *out, size_t out_sz);
#endif
