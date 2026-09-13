# bench_watchdog.ps1 - keep the serial captures alive, without supervision.
#
# WHY THIS EXISTS
#   The capture is the only record of a crash. CLAUDE.md's serial rule 10 says
#   a dead capture looks EXACTLY like a quiet, healthy device, and rule 11 says
#   one started from a tool call gets killed - which is why captures run as
#   scheduled tasks. But that task is `/sc once`: it starts the capture, and if
#   the capture ever dies, nothing brings it back.
#
#   It died on 2026-09-12 during a flash and stayed dead for 22 minutes. The
#   operator hit a complete UI freeze in that window and there was no log of it
#   at all. His words: "you simply need to fix that capture once and for all
#   ... i do not need to hear after a crasch that it was not captured."
#
#   So this runs every minute and restarts any capture that has stopped
#   producing output. It is deliberately dumb: it never reasons about WHY the
#   capture stopped, it just notices that the file is not growing.
#
# ⛔ WHAT IT MUST NOT DO
#   Restart a capture during a flash. bench.ps1 stops the capture on purpose
#   there, and re-opening the port mid-write would be the COM9-held-through-a-
#   flash incident of 2026-09-09 all over again. The bench LOCK is the
#   interlock: bench flash holds it for the whole operation, so this skips
#   entirely while it is held.
#
#   Reopening a port also resets the Tab5 (documented, non-deterministic), and
#   a warm reset with the radio attached is the #74 QMX wedge - so a restart is
#   not free. That is the price of having a record at all, and it is only paid
#   when the capture is genuinely dead, never on a whim.

param(
    [int] $StaleSeconds = 90,   # no new bytes for this long = dead
    [switch] $Once              # run one pass and exit (for testing)
)

$ErrorActionPreference = "Stop"

$Bench   = Join-Path $PSScriptRoot "bench.ps1"
$RegPath = "C:/dev/qmx-panadapter/tools/bench.json"
$LogPath = "C:/dev/qmx-panadapter/scratchpad/bench-watchdog.log"

function Note {
    param([string] $msg)
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    try { Add-Content -Path $LogPath -Value $line -Encoding utf8 } catch { }
}

function Lock-Held {
    # Same file bench.ps1 uses. Any holder at all means hands off: a build is
    # harmless to us, but a flash is not, and telling them apart from here
    # would mean duplicating Cmd-Flash's own state.
    try {
        $reg = Get-Content $RegPath -Raw | ConvertFrom-Json
        if (-not $reg.lock_file -or -not (Test-Path $reg.lock_file)) { return $false }
        $lock = Get-Content $reg.lock_file -Raw | ConvertFrom-Json
        if (-not $lock.pid) { return $false }
        # A lock whose process is gone is rubble, not a claim - same rule
        # bench.ps1's Test-LockStale applies.
        return [bool](Get-Process -Id $lock.pid -ErrorAction SilentlyContinue)
    } catch { return $false }
}

# ⛔ A STALE FILE CAN COEXIST WITH A GENUINELY-ALIVE PROCESS, AND THAT WAS THE
# WHOLE BUG. Found 2026-09-12 after `bench capture dev` correctly reported
# "already running" against a process that had produced NOTHING for 6.5 hours
# straight, one watchdog cycle every minute, all of them logging "restart
# produced NO output" within 8-9 seconds - too fast for the internal 20-40 s
# retry loop bench.ps1's Cmd-Capture runs when it actually tries to start
# something. That timing is the tell: Cmd-Capture was hitting its OWN
# "already running - leave it alone" early return every single time, because a
# process genuinely was alive - just stuck.
#
# The likely mechanism (consistent with the timeline: the device was frozen,
# then flashed and rebooted, and the failures continued for hours afterward
# regardless): SerialPort.Open()/Read() against a COM port backed by a
# USB-CDC device that has since reset can BLOCK INDEFINITELY on the OS side
# rather than throwing - so the capture script's own try/catch/reconnect logic
# (which is otherwise correct) never gets a chance to run, because the thread
# never returns from the blocking call. Task Scheduler's `/end` cannot reach
# it either: `/end` only knows about the wscript.exe process it launched, and
# wscript already exited the instant it started the detached capture (that is
# the whole point of run_hidden.vbs - see its own header). The capture process
# is an orphan from Task Scheduler's point of view from the moment it starts.
#
# So bench.ps1's "is a process already running" check is the RIGHT question
# for a human running `bench capture` (never clobber a healthy one), and the
# WRONG question for a watchdog, whose entire job is to find a process that is
# alive but not doing its job. Staleness of the FILE, not existence of the
# PROCESS, is the watchdog's ground truth - so on a stale file it kills
# whatever is holding the port BEFORE asking bench.ps1 to start a fresh one,
# every time, regardless of what Get-CaptureProcess would have said.
function Get-StaleCaptureProcess {
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
    return $procs   # unrolled on purpose, caller wraps in @() - see bench.ps1's Get-CaptureProcess for why `,$procs` was wrong
}

function Invoke-Pass {
    if (Lock-Held) { return }

    $reg = Get-Content $RegPath -Raw | ConvertFrom-Json
    foreach ($b in $reg.benches) {
        if (-not $b.capture) { continue }
        if (-not $b.com -or $b.com -eq "UNASSIGNED") { continue }

        # `bench standdown <name>` - the operator asked for the port back. Never
        # respawn over that; `bench capture <name>` clears the flag.
        if (Test-Path "C:/dev/bench.standdown.$($b.name)") { continue }

        # Not plugged in is not a fault - say nothing, do nothing.
        if (-not ([System.IO.Ports.SerialPort]::GetPortNames() -contains $b.com)) { continue }

        $alive = $false
        if (Test-Path $b.capture) {
            $age = (New-TimeSpan -Start (Get-Item $b.capture).LastWriteTime -End (Get-Date)).TotalSeconds
            $alive = ($age -lt $StaleSeconds)
        }
        if ($alive) { continue }

        # Kill anything already holding the port for THIS bench before asking
        # bench.ps1 to start a new one - see the header comment above. A stuck
        # process here is exactly what made every prior automated restart a
        # no-op: bench.ps1 saw it, called it "already running", and returned.
        $stuck = @(Get-StaleCaptureProcess $b.capture)
        if ($stuck.Count -gt 0) {
            Note "bench '$($b.name)': capture stale but $($stuck.Count) process(es) still hold the port - killing before restart"
            foreach ($p in $stuck) {
                try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop }
                catch { Note "bench '$($b.name)': could not kill pid $($p.ProcessId) - $($_.Exception.Message)" }
            }
            Start-Sleep -Seconds 2
        }

        Note "bench '$($b.name)': capture stale (>$StaleSeconds s) - restarting"
        try {
            & $Bench capture $b.name -Force *>&1 | Out-Null
        } catch {
            Note "bench '$($b.name)': restart FAILED - $($_.Exception.Message)"
            continue
        }

        # Say whether it actually came back, or the log records a restart that
        # never produced a byte - which is the same silent lie the capture
        # itself was guilty of.
        Start-Sleep -Seconds 8
        $ok = $false
        if (Test-Path $b.capture) {
            $age = (New-TimeSpan -Start (Get-Item $b.capture).LastWriteTime -End (Get-Date)).TotalSeconds
            $ok = ($age -lt 20)
        }
        Note ("bench '{0}': restart {1}" -f $b.name, ($(if ($ok) { "OK - capture is writing again" } else { "produced NO output" })))
    }
}

if ($Once) { Invoke-Pass; exit 0 }

# The scheduled task fires this every minute; one pass per fire.
Invoke-Pass
