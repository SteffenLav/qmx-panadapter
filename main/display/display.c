#include "display.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

static lv_display_t *s_disp = NULL;
static bool s_flipped = false;   // false = normal landscape (90), true = upside-down (270)

bool display_lock(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

void display_unlock(void)
{
    bsp_display_unlock();
}

void display_set_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

// Flip the landscape orientation 180 degrees (for upside-down mounting / which
// side the cables exit). Normal = LV_DISPLAY_ROTATION_90, flipped = _270. All
// widgets live in the same 1280x720 logical space, so LVGL re-maps the layout
// and ordinary touch automatically; only the raw-coord pinch handler in ui.c
// needs to know (via display_is_flipped). The lvgl_port lock is recursive, so
// this is safe both at boot (app_main) and from a drawer callback (LVGL task).
void display_set_flipped(bool flipped)
{
    if (!s_disp) return;
    s_flipped = flipped;
    if (display_lock(1000)) {
        lv_display_set_rotation(s_disp,
            flipped ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
        lv_obj_invalidate(lv_screen_active());
        display_unlock();
    }
    ESP_LOGI(TAG, "Display rotation: %s", flipped ? "270 (flipped 180)" : "90 (normal)");
}

bool display_is_flipped(void)
{
    return s_flipped;
}

esp_err_t display_init(lv_display_t **out_disp)
{
    ESP_LOGI(TAG, "Bringing up display via local M5Stack BSP");

    // Cold-boot fix: PI4IO expander holds LCD_RST and TP_RST low until configured.
    // bsp_display_start_with_config does NOT do this for us; warm/soft resets only
    // work because the expander retains state from the previous boot. Without these
    // two calls, on a true power-on-from-off the panel never responds to DSI commands
    // and esp_lcd_new_panel_io_dbi hangs forever in the read-FIFO wait loop.
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    ESP_LOGI(TAG, "PI4IO expander initialized (LCD_RST + TP_RST released)");
    // Let panel and touch chip come out of reset before the BSP probes I2C
    // for the touch chip. Without this, the probe fires too fast on cold boot,
    // the touch chip does not respond, BSP picks ST7703/ILI9881C path, but
    // touch chip *does* respond by the time touch init runs -> driver mismatch
    // -> assert in bsp_display_indev_init_to_st7123.
    vTaskDelay(pdMS_TO_TICKS(120));

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority    = 4,
            .task_stack       = 8192,
            .task_affinity    = 0,
            .task_max_sleep_ms = 500,
            .timer_period_ms  = 5,
        },
        .buffer_size   = DISPLAY_H_RES * 36,  // Phase 6.2: smaller flushes to keep LVGL rotate task under watchdog
        .double_buffer = true,
        .flags = {
            .buff_dma    = 0,
            .buff_spiram = 1,
            .sw_rotate   = 1,   // Phase 6.2: enable LVGL software rotation
        },
    };

    s_disp = bsp_display_start_with_config(&cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "bsp_display_start_with_config failed");
        return ESP_FAIL;
    }

    bsp_display_backlight_on();

    // Phase 6.2: rotate to landscape (panel is natively 720x1280 portrait)
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);
    ESP_LOGI(TAG, "Phase 6.2: requested LV_DISPLAY_ROTATION_90 (landscape)");

    ESP_LOGI(TAG, "Display ready: native=%dx%d (rotated to landscape)", DISPLAY_H_RES, DISPLAY_V_RES);
    ESP_LOGI(TAG, "Free PSRAM=%zu KB, free internal=%zu KB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    if (out_disp) *out_disp = s_disp;
    return ESP_OK;
}




