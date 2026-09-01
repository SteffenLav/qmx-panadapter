<#
.SYNOPSIS
    Stops the ESP-IDF USB host aborting when a failed URB carries a pipe event
    its error parser considers impossible, which reboots the whole device.

.DESCRIPTION
    ESP-IDF v5.4.4's hcd_dwc.c has, in _buffer_parse_error():

        switch (buffer->status_flags.pipe_event) {
        ...
        default:
            // HCD_PIPE_EVENT_URB_DONE and HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL should not occur here
            abort();

    That comment is an ASSUMPTION, and this hardware violates it. Serial-captured
    on the Tab5 (ESP32-P4 v1.3) during an overnight soak of the RELEASED v1.8.6,
    2026-08-18: after 7 h 06 m of a completely healthy FT8 receive session - 26
    decodes/slot, audio 49,460 pairs/s, 53 KB internal free, idle0 61 % - the
    device died with

        abort() was called at PC 0x480f3e31 on core 0
        rst:0xc (SW_CPU_RESET)

    a BARE abort() with no assert text, at 25,582,646 ms uptime.

    HOW THAT PC WAS TIED TO THIS LINE (it does not read as obvious, so it is
    recorded here). addr2line puts 0x480f3e31 in _buffer_parse at hcd_dwc.c:2578,
    which is the default: of a DIFFERENT switch - on pipe->ep_char.type. That one
    is declared "usb_dwc_xfer_type_t type : 2" (usb_dwc_hal.h) and the enum has
    exactly four members, 0-3, all with cases, so that default: is unreachable by
    construction. _buffer_parse_error() is static inline and is called from
    _buffer_parse(), so it is inlined, and its identical bare abort() folds with
    the one at 2578 - addr2line reports the surviving line. Its switch is on
    "hcd_pipe_event_t pipe_event : 8", eight bits, so its default: IS reachable.
    Corroborating: the project builds with
    CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2, so a failing assert() would
    have printed "assert failed: ... file:line". Nothing printed, which is what
    proves it was a plain abort() and not either assert() at the top of
    _buffer_parse.

    THE REBOOT IS NOT THE EXPENSIVE PART. An abort is a warm reset with the QMX
    attached, which is the documented TODO #74 trigger, so the radio then failed
    to re-enumerate and stayed dead for the rest of the night (4 ENUM failures,
    then "RX 0 pairs/s"). The operator's report was "the QMX was wedged" - the QMX
    was fine. When a QMX wedge is reported after an unattended run, look for a
    Tab5 abort first.

    THE FIX FOLLOWS THIS FILE'S OWN PRECEDENT. Standing patch #4 hit the same
    class a few hundred lines up, in _buffer_parse_bulk(), and resolved it by
    reporting USB_TRANSFER_STATUS_ERROR with zero bytes instead of asserting. The
    same answer applies here and is even smaller: _buffer_parse_error() has
    ALREADY set transfer->actual_num_bytes = 0 before the switch, so the default:
    only needs to set the status. Every other branch of this switch does exactly
    that and nothing else. The URB is then completed with an error and the class
    driver retries - and cat.c's poll task already tolerates ~20 consecutive
    transient failures.

    Deliberately NO logging added: this can run inside the HCD's critical section
    and in the USB interrupt path, which risks the MIPI-DSI frame-restart stall
    CLAUDE.md records as the "cyan flash".

    This is the FOURTH assert/abort of the same family in IDF's USB stack - see
    also apply_hcd_bulk_error_recovery.ps1 (#4, this same file),
    apply_hub_recover_tolerant.ps1 (#5) and
    apply_usb_dwc_hal_chan_error_tolerant.ps1 (#7). The pattern: IDF's USB stack
    asserts on hardware states it treats as impossible, and on this board they
    happen. When a new USB abort appears, look for a HAL_ASSERT / ESP_ERROR_CHECK
    / bare abort() guarding a "cannot happen" case with a working error path
    sitting right beside it.

    NOTE this file carries TWO of our patches (#4 and #8) with separate markers.
    Applying one does not apply the other.

    Because this edits the pinned IDF install (NOT the project tree), it is wiped
    if the IDF is reinstalled and must be re-applied per build machine.
    tools/check_patches.py fails the build if it is missing.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_hcd_buffer_parse_error_tolerant.ps1
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

# The marker is the COUNTER SYMBOL, not the date comment (TODO #189). A tolerant
# but silent patch is indistinguishable from no patch at all in a log, so it must
# not pass the check: "patched" has to mean "patched AND observable".
if ($contentLf.Contains("g_qmx_usb_pipe_event_unexpected")) {
    Write-Host "Already patched (_buffer_parse_error pipe_event tolerance + counter) - nothing to do." -ForegroundColor Green
    exit 0
}

# The counter increment. An extern declaration inside a function body is legal C,
# so this stays one self-contained block with no include to add. The symbol is
# DEFINED in firmware (main/util/usb_patch_counters.c), so a missing patch leaves
# the count at 0 rather than failing the link.
$counter = @'
        // TODO #189: count it. This path may not log - it can run in the HCD's
        // critical section and the USB interrupt path - and that made the patch
        // unverifiable: a clean log could not distinguish "the fault never
        // happened" from "it happened and was handled". A uint32_t increment is
        // safe exactly where a log call is not. Reported by
        // usb_patch_counters_report() from the 10 s heap watchdog.
        extern volatile uint32_t g_qmx_usb_pipe_event_unexpected;
        g_qmx_usb_pipe_event_unexpected++;
'@
$counterLf = ($counter -replace "`r`n", "`n") + "`n"   # trailing NL: the next
# statement must start on its own line

# v1 -> v2: tolerance already in, counter missing. Insert it rather than
# re-applying, so an already-working tree is not disturbed.
if ($contentLf -match "PATCHED \(qmx-panadapter, 2026-08-18 buffer_parse_error\)") {
    $anchor = "        // and the USB interrupt path (the `"cyan flash`" rule).`n"
    if (-not $contentLf.Contains($anchor)) {
        Write-Host "Found the v1 patch but not its last comment line - upgrade by hand." -ForegroundColor Red
        exit 1
    }
    $result = $contentLf.Replace($anchor, $anchor + $counterLf)
    [System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Upgraded hcd_dwc.c patch: the unexpected-pipe-event path now increments a counter (#189)." -ForegroundColor Green
    Write-Host "  $target"
    exit 0
}

$original = @'
    default:
        // HCD_PIPE_EVENT_URB_DONE and HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL should not occur here
        abort();
        break;
    }
'@

$patched = @'
    default:
        // PATCHED (qmx-panadapter, 2026-08-18 buffer_parse_error): stock code is
        //     // HCD_PIPE_EVENT_URB_DONE and HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL should not occur here
        //     abort();
        // That "should not occur" is an assumption. Field-observed on ESP32-P4
        // v1.3 at 7h06m of a healthy FT8 session (v1.8.6): this default: was
        // reached and the bare abort() rebooted the device - which, being a warm
        // reset with the QMX attached, then left the radio unable to
        // re-enumerate for the rest of the night (TODO #74). pipe_event is an
        // 8-bit field, so this branch is reachable, unlike the switch in
        // _buffer_parse() that addr2line points at.
        // Reporting a failed transfer is strictly better than aborting, and is
        // exactly what standing patch #4 does a few hundred lines up in this
        // same file: actual_num_bytes was already set to 0 above, so this only
        // needs the status, same as every other branch here. The class driver
        // then retries. No logging - this can run in the HCD critical section
        // and the USB interrupt path (the "cyan flash" rule).
        transfer->status = USB_TRANSFER_STATUS_ERROR;
        break;
    }
'@

# Normalize line endings for the match (the IDF tree uses LF)
$originalLf = $original -replace "`r`n", "`n"

# The counter goes in ahead of the status assignment, inside the same branch.
$patched = $patched -replace "        transfer->status = USB_TRANSFER_STATUS_ERROR;",
                             ($counter + "`n        transfer->status = USB_TRANSFER_STATUS_ERROR;")

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock _buffer_parse_error default branch - hcd_dwc.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

# Guard against a partial/ambiguous match: this exact block must appear once.
$occurrences = ([regex]::Matches($contentLf, [regex]::Escape($originalLf))).Count
if ($occurrences -ne 1) {
    Write-Host "Expected exactly 1 occurrence of the stock block, found $occurrences - refusing to patch." -ForegroundColor Red
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched hcd_dwc.c: an unexpected pipe_event on the error path is reported, not aborted." -ForegroundColor Green
Write-Host "  $target"
