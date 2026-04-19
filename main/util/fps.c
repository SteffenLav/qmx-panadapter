#include "fps.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "fps";

extern void ui_set_fps_text(const char *text);

static uint32_t s_frame_count = 0;

static void refr_finish_cb(lv_event_t *e)
{
    s_frame_count++;
}

static void fps_task(void *arg)
{
    int64_t last = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f;
        float fps = s_frame_count / dt;
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        size_t iram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;

        char buf[96];
        snprintf(buf, sizeof(buf),
                 "FPS: %.1f   PSRAM: %zu KB   IRAM: %zu KB",
                 fps, psram_free, iram_free);
        ui_set_fps_text(buf);
        ESP_LOGI(TAG, "%s", buf);

        s_frame_count = 0;
        last = now;
    }
}

void fps_counter_start(void)
{
    lv_display_add_event_cb(lv_display_get_default(), refr_finish_cb,
                            LV_EVENT_REFR_READY, NULL);
    xTaskCreate(fps_task, "fps", 4096, NULL, 2, NULL);
}