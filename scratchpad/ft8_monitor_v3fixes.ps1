# Non-interactive serial capture for long FT8 monitoring. Writes to a fixed file.
$portName = "COM3"
$logFile  = "C:\dev\qmx-panadapter\scratchpad\ft8_monitor_v3fixes.log"

"=== FT8 monitor capture started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') on $portName @921600 ===" |
    Out-File -FilePath $logFile -Encoding utf8

while ($true) {
    $port = New-Object System.IO.Ports.SerialPort $portName, 921600, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $port.ReadTimeout = 1000
    # Leave DTR/RTS at default false so we don't reset the P4 into bootloader.
    try { $port.Open() } catch {
        Start-Sleep -Seconds 2
        continue
    }
    "[connected $(Get-Date -Format 'HH:mm:ss')]" | Out-File -FilePath $logFile -Append -Encoding utf8
    try {
        while ($port.IsOpen) {
            try { $line = $port.ReadLine() }
            catch [System.TimeoutException] { continue }
            "$(Get-Date -Format 'HH:mm:ss') $line" | Out-File -FilePath $logFile -Append -Encoding utf8
        }
    } catch {
        "[disconnected $(Get-Date -Format 'HH:mm:ss') - reconnecting]" | Out-File -FilePath $logFile -Append -Encoding utf8
    } finally {
        if ($port.IsOpen) { $port.Close() }
        $port.Dispose()
    }
    Start-Sleep -Milliseconds 500
}

