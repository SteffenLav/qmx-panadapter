<#
.SYNOPSIS
    Makes FatFs allocate its sector buffers DMA-alignment-clean, so SD reads
    stop allocating a 512 B DMA bounce buffer per sector.

.DESCRIPTION
    MEASURED 2026-10-04 in Randy N4OPI's diag (qmx-diag-20261003-1637.zip),
    t=4133 s, on v1.16.11:

        E sdmmc_cmd: sdmmc_read_sectors_dma: ... returned 0x101
        E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
        E heapwatch: ALLOC FAILED: 512 B caps=0x00000008 in task 'httpd'
        W httpd_txrx: httpd_resp_send_err: 404 Not Found - not found

    0x101 is ESP_ERR_NO_MEM. caps=0x8 is MALLOC_CAP_DMA. The web file browser
    returned "file not found" for a file that was on the card, because the
    stat() behind it could not get 512 bytes of DMA memory.

    MECHANISM

    sdmmc_read_sectors() (IDF components/sdmmc/sdmmc_cmd.c) checks the caller's
    buffer with esp_dma_is_buffer_alignment_satisfied(). If the buffer passes,
    the read goes straight to DMA. If it does not, the driver falls back to a
    per-call heap_caps_malloc of one sector with MALLOC_CAP_DMA, reads one
    sector at a time into it, and memcpy's out. That malloc is the 512 B
    allocation above, and it fails whenever the DMA pool is low - which on this
    board it always eventually is.

    The required alignment is lcm(dma_alignment, cacheline):
      - sdmmc_host_get_dma_info() reports dma_alignment_bytes = 4
      - this project sets CONFIG_CACHE_L2_CACHE_LINE_SIZE=128 (PSRAM, L2)
        and CONFIG_CACHE_L1_CACHE_LINE_SIZE=64 (internal, L1)
    so a PSRAM buffer needs 128-byte alignment and an internal one needs 64.

    FatFs allocates its own sector window (fs->win, ff.c:3445) and, with
    CONFIG_FATFS_PER_FILE_CACHE=y, a per-file cache (fp->buf, ff.c:3885)
    through ff_memalloc(). With CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y those land
    in PSRAM via heap_caps_malloc_prefer(), which guarantees no alignment past
    the malloc minimum. So every FatFs metadata read - stat, readdir, opendir,
    f_open, the FAT itself - took the bounce path and needed a 512 B DMA
    allocation it could not always get.

    THE FIX

    Allocate through heap_caps_aligned_alloc() at 128 bytes, with the size
    rounded up to the same boundary, keeping the existing PSRAM-then-internal
    preference. 128 covers both cache levels, so the buffer is clean wherever
    it lands. ff_memfree() still uses free(), which is correct for
    heap_caps_aligned_alloc (heap_caps_aligned_free is a deprecated alias for
    heap_caps_free).

    This does NOT cover buffers the application passes to fread/fwrite - those
    are aligned at their own call sites in main/.

    Cost: at most 127 bytes of slack per FatFs allocation, in PSRAM.

    Because this edits the pinned IDF install (NOT the project tree), it is
    wiped if the IDF is reinstalled and must be re-applied per build machine -
    same maintenance model as tools/patches/apply_fatfs_exfat.ps1.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_fatfs_dma_aligned_alloc.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/fatfs/port/freertos/ffsystem.c"
if (-not (Test-Path $target)) {
    Write-Host "fatfs ffsystem.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$raw = Get-Content -Raw -Path $target

# ffsystem.c ships with CRLF line endings and this script is stored with LF, so
# the here-strings below would never match the file as read. Normalise to LF for
# matching, then restore CRLF on the way out. (A patch that silently fails to
# match is the failure mode tools/check_patches.py exists to catch - it just
# caught this one.)
$crlf = $raw.Contains("`r`n")
$content = $raw.Replace("`r`n", "`n")

if ($content -match "QMX_FF_MEMALLOC_DMA_ALIGNED") {
    Write-Host "Already patched (QMX_FF_MEMALLOC_DMA_ALIGNED present) - nothing to do." -ForegroundColor Green
    exit 0
}

# --- 1. the conditional include has to become unconditional ----------------
$oldInc = @"
#ifdef CONFIG_FATFS_ALLOC_PREFER_EXTRAM
#include "esp_heap_caps.h"
#endif
"@
$newInc = '#include "esp_heap_caps.h"'

if ($content -notmatch [regex]::Escape($oldInc)) {
    if ($content -notmatch [regex]::Escape($newInc)) {
        Write-Host "Could not find the esp_heap_caps.h include - ffsystem.c has changed." -ForegroundColor Red
        Write-Host "Patch by hand and update this script."
        exit 1
    }
} else {
    $content = $content.Replace($oldInc, $newInc)
}

# --- 2. replace the body of ff_memalloc ------------------------------------
$oldBody = @"
#ifdef CONFIG_FATFS_ALLOC_PREFER_EXTRAM
    return heap_caps_malloc_prefer(msize, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                            MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
#else
    return malloc(msize);
#endif
"@

if ($content -notmatch [regex]::Escape($oldBody)) {
    Write-Host "Could not find the expected ff_memalloc body - ffsystem.c has changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$newBody = @"
    /* QMX_FF_MEMALLOC_DMA_ALIGNED - qmx-panadapter standing patch 2026-10-04.
     *
     * FatFs allocates its sector window (fs->win) and per-file cache (fp->buf)
     * here, then hands them straight to the SDMMC driver. An unaligned buffer
     * sends sdmmc_read_sectors() down its bounce path, which heap_caps_malloc's
     * one 512 B MALLOC_CAP_DMA block per sector - and that allocation fails
     * once the DMA pool is low, surfacing as ESP_ERR_NO_MEM (0x101) from
     * sdmmc_read_blocks and a bogus "file not found" from anything reading the
     * card. 128 bytes covers lcm(dma_align 4, L2 line 128) for PSRAM and
     * lcm(4, L1 line 64) for internal, so the buffer is clean wherever it
     * lands. See tools/patches/apply_fatfs_dma_aligned_alloc.ps1 for the
     * measurement this came from. */
    const size_t qmx_align = 128;
    size_t qmx_size = ((size_t)msize + qmx_align - 1) & ~(qmx_align - 1);
#ifdef CONFIG_FATFS_ALLOC_PREFER_EXTRAM
    void *qmx_p = heap_caps_aligned_alloc(qmx_align, qmx_size,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!qmx_p) {
        qmx_p = heap_caps_aligned_alloc(qmx_align, qmx_size,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return qmx_p;
#else
    return heap_caps_aligned_alloc(qmx_align, qmx_size,
                                   MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
#endif
"@

$content = $content.Replace($oldBody, $newBody)

if ($crlf) { $content = $content.Replace("`n", "`r`n") }
[System.IO.File]::WriteAllText($target, $content, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched FatFs: ff_memalloc now returns 128-byte aligned buffers." -ForegroundColor Green
Write-Host "  $target"
