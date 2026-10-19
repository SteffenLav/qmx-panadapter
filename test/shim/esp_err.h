#ifndef SHIM_ESP_ERR_H
#define SHIM_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL       -1
#define ESP_ERR_NOT_FOUND 0x105
const char *esp_err_to_name(esp_err_t e);
#endif
