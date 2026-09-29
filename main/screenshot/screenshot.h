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

// ---- the frame-buffer path: a screenshot that allocates almost nothing ------
//
// The function above needs 1.8 MB of CONTIGUOUS PSRAM, because
// lv_snapshot_take_to_buf() re-renders the whole widget tree into one buffer.
// On the WSPR page that is simply not available - measured on the dev bench
// 2026-09-12, with the operator trying to photograph the spot map: 2.26 MB
// free but a largest block of 1.31 MB, i.e. fragmented rather than exhausted,
// and short by 467 KB. Waiting does not fix it either; across an overnight
// soak PSRAM moved only ~350 KB per WSPR cycle, so those capture windows are
// held rather than released between captures.
//
// So don't allocate a frame at all. The DSI panel already owns one
// (num_fbs = 1, so there is no back buffer to pick the wrong one of) and it
// holds exactly what is on the glass - including the top layer, which the
// snapshot path had to composite by hand.
//
// The panel is natively 720x1280 and LVGL presents 1280x720 through
// LV_DISPLAY_ROTATION_90, so ONE LOGICAL ROW IS ONE PANEL COLUMN. Rows are
// therefore read with a stride, which is cache-hostile but bounded, and
// costs one 2.5 KB scratch row instead of 1.8 MB.
//
// Returns ESP_OK and the LOGICAL (landscape) dimensions if the panel frame
// buffer is reachable; ESP_ERR_NOT_SUPPORTED if it is not, so the caller can
// fall back to the allocating path.
esp_err_t screenshot_fb_begin(uint32_t *out_w, uint32_t *out_h);

// One logical row: `count` pixels of row `y`, starting at logical column `x`,
// written to `dst` as RGB565. Valid only between _begin() and _end().
void screenshot_fb_row(uint32_t y, uint32_t x, uint32_t count, uint16_t *dst);

void screenshot_fb_end(void);

// Copy a w x h region out of the live frame buffer into a private PSRAM
// buffer, in ONE lock hold, so the result is a single instant rather than
// whatever the screen did across however long the caller then takes to send
// it. Steffen OZ1LAV, 2026-09-26: a full-frame /ss.bmp over a slow WiFi link
// takes up to ~15 s to leave (see ss_bmp_handler's own chunking comment), and
// without this, every one of those ~15 s worth of rows is read live off the
// panel as it keeps redrawing - the panadapter or waterfall moves mid-capture,
// so the top of the image is an older frame than the bottom. A zig-zag, not
// corruption.
//
// *out_buf is allocated in PSRAM and must be freed by the caller with
// heap_caps_free(). Valid only between _begin() and _end() - like _row(),
// this reads through the pointer _begin() set up.
//
// Returns ESP_ERR_NO_MEM if w*h*2 bytes of contiguous PSRAM aren't available
// (the same contiguous-allocation problem screenshot_capture_rgb565() has,
// just at whatever size the caller's crop asks for rather than always the
// full 1.8 MB) - the caller should fall back to reading _row() live rather
// than fail outright, exactly as it did before this existed.
esp_err_t screenshot_fb_freeze(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                uint16_t **out_buf);

#ifdef __cplusplus
}
#endif