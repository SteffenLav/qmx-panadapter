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
        [string] $Bench  = "dev"
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
            Push-Location "$tree\build"
            python -m esptool --chip esp32p4 merge_bin `
                -o qmx_panadapter_merged.bin `
                --flash_mode dio --flash_size 16MB --flash_freq 40m `
                0x2000  bootloader\bootloader.bin `
                0x8000  partition_table\partition-table.bin `
                0x10000 qmx_panadapter.bin
            Pop-Location
            if ($LASTEXITCODE -eq 0) {
                $sz = (Get-Item "$tree\build\qmx_panadapter_merged.bin").Length
                Write-Host ("Merged binary: build\qmx_panadapter_merged.bin ({0:N0} bytes)" -f $sz) -ForegroundColor Green
            }
        }
        default { Write-Host "Usage: qmx [b|f|m|fm|bfm|merge] [benchName]" -ForegroundColor Yellow }
    }
}

function ss {
    param([string] $Bench = "dev")
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
