#include "sd_io_buf.h"

#include "esp_heap_caps.h"

#include <stdlib.h>   // free()

// 128 covers both cache levels - see the long note in sd_io_buf.h. A buffer
// allocated at 128 is correct in PSRAM (L2 line 128) and in internal SRAM
// (L1 line 64), so one constant serves both the preferred and the fallback
// placement and the caller never has to know which it got.
#define SD_IO_BUF_ALIGN 128u

void *sd_io_buf_alloc(size_t size)
{
    if (size == 0) return NULL;

    // The DMA alignment check tests the LENGTH as well as the address, so the
    // allocation is rounded up. Callers pass sector multiples, which are
    // already clean; this only guards against a caller that does not.
    size_t aligned = (size + SD_IO_BUF_ALIGN - 1) & ~((size_t)SD_IO_BUF_ALIGN - 1);

    void *p = heap_caps_aligned_alloc(SD_IO_BUF_ALIGN, aligned,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        // PSRAM first on purpose: internal RAM is the scarce pool on this
        // board, and an SD transfer buffer is one of the few things that is
        // large, short-lived and has no latency requirement.
        p = heap_caps_aligned_alloc(SD_IO_BUF_ALIGN, aligned,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

void sd_io_buf_free(void *p)
{
    // free() is correct for heap_caps_aligned_alloc memory: IDF's
    // heap_caps_aligned_free is a deprecated alias for heap_caps_free.
    free(p);
}
