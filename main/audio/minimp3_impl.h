/* Where minimp3's scratch landed - read off the running board, not assumed.
 *
 * mp3dec_decode_frame()'s 16,256 B scratch is a function-local static inside
 * the vendored header, so no caller can take its address. main/audio/
 * minimp3_impl.c records it through the header's MINIMP3_SCRATCH_PROBE hook
 * and publishes it here, so webradio.c can log it with mem_where() alongside
 * its other buffers.
 *
 * ⛔ IT IS NULL UNTIL THE FIRST DECODE CALL. Log it after a frame has been
 * decoded, never at task start.
 */
#pragma once

#include <stddef.h>

const void *minimp3_scratch_addr(void);
size_t      minimp3_scratch_size(void);
