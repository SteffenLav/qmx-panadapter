# Capture from a CLEAN RESET, so the boot sequence is in the log.
#
# cap_serial_stream.ps1 deliberately never touches DTR/RTS (asserting them can
# drop the P4 into the ROM download stub), and as a result it always starts too
# late to see a boot - every "why did it fail at startup" question has had to be
# answered from the flash-persisted diag log instead.
#
# This one opens the port FIRST, then performs the standard auto-reset into the
# APPLICATION: DTR low (GPIO0 high = normal boot, NOT download mode) while RTS
# pulses EN. Getting DTR the wrong way round here is exactly how you end up in
# the bootloader with no output, which is the trap the other script avoids by
# not trying at all.
param([string]$Port = "COM3", [string]$Out = "boot.txt", [int]$Seconds = 90)

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
$sp.ReadTimeout = 500
try {
    $sp.Open()
    # Normal-boot reset: GPIO0 must stay HIGH (DtrEnable $false) or the chip
    # comes up in download mode and prints nothing at all.
    $sp.DtrEnable = $false
    $sp.RtsEnable = $true          # EN low  -> held in reset
    Start-Sleep -Milliseconds 150
    $sp.DiscardInBuffer()
    $sp.RtsEnable = $false         # EN high -> run
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) { [System.IO.File]::AppendAllText($Out, $chunk) }
        } catch {}
        Start-Sleep -Milliseconds 100
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
