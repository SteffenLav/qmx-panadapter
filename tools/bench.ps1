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
#   bench standdown <name>     "release COMx and stand down": kill + keep the watchdog off it
#                              until `bench capture <name>` (alias: bench release)
#   bench watchdog [off|status]  keep captures alive (every minute); see bench_watchdog.ps1
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
$OverlayPath  = "C:/dev/bench.local.json"   # private, outside every repo

function Read-Registry {
    if (-not (Test-Path $RegistryPath)) {
        throw "Registry missing: $RegistryPath"
    }
    $reg = (Get-Content $RegistryPath -Raw | ConvertFrom-Json)

    # ⭐ PRIVATE OVERLAY. A bench on a board that is not public has no
    # business being described in a public registry - which is exactly the
    # mistake this replaces. If the overlay exists its `benches` are merged
    # (replacing a same-named entry), so a private board is fully drivable
    # while this file says nothing about it. It lives outside every repo.
    if (Test-Path $OverlayPath) {
        $ov = (Get-Content $OverlayPath -Raw | ConvertFrom-Json)
        if ($ov.benches) {
            $names = @($ov.benches | ForEach-Object { $_.name })
            $kept  = @($reg.benches | Where-Object { $names -notcontains $_.name })
            $reg.benches = @($kept + $ov.benches)
        }
    }
    return $reg
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
    param([string] $outFile, [string] $com = "")
    $procs = @()
    try {
        Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
            ForEach-Object {
                if ($_.CommandLine -and $_.CommandLine -match 'cap_serial_reboot\.ps1') {
                    $byFile = (-not $outFile) -or ($_.CommandLine -replace '/', '\' -match [regex]::Escape(($outFile -replace '/', '\')))
                    # By PORT as well: a capture started by hand with a different
                    # -Out still holds the same COM, and that is what blocks a flash.
                    $byPort = $com -and ($_.CommandLine -match ("-Port\s+" + [regex]::Escape($com) + "\b"))
                    if ($byFile -or $byPort) { $procs += $_ }
                }
            }
    } catch { }
    # ⛔ THE COMMA IS LOAD-BEARING. PowerShell UNROLLS a one-element array on
    # return, so with exactly one capture running the caller got a bare
    # CimInstance - and in Windows PowerShell 5.1 a scalar has no .Count, so
    # `(Get-CaptureProcess ...).Count` was $null.
    #
    # That is not a cosmetic bug. Cmd-Flash asked `.Count -gt 0` to decide
    # whether to stop the capture, `$null -gt 0` is false, and so a flash with
    # ONE capture running never stopped it and died on "Could not open COM5,
    # the port is busy" - twice on 2026-09-12 before anyone looked at why.
    # Cmd-StopCapture only escaped it by accident: its guard is `-eq 0`, and
    # `$null -eq 0` is false too, so it fell through to the kill loop.
    #
    # ⛔⛔ AND `,$procs` WAS THE WRONG CURE, measured 2026-09-13 in PS 5.1:
    #   function F { $p=@(); return ,$p };  @(F).Count  ->  1   (an EMPTY capture list counts as ONE)
    #   @(F)[0] -is [array]                 ->  True  (every result is nested one level)
    # Combined with @() at the call sites, "no capture" read as one capture
    # whose ProcessId is $null - the watchdog logged "could not kill pid  -
    # Cannot bind argument to parameter 'Id' because it is null", and the first
    # `bench flash` after `bench standdown` died on it. It only LOOKED fixed
    # because member enumeration makes `$nested.ProcessId` return the real PID
    # when there is at least one element.
    #
    # The correct idiom is the plain one: let PowerShell unroll, and wrap in @()
    # at EVERY call site (they all do). @() of zero, one or many is always right.
    return $procs
}

function Wait-PortFree {
    param([string] $com, [int] $timeoutSec = 20)
    # ⛔ KILLING THE CAPTURE DOES NOT FREE THE PORT SYNCHRONOUSLY.
    #
    # Cmd-Flash used to Stop-Process the capture and then Start-Sleep 1 before
    # handing COM to esptool. Windows closes a dead process's handles on its own
    # schedule, so that one second was a guess - and on 2026-09-12 it lost twice
    # in a row on the dev bench: "A fatal error occurred: Could not open COM5,
    # the port is busy or doesn't exist. (Access is denied)". Nothing was
    # written either time, which is the good outcome; the bad one is a flash
    # that starts while something else still holds the line, which is how COM9
    # ended up held through a flash on 2026-09-09.
    #
    # So TEST it instead of waiting a magic number: try to open the port, which
    # is exactly what esptool is about to do. Returns $true the moment it opens.
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $p = New-Object System.IO.Ports.SerialPort($com, 115200)
            $p.Open(); $p.Close(); $p.Dispose()
            return $true
        } catch {
            Start-Sleep -Milliseconds 300
        }
    }
    return $false
}

# ⛔ STAND-DOWN FLAG. The operator: "release com5 and stand down". Without a
# flag the watchdog (every minute) respawned the capture as soon as it was
# killed, so a new session could never get the port back - "every time it say
# respawning and took ages to kill all". The watchdog skips a bench while this
# file exists; `bench capture <name>` removes it again.
function Get-StandDownPath { param($b) return "C:/dev/bench.standdown.$($b.name)" }

# Kill every capture for this bench AND CONFIRM THEY ARE GONE. Stop-Process
# inside try/catch{} used to report "Stopped 1" whether or not the kill worked,
# and a process blocked inside a USB-serial read can outlive a plain kill.
# Returns the number still alive (0 = all gone).
function Stop-CaptureHard {
    param($b, [int] $timeoutSec = 15)
    $taskName = "qmx-capture-$($b.name)"
    Invoke-Task-Quiet "/end /tn $taskName" | Out-Null
    Invoke-Task-Quiet "/delete /tn $taskName /f" | Out-Null

    $procs = @(Get-CaptureProcess $b.capture $b.com)
    foreach ($p in $procs) {
        try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch { }
    }
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $escalated = $false
    while ((Get-Date) -lt $deadline) {
        $left = @($procs | Where-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
        if ($left.Count -eq 0) { return 0 }
        if (-not $escalated -and ((Get-Date) -gt $deadline.AddSeconds(-($timeoutSec - 3)))) {
            foreach ($p in $left) { cmd /c "taskkill /F /T /PID $($p.ProcessId) >nul 2>&1" | Out-Null }
            $escalated = $true
        }
        Start-Sleep -Milliseconds 300
    }
    return @($procs | Where-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue }).Count
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

# Run one schtasks command and hand back its exit code, with stdout and stderr
# both discarded INSIDE cmd. See the note in Cmd-Capture: under
# $ErrorActionPreference = "Stop", a native exe writing to stderr aborts this
# script, and schtasks writes to stderr for perfectly ordinary outcomes such as
# "that task does not exist".
function Invoke-Task-Quiet {
    param([string] $argLine)
    cmd /c "schtasks $argLine >nul 2>&1"
    return $LASTEXITCODE
}

function Cmd-Capture {
    param($reg, $b)
    if (-not $b.com -or $b.com -eq "UNASSIGNED") { throw "Bench '$($b.name)' has no COM port assigned yet." }
    # An explicit capture is the end of a stand-down.
    $sd = Get-StandDownPath $b
    if (Test-Path $sd) { Remove-Item $sd -Force; Write-Host "Stand-down for '$($b.name)' cleared." -ForegroundColor DarkGray }

    $existing = @(Get-CaptureProcess $b.capture $b.com)
    if ($existing.Count -gt 0 -and -not $Force) {
        # ⛔ "ALREADY RUNNING" MUST MEAN WRITING. After the flash of 2026-09-13
        # this returned early against a process that had written nothing, so the
        # capture stayed dead until the watchdog's 90 s staleness caught it - the
        # exact post-flash boot window the capture exists for. A process with a
        # stale file is a corpse: kill it and start a fresh one.
        if (Test-CaptureFresh $b.capture) {
            Write-Host "A capture for '$($b.name)' is already running and writing. Leaving it alone." -ForegroundColor Yellow
            return
        }
        Write-Host "A capture process exists for '$($b.name)' but the file is stale - replacing it." -ForegroundColor DarkYellow
    }
    if ($existing.Count -gt 0) {
        $left = Stop-CaptureHard $b
        if ($left -gt 0) { Write-Host "$left old capture process(es) would not die - the new one may log PORT LOST." -ForegroundColor Red }
        [void](Wait-PortFree $b.com 15)
    }
    if (-not ((Get-PresentPorts) -contains $b.com)) {
        throw "$($b.com) is not present - is bench '$($b.name)' plugged in?"
    }
    # No -Reset. Opening the port already reboots the board once; -Reset would
    # do it deliberately and that is how the phantom "double boot" was born.
    #
    # ⛔ LAUNCHED AS A SCHEDULED TASK, NOT Start-Process - and that is the whole
    # point of this function. A capture started with Start-Process from inside a
    # Claude Code tool call gets KILLED minutes later: three times on the night of
    # 2026-09-11, across two ports and two trees, each time with NO
    # "=== [capture] done ===" marker and no PORT LOST, i.e. terminated rather
    # than exited. One of them (COM9, 22:47:13) died five seconds before Windows
    # logged "Application Hang: powershell.exe ... was closed", in the same minute
    # the Claude desktop app stopped its own service, redeployed itself and
    # restarted - so the capture died with the app that transitively owned it.
    # Launching via WMI (Win32_Process.Create) was tried and did NOT survive
    # either. A scheduled task is owned by the task scheduler, so it outlives the
    # tool call, the session, and an app update.
    #
    # The cost of getting this wrong is not a missing log: a dead capture looks
    # EXACTLY like a quiet, healthy device (CLAUDE.md serial rule 10), so the
    # night reads as "no crashes" for a window that closed minutes after it opened.
    $taskName = "qmx-capture-$($b.name)"

    # Launched through tools/run_hidden.vbs so there is NO console window. A
    # scheduled task running as the logged-on user runs interactively, and an
    # interactive console program always gets a window; `-WindowStyle Hidden`
    # only MINIMISES it (the window is conhost's, not powershell's - which is
    # why powershell's MainWindowHandle read 0 and looked hidden while it sat on
    # the taskbar). Running the task in session 0 instead would need
    # `/ru <user> /np`, and registering that needs elevation. See run_hidden.vbs.
    $vbs = Join-Path (Split-Path -Parent $PSCommandPath) "run_hidden.vbs"
    if (-not (Test-Path $vbs)) { throw "run_hidden.vbs not found next to bench.ps1 at $vbs" }

    # schtasks' /tr takes ONE quoted string, and quotes cannot be nested inside
    # it, so every path here has to be space-free. They are, and have always
    # been - but say so plainly rather than emitting a task that silently runs
    # the wrong thing.
    foreach ($p in @($vbs, $reg.capture_script, $b.capture)) {
        if ($p -match '\s') {
            throw "Path '$p' contains a space; schtasks /tr cannot quote it. Move it somewhere without spaces."
        }
    }

    $action = "wscript.exe $vbs powershell.exe -NoProfile -ExecutionPolicy Bypass " +
              "-File $($reg.capture_script) -Port $($b.com) -Out $($b.capture) " +
              "-Seconds $($reg.capture_seconds)"

    # Delete any previous definition first: /f on create replaces it, but an
    # already-RUNNING instance of the old task would keep the port and the new
    # one would log PORT LOST and record nothing (serial rule 3).
    #
    # ⛔ Every schtasks call goes through cmd /c with its output swallowed there.
    # This script runs under $ErrorActionPreference = "Stop", and in Windows
    # PowerShell 5.1 a native exe writing to stderr becomes a NativeCommandError
    # that ABORTS the script - so "/end" on a task that does not exist yet, which
    # is the normal first-run case, killed the whole function.
    Invoke-Task-Quiet "/end /tn $taskName" | Out-Null
    Invoke-Task-Quiet "/delete /tn $taskName /f" | Out-Null

    # ⛔ No /rl highest and no /ru: BOTH need elevation to register, and this
    # script is run from an ordinary shell, so schtasks answered "Access is
    # denied". The capture only opens a COM port - it has never needed admin.
    # /st is in the future purely to stop schtasks warning that a once-task with
    # a past start time may not run; the task is started by /run immediately
    # afterwards and the trigger time is never reached.
    $rc = Invoke-Task-Quiet "/create /tn $taskName /sc once /st 23:59 /f /tr `"$action`""
    if ($rc -ne 0) {
        Write-Host "schtasks /create failed (exit $rc) for '$taskName'." -ForegroundColor Red
        Write-Host "  Run it by hand to see why: schtasks /create /tn $taskName ..." -ForegroundColor DarkGray
        throw "Could not create the capture task."
    }
    $rc = Invoke-Task-Quiet "/run /tn $taskName"
    if ($rc -ne 0) { throw "schtasks /run failed (exit $rc) for '$taskName'." }

    # ⛔ DO NOT RETURN UNTIL THE FILE IS ACTUALLY GROWING, AND RETRY IF IT IS NOT.
    #
    # This used to wait 4 s and, on a miss, print a mild "check again in a few
    # seconds" - which is a capture that MIGHT be running, reported as a
    # non-problem. On 2026-09-12 a capture stopped at a flash and stayed dead
    # for 22 minutes; the operator hit a full UI freeze inside that window and
    # there was no log of it. A capture that did not start is not a cosmetic
    # failure, it is the difference between diagnosing the next crash and
    # guessing at it.
    #
    # 4 s is also simply too short when the board is mid-boot after a flash, so
    # this waits up to 20 s, then re-runs the task ONCE before giving up loudly.
    $fresh = $false
    foreach ($attempt in 1..2) {
        for ($i = 0; $i -lt 10; $i++) {
            Start-Sleep -Seconds 2
            if (Test-CaptureFresh $b.capture) { $fresh = $true; break }
        }
        if ($fresh) { break }
        if ($attempt -eq 1) {
            Write-Host "Capture produced nothing in 20 s - re-running the task once." -ForegroundColor DarkYellow
            Invoke-Task-Quiet "/run /tn $taskName" | Out-Null
        }
    }

    if ($fresh) {
        Write-Host "Capture running for '$($b.name)' on $($b.com) -> $($b.capture)" -ForegroundColor Green
        Write-Host "  (scheduled task '$taskName' - survives this session and app updates)" -ForegroundColor DarkGray
    } else {
        Write-Host "CAPTURE IS NOT RUNNING for '$($b.name)' - the next crash will NOT be recorded." -ForegroundColor Red
        Write-Host "  Diagnose: schtasks /query /tn $taskName /v /fo list" -ForegroundColor DarkGray
        Write-Host "  Port in use by something else? $($b.com)" -ForegroundColor DarkGray
    }
}

function Cmd-Watchdog {
    param($reg, [string] $mode)
    # Keeps every configured capture alive. See tools/bench_watchdog.ps1 for
    # why this exists and what it refuses to do during a flash.
    $taskName = "qmx-capture-watchdog"
    $vbs = Join-Path (Split-Path -Parent $PSCommandPath) "run_hidden.vbs"
    $wd  = Join-Path (Split-Path -Parent $PSCommandPath) "bench_watchdog.ps1"

    switch ($mode) {
        "off" {
            Invoke-Task-Quiet "/end /tn $taskName" | Out-Null
            Invoke-Task-Quiet "/delete /tn $taskName /f" | Out-Null
            Write-Host "Capture watchdog removed. Nothing will restart a dead capture now." -ForegroundColor Yellow
        }
        "status" {
            $out = cmd /c "schtasks /query /tn $taskName /fo list 2>&1"
            if ($LASTEXITCODE -ne 0) {
                Write-Host "Capture watchdog: NOT INSTALLED" -ForegroundColor Yellow
            } else {
                Write-Host "Capture watchdog: installed" -ForegroundColor Green
                ($out | Select-String "Status|Next Run Time") | ForEach-Object { "  $_" }
            }
            $log = "C:/dev/qmx-panadapter/scratchpad/bench-watchdog.log"
            if (Test-Path $log) {
                Write-Host "  recent activity:" -ForegroundColor DarkGray
                Get-Content $log -Tail 5 | ForEach-Object { "    $_" }
            }
        }
        default {
            foreach ($p in @($vbs, $wd)) {
                if ($p -match '\s') { throw "Path '$p' contains a space; schtasks /tr cannot quote it." }
            }
            $action = "wscript.exe $vbs powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wd"
            Invoke-Task-Quiet "/end /tn $taskName" | Out-Null
            Invoke-Task-Quiet "/delete /tn $taskName /f" | Out-Null
            # Every minute, for ever. /du 9999:59 because schtasks defaults a
            # minute-schedule to a single day and would stop overnight - which
            # is exactly the run you most want covered.
            $rc = Invoke-Task-Quiet "/create /tn $taskName /sc minute /mo 1 /du 9999:59 /f /tr `"$action`""
            if ($rc -ne 0) { throw "schtasks /create failed (exit $rc) for '$taskName'." }
            Invoke-Task-Quiet "/run /tn $taskName" | Out-Null
            Write-Host "Capture watchdog installed - checks every minute, restarts any capture that stopped." -ForegroundColor Green
            Write-Host "  Skips entirely while the bench lock is held, so it cannot interfere with a flash." -ForegroundColor DarkGray
            Write-Host "  Log: C:/dev/qmx-panadapter/scratchpad/bench-watchdog.log" -ForegroundColor DarkGray
        }
    }
}

function Cmd-StopCapture {
    param($reg, $b)
    # End the scheduled task as well as the process. Killing only the process
    # leaves the task defined and "ready", which reads as a live capture in
    # schtasks /query and invites someone to think one is running when it is not.
    $n = @(Get-CaptureProcess $b.capture $b.com).Count
    if ($n -eq 0) {
        Invoke-Task-Quiet "/end /tn qmx-capture-$($b.name)" | Out-Null
        Invoke-Task-Quiet "/delete /tn qmx-capture-$($b.name) /f" | Out-Null
        Write-Host "No capture process running for '$($b.name)'."
        return
    }
    $left = Stop-CaptureHard $b
    if ($left -gt 0) {
        Write-Host "$left of $n capture process(es) for '$($b.name)' are STILL ALIVE after kill + taskkill." -ForegroundColor Red
    } else {
        Write-Host "Stopped $n capture process(es) for '$($b.name)'. RESTART IT when you are done." -ForegroundColor Yellow
    }
}

# `bench standdown <name>` - the operator's "release com5 and stand down".
# Stops the watchdog from respawning it, kills every capture for that port and
# confirms they died, removes any read-only `tail -f` viewers other sessions
# left on the capture file, and does not return "done" until the COM port
# actually OPENS. Undo with `bench capture <name>`.
function Cmd-StandDown {
    param($reg, $b)
    Set-Content -Path (Get-StandDownPath $b) -Value (Get-Date -Format "yyyy-MM-dd HH:mm:ss") -Encoding utf8
    Write-Host "Stand-down set for '$($b.name)' - the watchdog will not restart its capture." -ForegroundColor Cyan

    $n = @(Get-CaptureProcess $b.capture $b.com).Count
    $left = Stop-CaptureHard $b
    Write-Host ("Capture processes: {0} found, {1} still alive." -f $n, $left) -ForegroundColor $(if ($left -gt 0) { "Red" } else { "Green" })

    $leaf = [regex]::Escape((Split-Path $b.capture -Leaf))
    $viewers = @(Get-CimInstance Win32_Process -Filter "Name='tail.exe'" -ErrorAction SilentlyContinue |
                 Where-Object { $_.CommandLine -match $leaf })
    foreach ($v in $viewers) { try { Stop-Process -Id $v.ProcessId -Force -ErrorAction Stop } catch { } }
    if ($viewers.Count -gt 0) { Write-Host "Removed $($viewers.Count) leftover tail viewer(s) of $(Split-Path $b.capture -Leaf)." -ForegroundColor DarkGray }

    if ($b.com -and $b.com -ne "UNASSIGNED" -and ((Get-PresentPorts) -contains $b.com)) {
        if (Wait-PortFree $b.com 20) {
            Write-Host "$($b.com) is FREE - opened and closed it just now." -ForegroundColor Green
        } else {
            Write-Host "$($b.com) is STILL HELD after 20 s by something that is not a bench capture." -ForegroundColor Red
        }
    } else {
        Write-Host "$($b.com) is not present - nothing holds it." -ForegroundColor DarkGray
    }
}

function Invoke-Idf {
    param($reg, [string] $tree, [string[]] $idfArgs)
    if (-not $env:IDF_PATH) { & $reg.idf_export | Out-Null }
    Push-Location $tree
    try {
        # Out-Host, NOT a bare call. Anything idf.py writes to stdout otherwise
        # joins this function's RETURN VALUE, so the caller's `$rc` becomes the
        # whole output array and `$rc -ne 0` is true even on a clean build -
        # which is exactly what happened the first time this ran: a successful
        # build reported "Build FAILED". A tool that cries wolf on its first
        # use is worse than no tool.
        & idf.py @idfArgs | Out-Host
        return $LASTEXITCODE
    } finally { Pop-Location }
}

function Get-IdfArgs {
    # Per-bench build dir / sdkconfig. A bench with none behaves exactly as
    # before, so the Tab5 benches are untouched.
    param($b, [string[]] $tail)
    $a = @()
    if ($b.build_dir)         { $a += @("-B", $b.build_dir) }
    if ($b.sdkconfig)         { $a += @("-D", "SDKCONFIG=$($b.sdkconfig)") }
    if ($b.sdkconfig_defaults){ $a += @("-D", "SDKCONFIG_DEFAULTS=$($b.sdkconfig_defaults)") }
    return $a + $tail
}

function Assert-BoardMatches {
    # ⛔ THE GUARD THAT WAS MISSING. `idf.py -B <dir> flash` with no
    # -D SDKCONFIG= builds the ROOT sdkconfig - on a fork's tree that can be
    # a DIFFERENT BOARD's config - and flashes it happily. This board has already been
    # bricked that way once (see the bench's own `warning`). One grep of the
    # generated header is the whole check, and it is the same one
    # docs/PORT_NOTES.md tells a human to run.
    param($b)
    if (-not $b.expect_board_macro) { return }
    # No ternary here: Windows PowerShell 5.1 has none (CLAUDE.md).
    $bd = "build"
    if ($b.build_dir) { $bd = $b.build_dir }
    $hdr = Join-Path $b.tree (Join-Path $bd "config/sdkconfig.h")
    if (-not (Test-Path $hdr)) { throw "Cannot verify the board: $hdr does not exist. Build first." }
    $want = "#define $($b.expect_board_macro) 1"
    if (-not (Select-String -Path $hdr -SimpleMatch $want -Quiet)) {
        throw ("REFUSING TO FLASH bench '$($b.name)'.`n" +
               "  $hdr does not define $($b.expect_board_macro).`n" +
               "  That build is for a DIFFERENT BOARD - flashing it is the documented brick.`n" +
               "  Rebuild with: idf.py -B $($b.build_dir) -D SDKCONFIG=$($b.sdkconfig) build")
    }
    Write-Host "board check OK: $($b.expect_board_macro) is set in this build." -ForegroundColor Green
}

function Cmd-Build {
    param($reg, $b)
    if (-not $b.tree -or $b.tree -eq "n/a") { throw "Bench '$($b.name)' has no source tree." }
    $took = Take-Lock $reg "build" $b.name
    try {
        Write-Host "Building $($b.tree) for bench '$($b.name)'..." -ForegroundColor Cyan
        $rc = Invoke-Idf $reg $b.tree (Get-IdfArgs $b @("build"))
        if ($rc -ne 0) { Write-Host "Build FAILED (exit $rc)" -ForegroundColor Red } else { Write-Host "Build OK" -ForegroundColor Green }
    } finally { if ($took) { Release-Lock $reg } }
}

# ⚠ WSPR's transmit state is on /api/wspr and FT8's is on /api/status. The first
# version of this asked /api/status for tx_state, which is not there - so it
# would have returned "not transmitting" every single time and looked like a
# working guard. Checked against the live device before trusting it.
function Get-Json {
    param([string] $url)
    try {
        return (Invoke-WebRequest -Uri $url -TimeoutSec 5 -UseBasicParsing).Content | ConvertFrom-Json
    } catch { return $null }
}

function Assert-NotTransmitting {
    param($b)
    if ($Force) {
        Write-Host "-Force: not checking whether '$($b.name)' is transmitting." -ForegroundColor DarkYellow
        return
    }
    if (-not $b.ip -or $b.ip -eq "UNKNOWN") { return }

    $reached = $false

    $w = Get-Json "http://$($b.ip)/api/wspr"
    if ($w) {
        $reached = $true
        if ($w.tx_state -eq "active") {
            throw ("Bench '$($b.name)' is TRANSMITTING right now (WSPR burst active).`n" +
                   "A flash resets the Tab5 mid-burst, so RX; is never sent and the QMX stays KEYED`n" +
                   "into the antenna until someone power-cycles it. WSPR keys for ~110 s of every 120,`n" +
                   "so most moments during a beacon session are inside a burst.`n" +
                   "Wait for it to end, or turn TX off first. -Force overrides.")
        }
    }

    $s = Get-Json "http://$($b.ip)/api/status"
    if ($s) {
        $reached = $true
        if ($s.ft8 -and $s.ft8.st -eq "active") {
            throw ("Bench '$($b.name)' is TRANSMITTING right now (FT8 burst active).`n" +
                   "Same reason: the flash resets the Tab5 before RX; is sent. -Force overrides.")
        }
    }

    if (-not $reached) {
        Write-Host "Could not ask '$($b.name)' whether it is transmitting." -ForegroundColor DarkYellow
        Write-Host "  Proceeding - an unreachable board is often exactly why it is being reflashed." -ForegroundColor DarkGray
    }
}

function Cmd-Flash {
    param($reg, $b)
    if ($b.usb_flash -ne "yes") {
        throw "Bench '$($b.name)' is marked NOT USB-flashable.`n$($b.usb_flash_reason)`nIf you really mean it, edit tools/bench.json first - deliberately."
    }
    if (-not $b.com -or $b.com -eq "UNASSIGNED") { throw "Bench '$($b.name)' has no COM port assigned yet." }
    if (-not ((Get-PresentPorts) -contains $b.com)) { throw "$($b.com) is not present - is bench '$($b.name)' plugged in?" }
    if ($b.warning) { Write-Host "NOTE: $($b.warning)" -ForegroundColor Yellow }

    # ⛔ NEVER FLASH A RADIO THAT IS TRANSMITTING.
    #
    # A flash is a reset, so the firmware never reaches the end of its burst and
    # never sends RX;. Nothing else will: the QMX stays KEYED, transmitting an
    # unmodulated carrier into the antenna until somebody power-cycles it.
    # Done on the dev bench 2026-09-12, mid-WSPR-burst, and the operator had to
    # catch it - "its still txing..." - and pull the power himself.
    #
    # WSPR makes this likely rather than unlucky: it keys for ~110 s out of every
    # 120, so a flash issued at random during a beacon session lands inside a
    # burst most of the time.
    #
    # Asks the RUNNING firmware rather than reasoning from settings: wspr_tx_en
    # says transmitting is allowed, tx_state says whether it is happening now.
    # A device that cannot be reached is not a reason to refuse - it may be
    # wedged, which is often exactly why it is being reflashed - so an
    # unreachable board falls through with a warning.
    Assert-NotTransmitting $b

    # Before anything else: is the built image even for THIS board?
    Assert-BoardMatches $b

    # What are we about to overwrite? Worth one line, because "which build is on
    # that board" has been wrong in writing before.
    Cmd-Status $reg $b

    $took = Take-Lock $reg "flash" $b.name
    $hadCapture = (@(Get-CaptureProcess $b.capture $b.com).Count -gt 0)
    $preLen = 0
    if (Test-Path $b.capture) { $preLen = (Get-Item $b.capture).Length }
    try {
        if ($hadCapture) { Cmd-StopCapture $reg $b }
        # Wait for the port to actually open, however the capture was stopped -
        # and even when there was no capture, since something else on this
        # machine may hold it. See Wait-PortFree for why a fixed sleep is not
        # good enough. Not fatal on timeout: esptool's own error is clearer
        # than anything invented here, and it has not written a byte yet.
        if (-not (Wait-PortFree $b.com 20)) {
            Write-Host "$($b.com) still busy after 20 s - flashing anyway, esptool will say who holds it." -ForegroundColor Yellow
        }
        $rc = Invoke-Idf $reg $b.tree (Get-IdfArgs $b @("-p", $b.com, "flash"))
        if ($rc -ne 0) { Write-Host "Flash FAILED (exit $rc)" -ForegroundColor Red }
    } finally {
        # Restarting the capture is a finally block, not a step - the crash you
        # care about lands in the window where nobody was watching.
        if ($hadCapture) {
            try { Cmd-Capture $reg $b } catch { Write-Host "COULD NOT RESTART CAPTURE: $($_.Exception.Message)" -ForegroundColor Red }
        }
        if ($took) { Release-Lock $reg }
    }
    # Reopening the port resets the Tab5 (documented), so a restarted capture
    # WILL see a boot header within a few seconds - that is what makes the
    # identity check possible at all after a flash, whose own reset happens
    # while the port is closed.
    Start-Sleep -Seconds 10
    Cmd-Verify $reg $b $preLen
}

function Cmd-Verify {
    param($reg, $b, [long] $sinceBytes = 0)
    if (-not (Test-Path $b.capture)) { Write-Host "No capture file to verify against."; return }
    # ⚠ Only trust a boot header written AFTER the flash. Searching the whole
    # file matched a serial(MAC)= line from a PREVIOUS boot and cheerfully
    # reported "Identity OK" when nothing had been captured at all - a check
    # that cannot fail is not a check, and this one is the last line of defence
    # against flashing the wrong board.
    $text = Get-Content $b.capture -Raw -ErrorAction SilentlyContinue
    if ($sinceBytes -gt 0 -and $text.Length -gt $sinceBytes) { $text = $text.Substring([int]$sinceBytes) }
    elseif ($sinceBytes -gt 0) { $text = "" }
    $line = [regex]::Matches($text, 'serial\(MAC\)=([0-9A-Fa-f:]{17})') | Select-Object -Last 1
    if (-not $line) {
        Write-Host "NOT VERIFIED: no fresh 'serial(MAC)=' boot header since the flash. It is printed at boot only, and a stale one from an earlier boot proves nothing." -ForegroundColor DarkYellow
        return
    }
    $mac = $line.Groups[1].Value.ToUpper()
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
    "standdown"    { Cmd-StandDown    $reg (Get-Bench $reg $Name) }
    "release"      { Cmd-StandDown    $reg (Get-Bench $reg $Name) }
    "watchdog"     { Cmd-Watchdog     $reg $Name }
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
