#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Takes a snapshot of the active screen as RGB565. *out_buf is allocated in
// PSRAM (heap_caps_malloc) and must be freed by the caller with
// heap_caps_free(). Returns ESP_OK on success.
esp_err_t screenshot_capture_rgb565(uint8_t **out_buf, size_t *out_size,
                                     uint32_t *out_w, uint32_t *out_h);

#ifdef __cplusplus
}
#endif