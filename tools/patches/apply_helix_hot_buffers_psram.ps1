<#
.SYNOPSIS
    Moves libhelix-mp3's three HOT per-frame buffers out of LP SRAM.

.DESCRIPTION
    MEASURED on the field bench 2026-11-01, read off the board:

        helix: dec 0x4ff360b4 L2 | huff 0x501084a8 LPSRAM
             | imdct 0x501097ac LPSRAM | subband 0x5010b3b0 LPSRAM

    All three of HuffmanInfo, IMDCTInfo and SubbandInfo landed inside
    0x50108000..0x50110000 - the 32 KB LP/RTC SRAM block - which from the HP
    core is about 2.5x SLOWER than cached PSRAM (measured independently on
    the JS8 decoder's hottest memmove: 1968 ms against 881 ms, T133). They
    are the structures Helix touches thousands of times per frame.

    The cost, same capture: MP3Decode burned 95.8% of core 1 and needed
    30.6 ms of CPU per 24 ms frame - 1.28x too slow in real time - so the
    web radio delivered 75% of the samples it owed the codec and broke up
    continuously. Nothing else was on that core above 1.7%.

    Why it happens here and not upstream: AllocateBuffers() uses eight plain
    malloc() calls, all under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384, so
    every one is requested from "internal" RAM - and on the ESP32-P4 with
    CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y that pool includes LP
    SRAM. It is a lottery decided at boot. dmatking/esp32-p4-webradio runs
    this same library on this same chip and keeps up because its firmware is
    4.5 kLOC with a roomy L2 heap and it wins the lottery by default; this
    project runs ~30 KB free (#65) and lost it on all three.

    The fix asks for PSRAM explicitly for those three and leaves the other
    five alone - MP3DecInfo already gets L2 (and carries the hot mainBuf
    with it), and FrameHeader / SideInfo / ScaleFactorInfo / DequantInfo are
    small and cold. FreeBuffers() releases them with free(), which is valid
    for heap_caps_malloc() pointers in ESP-IDF, so the teardown is unchanged.

    ⭐ PSRAM, not L2, deliberately. The three together are about 20 KB and
    the internal pool has ~30 KB free, so taking L2 would nearly empty it -
    and MALLOC_CAP_DMA (the only cap that excludes LP SRAM) comes out of the
    scarcest pool on the board, which USB host and the display need. PSRAM
    is 2.5x better than where they are now AND costs the internal pool
    nothing. If a future measurement shows PSRAM is still not fast enough,
    the number to beat is 24 ms per frame; print the addresses again rather
    than assuming.

    ⚠ This edits managed_components/, which is git-ignored and is WIPED by
    `idf.py fullclean`, a dependency refresh, and the release's
    `rm -r managed_components/`. Re-run it after any of those, like the
    other apply_*.ps1 patches. tools/check_patches.py is the authority and
    refuses the build if it is missing.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_helix_hot_buffers_psram.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/chmorgan__esp-libhelix-mp3/libhelix-mp3/real/buffers.c"

if (-not (Test-Path $target)) {
    Write-Host "buffers.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    Write-Host "Run a build first so the component is fetched, then re-run this."
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, 2026-11-01\) helix hot buffers") {
    Write-Host "Already patched (helix hot buffers -> PSRAM) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
	hi =  (HuffmanInfo *)     malloc(sizeof(HuffmanInfo));
	di =  (DequantInfo *)     malloc(sizeof(DequantInfo));
	mi =  (IMDCTInfo *)       malloc(sizeof(IMDCTInfo));
	sbi = (SubbandInfo *)     malloc(sizeof(SubbandInfo));
'@

$patched = @'
	/* PATCHED (qmx-panadapter, 2026-11-01) helix hot buffers -> PSRAM.
	 *
	 * MEASURED on the ESP32-P4 (M5Stack Tab5): these three landed at
	 * 0x501084a8 / 0x501097ac / 0x5010b3b0 - inside LP/RTC SRAM, which is
	 * in the generic heap on this target and ~2.5x SLOWER than cached
	 * PSRAM from the HP core. MP3Decode then needed 30.6 ms of CPU per
	 * 24 ms frame and the player broke up continuously. A plain malloc()
	 * here is a lottery between L2 DRAM and LP SRAM decided at boot, and
	 * on a board whose internal pool runs ~30 KB free it is a lottery we
	 * lose. These are the per-frame hot structures, so they are asked for
	 * explicitly. The other five stay as they were: MP3DecInfo already
	 * gets L2 and carries mainBuf with it, and the rest are small and cold.
	 *
	 * free() accepts heap_caps_malloc() pointers in ESP-IDF, so
	 * FreeBuffers() below needs no change. */
	hi =  (HuffmanInfo *)     heap_caps_malloc(sizeof(HuffmanInfo), MALLOC_CAP_SPIRAM);
	di =  (DequantInfo *)     malloc(sizeof(DequantInfo));
	mi =  (IMDCTInfo *)       heap_caps_malloc(sizeof(IMDCTInfo),   MALLOC_CAP_SPIRAM);
	sbi = (SubbandInfo *)     heap_caps_malloc(sizeof(SubbandInfo), MALLOC_CAP_SPIRAM);
'@

# The include has to come in with the same edit, or the file does not compile.
$origInc = @'
#include "coder.h"
'@
$newInc = @'
#include "coder.h"
#include "esp_heap_caps.h"	/* PATCHED (qmx-panadapter, 2026-11-01): PSRAM for the hot buffers */
'@

$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock AllocateBuffers() malloc block - buffers.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}
if (-not $contentLf.Contains(($origInc -replace "`r`n", "`n"))) {
    Write-Host "Could not find the coder.h include - buffers.c may have changed." -ForegroundColor Red
    exit 1
}

$result = $contentLf.Replace(($origInc -replace "`r`n", "`n"), ($newInc -replace "`r`n", "`n"))
$result = $result.Replace($originalLf, ($patched -replace "`r`n", "`n"))
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched buffers.c: HuffmanInfo, IMDCTInfo and SubbandInfo now come from PSRAM." -ForegroundColor Green
Write-Host "  $target"
