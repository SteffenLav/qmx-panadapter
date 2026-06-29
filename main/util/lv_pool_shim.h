#pragma once
// Force LVGL's builtin tlsf pool to allocate from PSRAM instead of a static
// 256 KB BSS array.  Injected via -include on the lvgl__lvgl component target
// (see main/CMakeLists.txt).  When LV_MEM_POOL_ALLOC is defined, the pool is
// created via a runtime heap_caps_malloc(SPIRAM) call in lv_mem_core_builtin.c
// instead of the static work_mem_int[] array, freeing 256 KB of internal SRAM
// for IDF's heap (FreeRTOS, SDMMC, USB host, LWIP, web server, etc.).
#include "esp_heap_caps.h"
#define LV_MEM_POOL_ALLOC(size) \
    heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
