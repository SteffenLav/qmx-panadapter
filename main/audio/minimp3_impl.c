/* minimp3's implementation, instantiated ONCE, and the one place that decides
 * where its memory lives.
 *
 * minimp3 (lieff/minimp3) is CC0 1.0 and vendored at components/minimp3 - see
 * that README for why it is in the tree and not a registry dependency. It
 * replaced chmorgan/esp-libhelix-mp3 on 2026-11-18, which was a LICENCE fix:
 * Helix is RealNetworks' decoder under RPSL 1.0, copyleft over combined works
 * and GPL-incompatible, and this firmware is GPL-3.
 *
 * ⛔ IT IS NOT A PERFORMANCE FIX, and nothing here claims to be one. The
 * web radio's 95.8%-of-core-1 break-up was memory PLACEMENT: Helix's three
 * hot per-frame buffers landed in LP SRAM, ~2.5x slower than cached PSRAM
 * (T133), and a standing patch moved them to PSRAM. That patch is deleted
 * with the library. Whether minimp3 is faster, slower or the same on the P4
 * is UNMEASURED - it is a float decoder where Helix was fixed-point.
 *
 * ---- where the memory goes, and why -------------------------------------
 *
 * Three allocations, all three deliberately in PSRAM. Internal RAM on this
 * board runs within ~30 KB of empty (#65), and the measured precedent is
 * that PSRAM is fast enough for exactly this kind of buffer: Helix's ~20 KB
 * of hot per-frame structures were moved there and the radio kept up.
 *
 *   scratch     16,256 B  .bss, EXT_RAM_BSS_ATTR (below)
 *   mp3dec_t     6,668 B  heap, MALLOC_CAP_SPIRAM (webradio.c)
 *   PCM buffer   4,608 B  heap, MALLOC_CAP_SPIRAM (webradio.c, s_pcm)
 *
 * The scratch is the one that had to move. Upstream it is a plain local, so
 * it is 16,256 B of STACK FRAME - measured 17,280 B for the whole function
 * with gcc -fstack-usage - on a task whose stack is 12,288 B. It would not
 * merely have been expensive, it would not have fit. A stack frame also
 * cannot be placed: internal RAM is the only option. Making it static moves
 * it to .bss (608 B of stack left, measured), and .bss CAN be placed. That
 * is the whole reason for the one local modification to the vendored header.
 *
 * ⚠ The cost is that the decoder is NON-REENTRANT. One stream, one task.
 *
 * ⛔ UNVERIFIED ON HARDWARE: grbuf (4,608 B) and syn (8,448 B) inside that
 * scratch are the hot working set, and PSRAM latency on them has never been
 * measured on a P4. If the radio breaks up, these are the first bytes to
 * suspect, and the number to beat is 24 ms of CPU per frame. Print where
 * things landed (webradio.c logs it) rather than assuming.
 */

#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR */
#include "minimp3_impl.h"

/* ---- the placement probe ------------------------------------------------
 *
 * The scratch is a function-local static inside the header, so nothing can
 * take its address from outside. The header calls this hook once per decode
 * and it records the pointer, so webradio.c can PRINT where the bytes went
 * rather than trusting the attribute. Two words, one branch, per 24 ms frame.
 *
 * ⛔ WHY THIS IS NOT PARANOIA. On the ESP32-P4 "internal RAM" includes LP
 * SRAM, ~2.5x slower than cached PSRAM from the HP core (T133), and that is
 * precisely where Helix's three hot buffers landed without anyone noticing
 * until the addresses were printed. A section attribute is a request to the
 * linker; the address is the measurement. */
static const void *s_scratch;
static size_t      s_scratch_sz;

static inline void minimp3_scratch_probe(const void *p, size_t n)
{
    if (!s_scratch) { s_scratch = p; s_scratch_sz = n; }
}

const void *minimp3_scratch_addr(void) { return s_scratch; }
size_t      minimp3_scratch_size(void) { return s_scratch_sz; }

#define MINIMP3_SCRATCH_PROBE(p, n) minimp3_scratch_probe((p), (n))

/* Put mp3dec_decode_frame()'s scratch in PSRAM .bss. The vendored header
 * defaults this to plain `static`, which is what the host A/B harness builds,
 * so the attribute is the only difference between the tested code and the
 * built code. */
#define MINIMP3_SCRATCH_STORAGE static EXT_RAM_BSS_ATTR

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
