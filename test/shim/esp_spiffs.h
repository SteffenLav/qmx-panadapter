/* Host shim. "SPIFFS" is a directory on the PC: the harness points base_path
   at a scratch dir and the real adif_log.c opens /spiffs/qso.adi inside it
   through the path hook below. */
#ifndef SHIM_ESP_SPIFFS_H
#define SHIM_ESP_SPIFFS_H
#include <stddef.h>
#include "esp_err.h"
typedef struct {
    const char *base_path;
    const char *partition_label;
    size_t      max_files;
    int         format_if_mount_failed;
} esp_vfs_spiffs_conf_t;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf);
esp_err_t esp_spiffs_info(const char *label, size_t *total, size_t *used);
esp_err_t esp_spiffs_check(const char *label);
esp_err_t esp_spiffs_gc(const char *label, size_t size);
#endif
