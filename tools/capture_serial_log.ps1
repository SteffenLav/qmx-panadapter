<#
.SYNOPSIS
    Captures the USB serial console log from a QMX Panadapter (M5Stack Tab5).

.DESCRIPTION
    No ESP-IDF install needed. Uses the .NET SerialPort class built into
    Windows/PowerShell. Lists available COM ports, connects at 921600 baud
    (console UART speed as of v0.16.0), and logs everything the device prints
    to a timestamped text file - including boot messages and any crash /
    "Guru Meditation Error" backtrace.

    Auto-reconnects if the port drops during a reset (some USB-Serial/JTAG
    resets briefly disconnect and re-enumerate).

.HOW TO USE
    1. Plug the Tab5 into this PC via USB-C.
    2. Right-click this file -> "Run with PowerShell".
       (If Windows blocks it, open PowerShell and run:
        powershell -ExecutionPolicy Bypass -File capture_serial_log.ps1)
    3. Pick the COM port from the list shown (usually the only one, or the
       one labelled "USB Serial" / "USB JTAG/serial").
    4. Power-cycle the Tab5 (unplug/replug power, or press the reset
       button) so the FULL boot log is captured, including the crash.
    5. Let it run through at least 2-3 reboot cycles (~30-60 seconds).
    6. Press Ctrl+C to stop.
    7. Send the generated qmx_log_*.txt file back.
#>

Add-Type -AssemblyName System.Core

$ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if (-not $ports -or $ports.Count -eq 0) {
    Write-Host "No serial ports found." -ForegroundColor Red
    Write-Host "Plug the Tab5 into this PC via USB-C, then run this script again."
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "=== QMX Panadapter serial log capture ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Available COM ports:"

# Try to show friendly descriptions via WMI (best-effort; falls back to bare list).
$descriptions = @{}
try {
    Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
        Where-Object { $_.Name -match "\(COM\d+\)" } |
        ForEach-Object {
            if ($_.Name -match "\((COM\d+)\)") {
                $descriptions[$matches[1]] = $_.Name
            }
        }
} catch { }

foreach ($p in $ports) {
    if ($descriptions.ContainsKey($p)) {
        Write-Host "  $p  -  $($descriptions[$p])"
    } else {
        Write-Host "  $p"
    }
}
Write-Host ""

if ($ports.Count -eq 1) {
    $portName = $ports[0]
    Write-Host "Only one port found - using $portName" -ForegroundColor Yellow
} else {
    $portName = Read-Host "Type the COM port to use (e.g. COM5)"
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$logFile = Join-Path $PSScriptRoot "qmx_log_$timestamp.txt"

Write-Host ""
Write-Host "Logging '$portName' @ 921600 baud to:" -ForegroundColor Green
Write-Host "  $logFile"
Write-Host ""
Write-Host "Now power-cycle the Tab5 (unplug/replug power or press reset)." -ForegroundColor Yellow
Write-Host "Let it run through 2-3 reboot cycles, then press Ctrl+C to stop."
Write-Host ""

"=== QMX Panadapter serial capture started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" |
    Out-File -FilePath $logFile -Encoding utf8

while ($true) {
    $port = New-Object System.IO.Ports.SerialPort $portName, 921600, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $port.ReadTimeout = 1000
    # Leave DTR/RTS untouched (default false): the ESP32-P4 USB-Serial/JTAG
    # auto-reset-to-bootloader circuit watches these lines (same as esptool's
    # reset sequence). Asserting them here can drop the chip into the ROM
    # download stub, which prints nothing.

    try {
        $port.Open()
    } catch {
        Write-Host "Could not open $portName - retrying in 2s... ($($_.Exception.Message))" -ForegroundColor DarkYellow
        Start-Sleep -Seconds 2
        continue
    }

    Write-Host "[connected to $portName]" -ForegroundColor DarkGray
    "[connected to $portName]" | Out-File -FilePath $logFile -Append -Encoding utf8

    try {
        while ($port.IsOpen) {
            try {
                $line = $port.ReadLine()
            } catch [System.TimeoutException] {
                continue
            }
            Write-Host $line
            $line | Out-File -FilePath $logFile -Append -Encoding utf8
        }
    } catch {
        Write-Host "[disconnected - reconnecting...]" -ForegroundColor DarkYellow
        "[disconnected - reconnecting...]" | Out-File -FilePath $logFile -Append -Encoding utf8
    } finally {
        if ($port.IsOpen) { $port.Close() }
        $port.Dispose()
    }

    Start-Sleep -Milliseconds 500
}
