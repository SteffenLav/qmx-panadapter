#pragma once

/* ---- Which internal RAM is this? ----------------------------------------
 *
 * "INTERNAL" is TWO different memories on the P4 and they are not
 * interchangeable. With CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y the
 * 32 KB LP/RTC SRAM block joins the generic heap, and esp_ptr_internal()
 * calls it internal - but reached from the HP core it is far SLOWER than
 * cached PSRAM. MEASURED at 2.5x on the JS8 decoder's hottest memmove
 * (T133: last_frame p50 1968 ms in LP SRAM against 881 ms once moved to
 * PSRAM - faster AND 15 KB cheaper, no trade).
 *
 * Any sub-16 KB malloc can win or lose that lottery at boot, so a diagnostic
 * that prints "INTERNAL" hides exactly the thing worth seeing. Three words,
 * not two.
 *
 * ⛔ Turning the config off is NOT the fix - it costs 32 KB of a heap already
 * at ~30 KB free (#65). LP SRAM is fine for cold data; hot buffers must not
 * be left to the lottery. Print the address and read it off the board.
 *
 * ⛔ And do NOT relocate on suspicion: on the FFT work area a PSRAM spill
 * cost 10x in the OTHER direction. This is per-buffer, and the address is
 * the only thing that settles it. */

#include <stdint.h>
#include "soc/soc.h"

static inline const char *mem_where(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    if (a >= SOC_RTC_DRAM_LOW && a < SOC_RTC_DATA_HIGH) return "LPSRAM";
    if (a >= SOC_DRAM_LOW     && a < SOC_DRAM_HIGH)     return "L2";
    return "PSRAM";
}
