# Capture ACROSS reboots.
#
# The other two capture scripts in this directory cannot see a reboot at all,
# and that is not a small gap - it made me report "only one boot in the log"
# when the device was demonstrably rebooting in front of the operator. The
# Tab5's console is USB-Serial/JTAG implemented BY the ESP32-P4 itself, so a
# reset drops the USB device: the COM port disappears, the open handle dies,
# and a script without reconnect logic simply stops recording at the exact
# moment of interest while still looking healthy.
#
# This one reopens the port whenever it goes away, and stamps each reconnect so
# the reboot itself is visible in the file.
# -Reset: pulse RTS on the first open so the BOOT is captured. OFF BY DEFAULT.
# It is a real side effect - it reboots the operator's device - and leaving it
# on by default had me resetting the Tab5 every time I started a "passive"
# monitor. That is almost certainly the mysterious "boot -> dark -> boot again"
# the operator reported: my flash booted it once, then my capture reset it a
# second time. Only ask for it when you actually want to observe a startup.
param([string]$Port = "COM3", [string]$Out = "reboot.txt", [int]$Seconds = 300,
      [switch]$Reset)

$end = (Get-Date).AddSeconds($Seconds)
$reconnects = 0

$consecutiveLost = 0   # resets on the first real byte; drives the backoff below
# This process is nearly idle (a 100 ms poll of ~40 bytes/s), so raising its
# priority costs nothing and buys the one thing that matters: it still gets
# scheduled when the machine is saturated by something else - a build, or the
# PixInsight session sharing this box. Steady state was never the risk; a BURST
# is (a boot log, an FT8 decode storm, a panic dump run at tens of KB), and a
# panic dump is unrepeatable, so losing it to a scheduling slip is the one
# failure this capture cannot tolerate.
try { (Get-Process -Id $PID).PriorityClass = 'AboveNormal' } catch { }

while ((Get-Date) -lt $end) {
    $sp = $null
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
        $sp.ReadTimeout = 500
        # Default is 4096 bytes = ~100 s of steady state but well under one
        # second of a boot log. 256 KB removes the overflow window entirely.
        $sp.ReadBufferSize = 262144
        $sp.Open()
        [System.IO.File]::AppendAllText($Out,
            "`n=== [capture] port opened $(Get-Date -Format HH:mm:ss) (reconnect #$reconnects) ===`n")
        # On the FIRST open only, reset into the application so the boot itself
        # is captured. DTR low keeps GPIO0 high (normal boot, not download
        # mode); RTS pulses EN. Reconnects must NOT do this - a reset on every
        # reconnect would relaunch the very reboot we are trying to observe.
        if ($Reset -and $reconnects -eq 0 -and -not $script:didReset) {
            $script:didReset = $true
            $sp.DtrEnable = $false
            $sp.RtsEnable = $true
            Start-Sleep -Milliseconds 150
            $sp.DiscardInBuffer()
            $sp.RtsEnable = $false
        }
        while ((Get-Date) -lt $end) {
            $chunk = $sp.ReadExisting()          # throws when the device vanishes
            if ($chunk.Length -gt 0) {
                [System.IO.File]::AppendAllText($Out, $chunk)
                $consecutiveLost = 0                 # real bytes = the port is back
            }
            Start-Sleep -Milliseconds 100
        }
    } catch {
        $reconnects++
        # A flash or a reboot loses the port for a second or two, and saying so
        # is the point of this line. A board that is UNPLUGGED or switched off
        # loses it for ever, and at 400 ms a go that wrote 2,506,870 identical
        # lines into the capture - about 9 KB/min, 13 MB/day - measured
        # 2026-10-13 after the dev bench was powered down. The capture IS the
        # diagnostic record, so diluting it with millions of lines of its own
        # retry noise is the one thing it must not do.
        #
        # So: say it the first few times, then back off and say it rarely. The
        # backoff also stops a dead port spinning a CPU for days.
        if ($consecutiveLost -lt 3 -or ($consecutiveLost % 150) -eq 0) {
            $note = if ($consecutiveLost -lt 3) { "device reset? reopening" }
                    else { "still gone after $consecutiveLost attempts - unplugged or powered off?" }
            [System.IO.File]::AppendAllText($Out,
                "`n=== [capture] PORT LOST $(Get-Date -Format HH:mm:ss) - $note ===`n")
        }
        $consecutiveLost++
        # 400 ms for the first few, so a flash is still followed closely, then
        # up to 4 s. Capped rather than growing without limit: the port coming
        # back must be noticed within a few seconds, not a few minutes.
        $wait = if ($consecutiveLost -le 5) { 400 } elseif ($consecutiveLost -le 20) { 1500 } else { 4000 }
        Start-Sleep -Milliseconds $wait
    } finally {
        if ($sp -and $sp.IsOpen) { try { $sp.Close() } catch {} }
    }
}
[System.IO.File]::AppendAllText($Out, "`n=== [capture] done, $reconnects reconnect(s) ===`n")
