# bench.ps1 - one front door for a bench with four boards on it.
#
# WHY THIS EXISTS
#   Four ESP32-P4 boards share this machine and all four enumerate as the same
#   VID_303A&PID_1001, so nothing in the USB descriptor tells them apart. The
#   COM number follows the SOCKET, not the board. That has already cost one
#   board: it took a Tab5 binary at 94% and boot-looped, because VID_303A was
#   read as "must be the Tab5".
#   So: every port here comes from tools/bench.json by bench NAME, never from
#   auto-detection, and a flash is verified afterwards against the MAC the
#   firmware prints in its own boot header.
#
#   The CPU is 2 cores. One build saturates it; two thrash and starve whatever
#   else is running. The lock is not politeness, it is the scheduler.
#
# USAGE
#   bench list                 what exists, what is plugged in, what is running
#   bench status <name>        ask the running firmware over the network
#   bench capture <name>       start the standing serial capture (leave it up)
#   bench stopcapture <name>   the only legitimate reason is a flash
#   bench build [<name>]       build that bench's tree, under the lock
#   bench flash <name>         lock, stop capture, flash, restart capture, verify
#   bench verify <name>        re-check the MAC in the latest boot header
#   bench antenna [<name>]     show / record which bench is on the antenna
#   bench lock | unlock | who  manual lock control
#
# Usable from any worktree: it reads the registry by absolute path.

param(
    [Parameter(Position = 0)] [string] $Command = "list",
    [Parameter(Position = 1)] [string] $Name    = "",
    [switch] $Force
)

$ErrorActionPreference = "Stop"

$RegistryPath = "C:/dev/qmx-panadapter/tools/bench.json"
$AntennaPath  = "C:/dev/bench.antenna"

function Read-Registry {
    if (-not (Test-Path $RegistryPath)) {
        throw "Registry missing: $RegistryPath"
    }
    return (Get-Content $RegistryPath -Raw | ConvertFrom-Json)
}

function Get-Bench {
    param($reg, [string] $name)
    if (-not $name) { throw "This command needs a bench name. Try: bench list" }
    $b = $reg.benches | Where-Object { $_.name -eq $name }
    if (-not $b) {
        $all = ($reg.benches | ForEach-Object { $_.name }) -join ", "
        throw "No bench called '$name'. Known: $all"
    }
    return $b
}

function Get-PresentPorts {
    return [System.IO.Ports.SerialPort]::GetPortNames()
}

# Every P4-class board on this machine, present or merely remembered. Shown on
# 'list' because a REMEMBERED port is exactly what makes a COM number look free
# when it is not.
function Get-EspPorts {
    $out = @()
    try {
        Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_303A' } |
            ForEach-Object {
                $com = ""
                if ($_.FriendlyName -match '\((COM\d+)\)') { $com = $Matches[1] }
                $out += [pscustomobject]@{ Com = $com; Present = $_.Present }
            }
    } catch { }
    return $out
}

function Get-CaptureProcess {
    param([string] $outFile)
    $procs = @()
    try {
        Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
            ForEach-Object {
                if ($_.CommandLine -and $_.CommandLine -match 'cap_serial_reboot\.ps1') {
                    if (-not $outFile -or $_.CommandLine -replace '/', '\' -match [regex]::Escape(($outFile -replace '/', '\'))) {
                        $procs += $_
                    }
                }
            }
    } catch { }
    return $procs
}

function Test-CaptureFresh {
    param([string] $path)
    # A capture that has EXPIRED looks exactly like a quiet, healthy device.
    # Liveness is the process AND a file written seconds ago - never just one.
    if (-not (Test-Path $path)) { return $false }
    $age = (New-TimeSpan -Start (Get-Item $path).LastWriteTime -End (Get-Date)).TotalSeconds
    return ($age -lt 30)
}

# ---------------------------------------------------------------- lock

function Get-LockPath { param($reg) return $reg.lock_file }

function Read-Lock {
    param($reg)
    $p = Get-LockPath $reg
    if (-not (Test-Path $p)) { return $null }
    try { return (Get-Content $p -Raw | ConvertFrom-Json) } catch { return $null }
}

function Test-LockStale {
    param($lock)
    # A lock whose process is gone is rubble from a crashed session, not a
    # claim. Clearing it automatically is safe; refusing forever is not.
    if (-not $lock) { return $false }
    if (-not $lock.pid) { return $true }
    $p = Get-Process -Id $lock.pid -ErrorAction SilentlyContinue
    return ($null -eq $p)
}

function Take-Lock {
    param($reg, [string] $what, [string] $benchName)
    $p = Get-LockPath $reg
    $lock = Read-Lock $reg
    if ($lock -and -not (Test-LockStale $lock)) {
        if ($lock.pid -eq $PID) { return $false }   # already ours, do not re-take
        throw ("Build/flash lock is held: '{0}' on bench '{1}' (pid {2}) since {3}.`n" -f `
               $lock.what, $lock.bench, $lock.pid, $lock.since) +
              "This machine has 2 CPU cores - a second build does not run alongside, it starves both. Wait, or 'bench unlock' if you know that session is gone."
    }
    if ($lock) { Write-Host "Clearing a stale lock from pid $($lock.pid)." -ForegroundColor DarkYellow }
    $obj = [pscustomobject]@{
        what  = $what
        bench = $benchName
        pid   = $PID
        since = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
    }
    $obj | ConvertTo-Json | Set-Content -Path $p -Encoding utf8
    return $true
}

function Release-Lock {
    param($reg)
    $p = Get-LockPath $reg
    if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------- commands

function Cmd-List {
    param($reg)
    $present = Get-PresentPorts
    Write-Host ""
    Write-Host "BENCHES" -ForegroundColor Cyan
    foreach ($b in $reg.benches) {
        $plugged = "-"
        if ($b.com -and $present -contains $b.com) { $plugged = "plugged" }
        elseif ($b.com -and $b.com -ne "UNASSIGNED") { $plugged = "absent" }
        $cap = "no capture"
        if (Test-CaptureFresh $b.capture) { $cap = "capture LIVE" }
        elseif (Test-Path $b.capture) { $cap = "capture STALE" }
        $flash = ""
        if ($b.usb_flash -ne "yes") { $flash = "  [USB-FLASH BLOCKED]" }
        Write-Host ("  {0,-6} {1,-9} {2,-10} {3,-13} {4,-12} {5}{6}" -f `
            $b.name, $b.com, $plugged, $cap, $b.radio, $b.board, $flash)
    }
    Write-Host ""
    Write-Host "ESP32-P4 COM NUMBERS ON THIS MACHINE (remembered ones still hold their number)" -ForegroundColor Cyan
    foreach ($e in (Get-EspPorts | Sort-Object Com)) {
        $state = "remembered"
        if ($e.Present) { $state = "PRESENT" }
        Write-Host ("  {0,-8} {1}" -f $e.Com, $state)
    }
    Write-Host ""
    $lock = Read-Lock $reg
    if ($lock -and -not (Test-LockStale $lock)) {
        Write-Host ("LOCK: {0} on '{1}' (pid {2}) since {3}" -f $lock.what, $lock.bench, $lock.pid, $lock.since) -ForegroundColor Yellow
    } else {
        Write-Host "LOCK: free" -ForegroundColor DarkGray
    }
    Cmd-Antenna $reg ""
}

function Cmd-Status {
    param($reg, $b)
    if (-not $b.ip -or $b.ip -eq "UNKNOWN") { Write-Host "No IP recorded for '$($b.name)'."; return }
    try {
        $r = Invoke-WebRequest -Uri "http://$($b.ip)/api/status" -TimeoutSec 6 -UseBasicParsing
        $j = $r.Content | ConvertFrom-Json
        Write-Host ("{0}: {1}   screen={2}  radio={3}  {4}" -f `
            $b.name, $j.tab5_fw, $j.screen, $j.qmx_fw, $j.mode) -ForegroundColor Green
        if ($j.update) {
            Write-Host ("  update: running={0} latest={1} available={2}" -f `
                $j.update.running, $j.update.latest, $j.update.available) -ForegroundColor DarkGray
        }
    } catch {
        Write-Host "$($b.name) at $($b.ip): no answer ($($_.Exception.Message))" -ForegroundColor DarkYellow
    }
}

function Cmd-Capture {
    param($reg, $b)
    if (-not $b.com -or $b.com -eq "UNASSIGNED") { throw "Bench '$($b.name)' has no COM port assigned yet." }
    if ((Get-CaptureProcess $b.capture).Count -gt 0 -and -not $Force) {
        Write-Host "A capture for '$($b.name)' is already running. Leave it alone (-Force to start another anyway)." -ForegroundColor Yellow
        return
    }
    if (-not ((Get-PresentPorts) -contains $b.com)) {
        throw "$($b.com) is not present - is bench '$($b.name)' plugged in?"
    }
    # No -Reset. Opening the port already reboots the board once; -Reset would
    # do it deliberately and that is how the phantom "double boot" was born.
    Start-Process powershell -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $reg.capture_script,
        "-Port", $b.com, "-Out", $b.capture, "-Seconds", "$($reg.capture_seconds)"
    ) -WindowStyle Hidden
    Start-Sleep -Seconds 3
    if (Test-CaptureFresh $b.capture) {
        Write-Host "Capture running for '$($b.name)' on $($b.com) -> $($b.capture)" -ForegroundColor Green
    } else {
        Write-Host "Capture started but the file is not growing yet. Check again in a few seconds." -ForegroundColor DarkYellow
    }
}

function Cmd-StopCapture {
    param($reg, $b)
    $procs = Get-CaptureProcess $b.capture
    if ($procs.Count -eq 0) { Write-Host "No capture running for '$($b.name)'."; return }
    foreach ($p in $procs) {
        try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch { }
    }
    Write-Host "Stopped $($procs.Count) capture process(es) for '$($b.name)'. RESTART IT when you are done." -ForegroundColor Yellow
}

function Invoke-Idf {
    param($reg, [string] $tree, [string[]] $idfArgs)
    if (-not $env:IDF_PATH) { & $reg.idf_export | Out-Null }
    Push-Location $tree
    try {
        & idf.py @idfArgs
        return $LASTEXITCODE
    } finally { Pop-Location }
}

function Cmd-Build {
    param($reg, $b)
    if (-not $b.tree -or $b.tree -eq "n/a") { throw "Bench '$($b.name)' has no source tree." }
    $took = Take-Lock $reg "build" $b.name
    try {
        Write-Host "Building $($b.tree) for bench '$($b.name)'..." -ForegroundColor Cyan
        $rc = Invoke-Idf $reg $b.tree @("build")
        if ($rc -ne 0) { Write-Host "Build FAILED (exit $rc)" -ForegroundColor Red } else { Write-Host "Build OK" -ForegroundColor Green }
    } finally { if ($took) { Release-Lock $reg } }
}

function Cmd-Flash {
    param($reg, $b)
    if ($b.usb_flash -ne "yes") {
        throw "Bench '$($b.name)' is marked NOT USB-flashable.`n$($b.usb_flash_reason)`nIf you really mean it, edit tools/bench.json first - deliberately."
    }
    if (-not $b.com -or $b.com -eq "UNASSIGNED") { throw "Bench '$($b.name)' has no COM port assigned yet." }
    if (-not ((Get-PresentPorts) -contains $b.com)) { throw "$($b.com) is not present - is bench '$($b.name)' plugged in?" }
    if ($b.warning) { Write-Host "NOTE: $($b.warning)" -ForegroundColor Yellow }

    # What are we about to overwrite? Worth one line, because "which build is on
    # that board" has been wrong in writing before.
    Cmd-Status $reg $b

    $took = Take-Lock $reg "flash" $b.name
    $hadCapture = ((Get-CaptureProcess $b.capture).Count -gt 0)
    try {
        if ($hadCapture) { Cmd-StopCapture $reg $b; Start-Sleep -Seconds 1 }
        $rc = Invoke-Idf $reg $b.tree @("-p", $b.com, "flash")
        if ($rc -ne 0) { Write-Host "Flash FAILED (exit $rc)" -ForegroundColor Red }
    } finally {
        # Restarting the capture is a finally block, not a step - the crash you
        # care about lands in the window where nobody was watching.
        if ($hadCapture) {
            try { Cmd-Capture $reg $b } catch { Write-Host "COULD NOT RESTART CAPTURE: $($_.Exception.Message)" -ForegroundColor Red }
        }
        if ($took) { Release-Lock $reg }
    }
    Start-Sleep -Seconds 8
    Cmd-Verify $reg $b
}

function Cmd-Verify {
    param($reg, $b)
    if (-not (Test-Path $b.capture)) { Write-Host "No capture file to verify against."; return }
    $line = Select-String -Path $b.capture -Pattern 'serial\(MAC\)=([0-9A-Fa-f:]{17})' -AllMatches |
            Select-Object -Last 1
    if (-not $line) {
        Write-Host "No 'serial(MAC)=' boot header in the capture yet - cannot verify identity. It is printed at boot only." -ForegroundColor DarkYellow
        return
    }
    $mac = ($line.Matches | Select-Object -Last 1).Groups[1].Value.ToUpper()
    if ($b.mac -like "UNKNOWN*") {
        Write-Host "Bench '$($b.name)' has no MAC recorded. The board that just booted is $mac - put that in tools/bench.json." -ForegroundColor Cyan
        return
    }
    if ($mac -eq $b.mac.ToUpper()) {
        Write-Host "Identity OK: $mac is bench '$($b.name)'." -ForegroundColor Green
    } else {
        Write-Host "*** WRONG BOARD *** capture says $mac, registry says bench '$($b.name)' is $($b.mac). Check which board is on $($b.com) before doing anything else." -ForegroundColor Red
    }
}

function Cmd-Antenna {
    param($reg, [string] $name)
    if ($name) {
        Set-Content -Path $AntennaPath -Value $name -Encoding utf8
        Write-Host "Antenna recorded as: bench '$name'." -ForegroundColor Green
        return
    }
    $cur = "not recorded"
    if (Test-Path $AntennaPath) { $cur = (Get-Content $AntennaPath -Raw).Trim() }
    Write-Host "ANTENNA: $cur" -ForegroundColor Cyan
    Write-Host "  Decode counts, SNR and noise floor from any OTHER bench are meaningless." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- dispatch

$reg = Read-Registry

switch ($Command.ToLower()) {
    "list"         { Cmd-List $reg }
    "status"       { Cmd-Status       $reg (Get-Bench $reg $Name) }
    "capture"      { Cmd-Capture      $reg (Get-Bench $reg $Name) }
    "stopcapture"  { Cmd-StopCapture  $reg (Get-Bench $reg $Name) }
    "build"        { if (-not $Name) { $Name = "dev" }; Cmd-Build $reg (Get-Bench $reg $Name) }
    "flash"        { Cmd-Flash        $reg (Get-Bench $reg $Name) }
    "verify"       { Cmd-Verify       $reg (Get-Bench $reg $Name) }
    "antenna"      { Cmd-Antenna      $reg $Name }
    "lock"         { [void](Take-Lock $reg "manual" $Name); Write-Host "Lock taken by pid $PID." -ForegroundColor Green }
    "unlock"       { Release-Lock $reg; Write-Host "Lock released." -ForegroundColor Green }
    "who"          {
        $l = Read-Lock $reg
        if ($l -and -not (Test-LockStale $l)) { Write-Host ("{0} on '{1}' (pid {2}) since {3}" -f $l.what, $l.bench, $l.pid, $l.since) }
        else { Write-Host "Lock is free." }
    }
    default        { Write-Host "Unknown command '$Command'. Try: list status capture stopcapture build flash verify antenna lock unlock who" -ForegroundColor Yellow }
}
