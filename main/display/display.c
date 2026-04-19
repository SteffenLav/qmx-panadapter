#include "display.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

static lv_display_t *s_disp = NULL;

bool display_lock(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

void display_unlock(void)
{
    bsp_display_unlock();
}

esp_err_t display_init(lv_display_t **out_disp)
{
    ESP_LOGI(TAG, "Bringing up display via local M5Stack BSP");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority    = 4,
            .task_stack       = 8192,
            .task_affinity    = 0,
            .task_max_sleep_ms = 500,
            .timer_period_ms  = 5,
        },
        .buffer_size   = DISPLAY_H_RES * 72,
        .double_buffer = true,
        .flags = {
            .buff_dma    = 0,
            .buff_spiram = 1,
            .sw_rotate   = 0,
        },
    };

    s_disp = bsp_display_start_with_config(&cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "bsp_display_start_with_config failed");
        return ESP_FAIL;
    }

    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display ready: %dx%d", DISPLAY_H_RES, DISPLAY_V_RES);
    ESP_LOGI(TAG, "Free PSRAM=%zu KB, free internal=%zu KB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    if (out_disp) *out_disp = s_disp;
    return ESP_OK;
}
