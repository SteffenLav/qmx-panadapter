#pragma once
/* Host stub: the firmware's logging, reduced to nothing. See
 * test/js8_submode_table_harness.c for why these stubs exist. */
#include <stdio.h>
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
