/* Host shim. The caps are ignored - on the PC there is one heap - but
   shim_heap_fail_after lets a test starve an allocation on purpose, which is
   how the "cache cannot grow" branch is reached without a real PSRAM
   shortage. */
#ifndef SHIM_ESP_HEAP_CAPS_H
#define SHIM_ESP_HEAP_CAPS_H
#include <stddef.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DEFAULT  0
extern int shim_heap_fail_after;   /* <0 = never fail (default) */
void *heap_caps_malloc(size_t n, unsigned caps);
void *heap_caps_calloc(size_t n, size_t sz, unsigned caps);
void *heap_caps_realloc(void *p, size_t n, unsigned caps);
#endif
