#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "display.h"
#include "ui.h"
#include "fps.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "QMX+ Panadapter starting");
    ESP_LOGI(TAG, "PSRAM total: %zu MB", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024*1024));

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    ui_init(disp);
    fps_counter_start();

    ESP_LOGI(TAG, "Init complete — main task idle");
    // app_main returns; FreeRTOS keeps LVGL and FPS tasks running.
}