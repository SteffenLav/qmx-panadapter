<#
.SYNOPSIS
    Re-applies the QMX-Panadapter esp_hosted "handled error must not abort" patch.

.DESCRIPTION
    managed_components/ is git-ignored and is wiped by `idf.py fullclean`, a
    dependency refresh, or the release process's `rm -r managed_components/`.
    Re-run this after any of those, before building.

    THE BUG, field-captured 2026-08-25 on the dev bench:

        assert failed: hosted_destroy_semaphore os_wrapper.c:573 (semaphore_handle)
        task: status   core 1   after 134.645 s of uptime

    esp_hosted's hosted_destroy_semaphore() was handed a NULL handle - almost
    certainly a semaphore whose creation had failed, since the internal-heap
    watermark on this board reaches 0 KB - and aborted the whole device. The
    caller was the 1 Hz WiFi RSSI poll in util/status.c.

    ⛔ THE REBOOT IS NOT THE EXPENSIVE PART. An abort is a warm reset with the
    radio attached, which is the documented #74 trigger, so the QMX then needs a
    manual power cycle too. One assert costs the session.

    THE FIX: the correct handling was ALREADY THERE - `return RET_INVALID;` sits
    directly under the assert. The assert only converted a handled error into a
    reboot. Same for the dest/src guards in the memcpy wrapper, which already
    `return NULL`. Both asserts removed; both keep their ESP_LOGE so the
    condition stays diagnosable.

    ⚠ THE TWO MEMPOOL asserts (hosted_lock_mempool / hosted_unlock_mempool) are
    DELIBERATELY LEFT IN. They have NO error path - portENTER_CRITICAL()
    dereferences the handle on the next line - and "tolerating" a NULL lock
    would mean running a critical section unprotected. Silent memory corruption
    is worse than a crash and far harder to find. The rule this patch follows is:
    REMOVE an assert that duplicates an existing handled path; KEEP one where no
    handling exists.

    This is the same family as IDF patches #4, #5, #7 and #8 - vendor code
    asserting on a state it treats as impossible, with a working error path
    beside it, on a board where that state happens.

    Ships as a byte-exact copy of the patched file rather than string edits.
    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_assert_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

$repo    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target  = Join-Path $PSScriptRoot "..\..\managed_components\espressif__esp_hosted\host\port\src\os_wrapper.c"
$stored  = Join-Path $PSScriptRoot "esp_hosted_os_wrapper.c.patched"
$marker  = "QMX_PANADAPTER_ASSERT_TOLERANT_PATCH_MARKER"

if (-not (Test-Path $stored)) { throw "Stored patched file missing: $stored" }
if (-not (Test-Path $target)) {
    Write-Host "os_wrapper.c not present - managed_components not fetched yet. Nothing to do." -ForegroundColor Yellow
    exit 0
}

if ((Get-Content $target -Raw) -match $marker) {
    Write-Host "already patched (marker present) - no action." -ForegroundColor Green
    exit 0
}

# Version guard: refuse to clobber a file that is not the shape we patched.
$cur = Get-Content $target -Raw
if ($cur -notmatch 'hosted_destroy_semaphore' -or $cur -notmatch 'Uninitialized sem id 4') {
    throw "os_wrapper.c does not look like the esp_hosted version this patch was built against. Refusing to overwrite - re-derive the patch by hand."
}

Copy-Item $target "$target.qmx-backup" -Force
Copy-Item $stored $target -Force
Write-Host "patched os_wrapper.c (pristine saved as os_wrapper.c.qmx-backup)" -ForegroundColor Green
