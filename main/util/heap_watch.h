/* heap_watch.h - fast internal-RAM dip detector + failed-allocation reporter.
 *
 * See heap_watch.c for why this exists: a 20.5 h capture ruled out a leak but
 * showed 84 transient events where internal free reached zero, invisible to
 * the 10 s heap line. Start it once from app_main.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the failed-allocation callback and starts the sampler task.
 * Safe to call once, after the heap and FreeRTOS are up. */
void heap_watch_start(void);

#ifdef __cplusplus
}
#endif
