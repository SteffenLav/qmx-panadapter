#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/m5stack_tab5.h"

#include "display.h"
#include "ui.h"
#include "fps.h"
#include "cat.h"
#include "audio.h"
#include "dsp.h"
#include "render.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "QMX+ Panadapter starting");
    ESP_LOGI(TAG, "PSRAM total: %zu MB",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    ui_init(disp);
    fps_counter_start();

    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    ESP_LOGI(TAG, "USB host started");

    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(cat_init());
    ESP_ERROR_CHECK(dsp_init());
    ESP_ERROR_CHECK(render_init());

    ESP_LOGI(TAG, "Init complete — main task idle");
}

