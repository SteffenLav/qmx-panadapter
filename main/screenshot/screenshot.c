#include "screenshot.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "ui.h"

static const char *TAG = "SCREENSHOT";

// Recursively zero horizontal scroll on every object in the tree.
// Vertical scroll is preserved (the FT8 decode list intentionally scrolls
// vertically; we don't want to reset that). Defensive: any container that
// has acquired a non-zero scroll_x for any reason (focus chain, layout
// nudge, animation residue) will be reset to 0 before we render the
// snapshot, so the captured image matches what the user sees.
static void zero_h_scroll_recursive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_scroll_to_x(obj, 0, LV_ANIM_OFF);
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        zero_h_scroll_recursive(lv_obj_get_child(obj, i));
    }
}

// Overlays (band/mode popups, etc.) live on lv_layer_top(), which is a sibling
// of the active screen - lv_snapshot_take_to_buf(screen) never includes it, so
// a screenshot taken with a popup open showed a blank top-left. Snapshot the
// top layer as ARGB8888 and alpha-blend it over the RGB565 base so the capture
// matches exactly what's on the Tab5 screen. Best-effort: on OOM/failure we
// just return the base (screen-only) image rather than erroring.
static void composite_layer_over_rgb565(lv_obj_t *layer, uint16_t *base,
                                        int32_t w, int32_t h)
{
    if (!layer || lv_obj_get_child_count(layer) == 0) return;  // nothing on it

    size_t top_size = (size_t)w * (size_t)h * 4;               // ARGB8888
    uint8_t *tbuf = heap_caps_malloc(top_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tbuf) return;

    lv_image_dsc_t tdsc;
    memset(&tdsc, 0, sizeof(tdsc));
    lv_result_t r = lv_snapshot_take_to_buf(layer, LV_COLOR_FORMAT_ARGB8888,
                                            &tdsc, tbuf, top_size);
    if (r == LV_RESULT_OK &&
        tdsc.header.w == (uint32_t)w && tdsc.header.h == (uint32_t)h) {
        const uint8_t *src = tbuf;   // ARGB8888 in memory is B,G,R,A per pixel
        for (int32_t i = 0; i < w * h; i++) {
            uint8_t a = src[i * 4 + 3];
            if (a == 0) continue;                 // transparent -> keep base
            uint8_t sb = src[i * 4 + 0];
            uint8_t sg = src[i * 4 + 1];
            uint8_t sr = src[i * 4 + 2];
            if (a != 255) {                       // blend over the base pixel
                uint16_t p  = base[i];
                uint8_t  dr = (p >> 11) & 0x1F, dg = (p >> 5) & 0x3F, db = p & 0x1F;
                uint16_t br8 = (dr << 3) | (dr >> 2);
                uint16_t bg8 = (dg << 2) | (dg >> 4);
                uint16_t bb8 = (db << 3) | (db >> 2);
                uint16_t ia = 255 - a;
                sr = (uint8_t)((sr * a + br8 * ia) / 255);
                sg = (uint8_t)((sg * a + bg8 * ia) / 255);
                sb = (uint8_t)((sb * a + bb8 * ia) / 255);
            }
            base[i] = (uint16_t)(((sr & 0xF8) << 8) | ((sg & 0xFC) << 3) | (sb >> 3));
        }
    }
    heap_caps_free(tbuf);
}

esp_err_t screenshot_capture_rgb565(uint8_t **out_buf, size_t *out_size,
                                     uint32_t *out_w, uint32_t *out_h)
{
    lv_obj_t *screen = lv_screen_active();
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    size_t bytes_per_px = 2;  // RGB565
    size_t buf_size = (size_t)w * (size_t)h * bytes_per_px;

    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "heap_caps_malloc failed for %u bytes", (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }

    lv_image_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));

    bsp_display_lock(portMAX_DELAY);
    // Defensive: kill any stale horizontal scroll offset on every object in
    // the tree. Without this, FT8-view captures could wrap leftmost content
    // to the right edge if a container's scroll_x had drifted to ~45 px.
    zero_h_scroll_recursive(screen);
    // Also stop any in-flight animations so the snapshot renders a stable
    // frame (no mid-tween position interpolation).
    lv_anim_delete_all();
    lv_result_t res = lv_snapshot_take_to_buf(screen, LV_COLOR_FORMAT_RGB565,
                                              &dsc, buf, buf_size);
    // Blend anything on the top layer (open popups/menus) over the base so the
    // screenshot captures the WHOLE screen, not just the active-screen widgets.
    if (res == LV_RESULT_OK) {
        composite_layer_over_rgb565(lv_layer_top(), (uint16_t *)buf, w, h);
    }
    // lv_anim_delete_all() also killed the infinite-repeat "breathing"
    // animations on the edge-swipe grip handles; resume them now.
    ui_restart_edge_grip_anims();
    bsp_display_unlock();

    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_snapshot_take_to_buf failed: %d", (int)res);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    *out_size = dsc.data_size;
    *out_w = dsc.header.w;
    *out_h = dsc.header.h;
    return ESP_OK;
}
