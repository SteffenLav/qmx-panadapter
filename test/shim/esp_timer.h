/* Host shim. A monotonic microsecond clock, driven by the harness rather than
   by the wall clock: a test that has to put a gap between two characters must
   be able to state the gap, not sleep for it. */
#ifndef SHIM_ESP_TIMER_H
#define SHIM_ESP_TIMER_H
#include <stdint.h>
extern int64_t shim_now_us;     /* advance it yourself; starts at 0 */
int64_t esp_timer_get_time(void);
#endif
