#include "screenshot.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "SCREENSHOT";
static lv_obj_t *s_btn = NULL;

// Base64 alphabet
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Chunk size for base64 OUTPUT lines (multiple of 4). 4096 b64 chars
// represents 3072 raw bytes per chunk.
#define B64_CHUNK_CHARS 4096

static void b64_encode_chunk(const uint8_t *in, size_t in_len, char *out)
{
    size_t i = 0, o = 0;
    while (i + 3 <= in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
        i += 3;
    }
    if (i < in_len) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < in_len) v |= ((uint32_t)in[i + 1]) << 8;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

// PackBits-style RLE. Control byte 0..127 -> copy next (n+1) bytes literally.
// Control byte 129..255 -> repeat next byte (257-n) times. 128 is unused.
// Returns encoded length, or 0 if it would exceed out_cap.
static size_t rle_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    size_t i = 0, o = 0;
    while (i < in_len) {
        size_t run = 1;
        while (i + run < in_len && in[i + run] == in[i] && run < 128) run++;

        if (run >= 2) {
            if (o + 2 > out_cap) return 0;
            out[o++] = (uint8_t)(257 - run);
            out[o++] = in[i];
            i += run;
        } else {
            size_t lit_start = i;
            size_t lit_len = 0;
            while (i < in_len && lit_len < 128) {
                size_t r = 1;
                while (i + r < in_len && in[i + r] == in[i] && r < 128) r++;
                if (r >= 2) break;
                i++;
                lit_len++;
            }
            if (o + 1 + lit_len > out_cap) return 0;
            out[o++] = (uint8_t)(lit_len - 1);
            memcpy(out + o, in + lit_start, lit_len);
            o += lit_len;
        }
    }
    return o;
}

static void dump_image(const uint8_t *data, size_t size, size_t raw_size,
                       uint32_t w, uint32_t h, const char *fmt)
{
    printf("\r\n===SCREENSHOT_BEGIN w=%lu h=%lu fmt=%s bytes=%u raw=%u===\r\n",
           (unsigned long)w, (unsigned long)h, fmt, (unsigned)size, (unsigned)raw_size);

    const size_t RAW_PER_CHUNK = (B64_CHUNK_CHARS / 4) * 3;
    char *line = malloc(B64_CHUNK_CHARS + 1);
    if (!line) {
        ESP_LOGE(TAG, "out of memory for b64 buffer");
        printf("===SCREENSHOT_ABORT===\r\n");
        return;
    }

    size_t sent = 0;
    size_t chunk_idx = 0;
    size_t total_chunks = (size + RAW_PER_CHUNK - 1) / RAW_PER_CHUNK;

    int64_t t_start = esp_timer_get_time();
    int64_t t_printf_total = 0;
    int64_t t_delay_total = 0;

    while (sent < size) {
        size_t this_raw = (size - sent > RAW_PER_CHUNK) ? RAW_PER_CHUNK : (size - sent);
        b64_encode_chunk(data + sent, this_raw, line);

        int64_t t0 = esp_timer_get_time();
        printf("%s\r\n", line);
        t_printf_total += esp_timer_get_time() - t0;

        sent += this_raw;
        chunk_idx++;

        if ((chunk_idx % 32) == 0 || chunk_idx == total_chunks) {
            ESP_LOGI(TAG, "chunk %u/%u (%u/%u bytes)",
                     (unsigned)chunk_idx, (unsigned)total_chunks,
                     (unsigned)sent, (unsigned)size);
        }

        int64_t t1 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(5));
        t_delay_total += esp_timer_get_time() - t1;
    }

    printf("===SCREENSHOT_END===\r\n");

    int64_t t_total = esp_timer_get_time() - t_start;
    ESP_LOGI(TAG, "timing: total=%lld ms, printf=%lld ms, delay=%lld ms, chunks=%u",
             (long long)(t_total / 1000), (long long)(t_printf_total / 1000),
             (long long)(t_delay_total / 1000), (unsigned)chunk_idx);

    free(line);
}

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

static void long_press_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "capture triggered");

    uint8_t *buf;
    size_t raw_size;
    uint32_t w, h;
    if (screenshot_capture_rgb565(&buf, &raw_size, &w, &h) != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "snapshot %lux%lu, %u bytes",
             (unsigned long)w, (unsigned long)h, (unsigned)raw_size);

    size_t comp_cap = raw_size + raw_size / 128 + 2;
    uint8_t *comp = heap_caps_malloc(comp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t comp_size = comp ? rle_encode(buf, raw_size, comp, comp_cap) : 0;

    if (comp_size > 0 && comp_size < raw_size) {
        ESP_LOGI(TAG, "RLE: %u -> %u bytes (%.1f%%)",
                 (unsigned)raw_size, (unsigned)comp_size,
                 100.0 * comp_size / raw_size);
        dump_image(comp, comp_size, raw_size, w, h, "rle565");
    } else {
        ESP_LOGI(TAG, "RLE not beneficial, sending raw");
        dump_image(buf, raw_size, raw_size, w, h, "rgb565");
    }

    if (comp) heap_caps_free(comp);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "capture complete");
}

void screenshot_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "parent is NULL");
        return;
    }

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        lv_indev_set_long_press_time(indev, 1000);
        ESP_LOGI(TAG, "long_press_time set to 1000 ms on indev %p", indev);
    } else {
        ESP_LOGW(TAG, "bsp_display_get_input_dev() returned NULL; "
                      "long_press_time not set (default 400 ms will apply)");
    }

    lv_obj_t *btn = lv_obj_create(parent);
    s_btn = btn;
    lv_obj_set_size(btn, 80, 80);
    lv_obj_set_pos(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    ESP_LOGI(TAG, "hidden 80x80 capture region at (0,0) ready");
}

lv_obj_t *screenshot_get_btn(void) { return s_btn; }
