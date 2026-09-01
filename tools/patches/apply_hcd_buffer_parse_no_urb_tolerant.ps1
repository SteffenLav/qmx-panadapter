<#
.SYNOPSIS
    Stops the ESP-IDF USB host aborting when a DMA buffer reaches the parser with
    no URB attached, which reboots the whole device - and, with the radio
    connected, wedges the QMX with it.

.DESCRIPTION
    ESP-IDF v5.4.4's hcd_dwc.c has, at the top of _buffer_parse():

        dma_buffer_block_t *buffer_to_parse = pipe->buffers[pipe->multi_buffer_control.fr_idx];
        assert(buffer_to_parse->urb != NULL);

    Field-observed on the Tab5 (ESP32-P4 v1.3), 2026-09-01: the operator
    restarted the QMX and the Tab5 flashed cyan and rebooted. The #117 crash
    record had the whole thing on the next boot, with nothing to reproduce:

        assert failed: _buffer_parse hcd_dwc.c:2573 (buffer_to_parse->urb != NULL)
        task   : USB UAC Host   core 0   after 397.023 s of uptime
        reason : Illegal instruction - Exception was unhandled

    A power cycle with isochronous transfers in flight tears the pipe down
    underneath the parser, so the buffer at fr_idx can legitimately have been
    released already. The assert calls that impossible. It is not.

    THE REBOOT IS NOT THE EXPENSIVE PART. An abort is a warm reset with the QMX
    attached, which is the documented TODO #74 trigger - so restarting the radio
    can cost the radio as well, and the operator sees "the QMX wedged" for what
    is actually a Tab5 abort. That inversion is recorded twice already in
    CLAUDE.md; this is the third way into it.

    THE FIX. There is nothing to parse into a URB that is not there, so the
    buffer is skipped - but the ring bookkeeping still has to advance exactly as
    the normal path advances it, or _buffer_flush_all() spins on counts that
    never drain:

        buffer_to_parse->flags.val = 0;
        pipe->multi_buffer_control.fr_idx++;
        pipe->multi_buffer_control.buffer_num_to_parse--;
        pipe->multi_buffer_control.buffer_num_to_fill++;

    That is precisely what the tail of _buffer_parse() does, minus everything
    that touches the URB (hcd_var, the done tailq, num_urb_done). No URB is
    completed because none exists; the class driver simply sees one fewer
    completion, which the UAC driver already tolerates - it is an isochronous
    stream, where a lost buffer is a lost audio packet and nothing more.

    Deliberately NO logging: this runs in the HCD's critical section and the USB
    interrupt path, which risks the MIPI-DSI frame-restart stall CLAUDE.md
    records as the "cyan flash". It increments g_qmx_usb_buffer_parse_no_urb
    instead (TODO #189), reported by usb_patch_counters_report() from the 10 s
    heap watchdog and in /api/status as usb_patch.

    This is the FIFTH assert/abort of the same family in IDF's USB stack - see
    also apply_hcd_bulk_error_recovery.ps1 (#4, this same file),
    apply_hub_recover_tolerant.ps1 (#5),
    apply_usb_dwc_hal_chan_error_tolerant.ps1 (#7) and
    apply_hcd_buffer_parse_error_tolerant.ps1 (#8, this same file). The pattern:
    IDF's USB stack asserts on hardware states it treats as impossible, and on
    this board they happen. When a new USB abort appears, look for a HAL_ASSERT /
    ESP_ERROR_CHECK / bare abort() guarding a "cannot happen" case with a working
    error path sitting right beside it.

    NOTE this file now carries THREE of our patches (#4, #8 and #9) with separate
    markers. Applying one does not apply the others.

    Because this edits the pinned IDF install (NOT the project tree), it is wiped
    if the IDF is reinstalled and must be re-applied per build machine.
    tools/check_patches.py fails the build if it is missing.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_hcd_buffer_parse_no_urb_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/usb/hcd_dwc.c"
if (-not (Test-Path $target)) {
    Write-Host "hcd_dwc.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content = Get-Content -Raw -Path $target
$contentLf = $content -replace "`r`n", "`n"

# The marker is the COUNTER SYMBOL, not a date comment (TODO #189). A tolerant
# but silent patch is indistinguishable from no patch at all in a log, so
# "patched" has to mean "patched AND observable".
if ($contentLf.Contains("g_qmx_usb_buffer_parse_no_urb")) {
    Write-Host "Already patched (_buffer_parse no-URB tolerance + counter) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
static void _buffer_parse(pipe_t *pipe)
{
    assert(pipe->multi_buffer_control.buffer_num_to_parse > 0);
    dma_buffer_block_t *buffer_to_parse = pipe->buffers[pipe->multi_buffer_control.fr_idx];
    assert(buffer_to_parse->urb != NULL);
'@

$patched = @'
static void _buffer_parse(pipe_t *pipe)
{
    assert(pipe->multi_buffer_control.buffer_num_to_parse > 0);
    dma_buffer_block_t *buffer_to_parse = pipe->buffers[pipe->multi_buffer_control.fr_idx];
    // PATCHED (qmx-panadapter, 2026-09-01 buffer_parse_no_urb): stock code is
    //     assert(buffer_to_parse->urb != NULL);
    // Field-observed on ESP32-P4 v1.3 when the operator restarted the QMX: a
    // power cycle with isochronous transfers in flight tears the pipe down
    // underneath the parser, so the buffer at fr_idx can already have been
    // released. The assert called that impossible and rebooted the device -
    // which, being a warm reset with the radio attached, then wedged the QMX
    // too (TODO #74).
    //
    // There is nothing to parse into a URB that is not there, so skip the
    // buffer - but advance the ring bookkeeping exactly as the tail of this
    // function does, minus everything that touches the URB, or
    // _buffer_flush_all() spins on counts that never drain. On an isochronous
    // stream the cost is one lost audio packet.
    //
    // No logging: this runs in the HCD critical section and the USB interrupt
    // path (the "cyan flash" rule). TODO #189: count it instead, so the patch
    // is observable rather than merely silent. The symbol is DEFINED in
    // firmware (main/util/usb_patch_counters.c), so a missing patch leaves the
    // count at 0 instead of failing the link.
    if (buffer_to_parse->urb == NULL) {
        extern volatile uint32_t g_qmx_usb_buffer_parse_no_urb;
        g_qmx_usb_buffer_parse_no_urb++;
        buffer_to_parse->flags.val = 0;
        pipe->multi_buffer_control.fr_idx++;
        pipe->multi_buffer_control.buffer_num_to_parse--;
        pipe->multi_buffer_control.buffer_num_to_fill++;
        return;
    }
'@

$originalLf = $original -replace "`r`n", "`n"
$patchedLf  = $patched  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock _buffer_parse() prologue - hcd_dwc.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

# Guard against a partial/ambiguous match: this exact block must appear once.
$occurrences = ([regex]::Matches($contentLf, [regex]::Escape($originalLf))).Count
if ($occurrences -ne 1) {
    Write-Host "Expected exactly 1 occurrence of the stock block, found $occurrences - refusing to patch." -ForegroundColor Red
    exit 1
}

$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched hcd_dwc.c: a DMA buffer with no URB is skipped, not asserted on." -ForegroundColor Green
Write-Host "  $target"
