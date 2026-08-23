# Continuous serial capture, APPENDS as it reads (crash-safe - the buffer-at-
# end variant loses everything if the reader is killed). No DtrEnable/RtsEnable
# (the P4 auto-reset circuit watches those lines).
param([string]$Port = "COM3", [string]$Out = "serial_stream.txt", [int]$Seconds = 3600)
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
$sp.ReadTimeout = 500
try {
    $sp.Open()
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) { [System.IO.File]::AppendAllText($Out, $chunk) }
        } catch {}
        Start-Sleep -Milliseconds 200
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
