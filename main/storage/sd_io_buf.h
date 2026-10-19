// DMA-alignment-clean buffers for file I/O that can land on the microSD card.
//
// WHY THIS EXISTS (measured 2026-10-04, Randy N4OPI's v1.16.11 diag):
//
//   E sdmmc_cmd: sdmmc_read_sectors_dma: ... returned 0x101
//   E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
//   E heapwatch: ALLOC FAILED: 512 B caps=0x00000008 in task 'httpd'
//   W httpd_txrx: httpd_resp_send_err: 404 Not Found - not found
//
// 0x101 is ESP_ERR_NO_MEM; caps=0x8 is MALLOC_CAP_DMA. The web file browser
// reported a missing file on a healthy card because the read behind it could
// not get 512 bytes of DMA memory.
//
// MECHANISM. sdmmc_read_sectors()/sdmmc_write_sectors() (IDF
// components/sdmmc/sdmmc_cmd.c) test the caller's buffer with
// esp_dma_is_buffer_alignment_satisfied(). A buffer that passes is DMA'd
// directly. A buffer that fails sends the driver down a bounce path that
// heap_caps_malloc's ONE SECTOR of MALLOC_CAP_DMA memory per call, reads a
// sector at a time into it and memcpy's out. That per-call allocation is what
// fails once the DMA pool is low - and on this board it always eventually is
// (see memory project_dma_pool_below_one_sd_block).
//
// The requirement is lcm(dma_alignment, cacheline), applied to BOTH the
// address and the transfer length:
//   - sdmmc_host_get_dma_info() reports dma_alignment_bytes = 4
//   - CONFIG_CACHE_L2_CACHE_LINE_SIZE = 128  (PSRAM, behind L2)
//   - CONFIG_CACHE_L1_CACHE_LINE_SIZE =  64  (internal SRAM, behind L1)
// so a PSRAM buffer needs 128 and an internal buffer needs 64. Transfer
// lengths are always whole 512 B sectors, which satisfy both.
//
// Nothing here is needed for correctness - an unaligned buffer still reads the
// right bytes, when the allocation happens to succeed. It is needed so that SD
// I/O stops depending on the state of the DMA pool.
//
// FatFs's own buffers (fs->win, fp->buf) are fixed separately, by TWO things
// that only work together:
//   - CONFIG_FATFS_USE_DYN_BUFFERS=y in sdkconfig.defaults, which moves them
//     out of the FATFS/FIL structs. Inline, they sit at a fixed OFFSET inside
//     the struct and no amount of aligning the allocation reaches them.
//   - tools/patches/apply_fatfs_dma_aligned_alloc.ps1, which makes the
//     resulting ff_memalloc() return them 128-byte aligned.
// Those cover metadata reads (stat/readdir/opendir/f_open); this header covers
// the bulk data buffers the application passes to fread/fwrite.
//
// VERIFIED on bench dev 2026-10-04: with both halves in, zero bounce-path
// entries and zero DMA allocation failures across a boot, and the web file
// browser listed and downloaded with the DMA pool at 2935 B (largest block
// 2688 B) - a state that returned an empty folder and a 404 before.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Alignment for a STATIC or stack buffer, which lives in internal SRAM:
// lcm(4, L1 cacheline 64). Use as
//     static char buf[4096] SD_IO_ALIGNED;
// Do not use this on a PSRAM buffer - that needs 128 (see above); allocate
// those with sd_io_buf_alloc() instead.
#define SD_IO_ALIGNED __attribute__((aligned(64)))

// Allocate a bulk I/O buffer that satisfies the SDMMC alignment check wherever
// it lands: 128-byte aligned, PSRAM preferred, internal as a fallback. `size`
// is rounded up to the alignment internally, so the caller may ask for any
// size; pass whole 512 B multiples when the buffer feeds fread/fwrite so the
// transfer length is aligned too.
//
// Returns NULL on failure. Free with sd_io_buf_free().
void *sd_io_buf_alloc(size_t size);

// Release a buffer from sd_io_buf_alloc(). NULL is a no-op.
void sd_io_buf_free(void *p);

#ifdef __cplusplus
}
#endif
