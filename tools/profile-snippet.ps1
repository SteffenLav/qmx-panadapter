# Shell helpers for the four-board bench. VERSIONED HERE, not in $PROFILE —
# the profile should hold one line, so the logic can be edited, reviewed and
# committed like everything else:
#
#     . C:\dev\qmx-panadapter\tools\profile-snippet.ps1
#
# Everything below resolves ports and trees from tools/bench.json by BENCH
# NAME. Nothing here auto-detects a COM port; see docs/bench-setup.md for why
# that rule exists and what it already cost.

function bench {
    powershell -NoProfile -ExecutionPolicy Bypass -File "C:\dev\qmx-panadapter\tools\bench.ps1" @args
}

# qmx — unchanged verbs, but the port now comes from the registry instead of
# being hardcoded in six places. `qmx b|f|m|fm|bfm` still means the dev bench;
# pass a bench name as the second argument for any other board.
function qmx {
    param(
        [string] $Action = "fm",
        [string] $Bench  = "bench1"
    )

    $reg = Get-Content "C:\dev\qmx-panadapter\tools\bench.json" -Raw | ConvertFrom-Json
    $b   = $reg.benches | Where-Object { $_.name -eq $Bench }
    if (-not $b) {
        Write-Host "No bench called '$Bench'. Try: bench list" -ForegroundColor Yellow
        return
    }
    if ($b.com -eq "UNASSIGNED") {
        Write-Host "Bench '$Bench' has no COM port assigned yet (see docs/bench-setup.md §3)." -ForegroundColor Yellow
        return
    }

    $port = $b.com
    $tree = $b.tree
    if (-not $env:IDF_PATH) {
        Write-Host "Activating IDF..." -ForegroundColor DarkGray
        & $reg.idf_export | Out-Null
    }
    Set-Location $tree

    # The monitor is run directly rather than through idf.py to avoid the
    # orphan-Python problem. Note it ALWAYS misses the boot after a flash —
    # for boot lines read the bench's standing capture instead.
    $mon = {
        param($p, $t)
        python -m esp_idf_monitor -p $p -b 921600 --toolchain-prefix riscv32-esp-elf- --target esp32p4 "$t\build\qmx_panadapter.elf"
    }

    switch ($Action) {
        "b"   { bench build $Bench }
        "f"   { bench flash $Bench }
        "m"   { & $mon $port $tree }
        "fm"  { bench flash $Bench; if ($LASTEXITCODE -eq 0) { & $mon $port $tree } }
        "bfm" {
            bench build $Bench
            if ($LASTEXITCODE -eq 0) {
                bench flash $Bench
                if ($LASTEXITCODE -eq 0) { & $mon $port $tree }
            }
        }
        "merge" {
            # REMOVED 2026-10-22, on the operator's instruction: "block the
            # building of it as it will never be used going forward".
            #
            # A merged image is a WHOLE-CHIP file whose byte 0 is flash
            # address 0x0. Nothing ships it: flash.bat writes bootloader.bin,
            # partition-table.bin, qmx_panadapter.bin and ota_data_initial.bin
            # separately at their own offsets, and the flasher zip contains no
            # merged file. It has only ever caused harm - v0.18.6 shipped one,
            # flash.bat wrote it at the wrong offset, and it bricked a unit on
            # every use (memory: project_merged_bin_offset_bug).
            #
            # MEASURED on the v1.20.4 file, 2026-10-22: a merge reaching
            # 0x920000 spans 0x0..0x922000, and its user_nvs (0x810000, 64 KB)
            # and storage (0x820000, 1 MB) regions are ALL 0xFF - blank
            # padding, not a copy. esptool erases each sector before writing,
            # so flashing it wipes WiFi credentials, callsign, memory
            # channels, the ADIF log and the LoTW private key.
            #
            # The v1.20.4 release asset was NOT built by this verb: it was a
            # hand-typed 4-way esptool command (this one is 3-way and names its
            # output without a version). Removing the verb is therefore not the
            # whole guard - check_release_assets.py refuses a merged ASSET.
            Write-Host "qmx merge is REMOVED - a merged image wipes user_nvs and storage." -ForegroundColor Red
            Write-Host "Flash the four separate files, as flash.bat does." -ForegroundColor Yellow
        }
        default { Write-Host "Usage: qmx [b|f|m|fm|bfm] [benchName]" -ForegroundColor Yellow }
    }
}

function ss {
    param([string] $Bench = "bench1")
    $reg = Get-Content "C:\dev\qmx-panadapter\tools\bench.json" -Raw | ConvertFrom-Json
    $b   = $reg.benches | Where-Object { $_.name -eq $Bench }
    if (-not $b -or $b.com -eq "UNASSIGNED") { Write-Host "No usable port for bench '$Bench'." -ForegroundColor Yellow; return }
    python C:\dev\qmx-panadapter\tools\screenshot_decode.py $b.com
}

function idfenv {
    & "C:\esp\v5.4.4\esp-idf\export.ps1"
    Set-Location "C:\dev\qmx-panadapter"
    Write-Host ""
    Write-Host "ESP-IDF v5.4.4 ready, in qmx-panadapter" -ForegroundColor Green
}

function kpy {
    Get-Process python -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host 'Python processes killed.' -ForegroundColor Yellow
}
