@echo off
setlocal enabledelayedexpansion
title QMX Panadapter - Tab5 Flasher

echo(
echo ============================================================
echo    QMX Panadapter firmware flasher  ^(M5Stack Tab5^)
echo ============================================================
echo(

rem --- 1. get esptool: bundled beside script > on PATH > previously cached >
rem        auto-download the Windows build from Espressif's GitHub releases ----
set "ESPTOOL="
if exist "%~dp0esptool.exe" set "ESPTOOL=%~dp0esptool.exe"
if not defined ESPTOOL (
    where esptool.exe >nul 2>nul && set "ESPTOOL=esptool.exe"
)
if not defined ESPTOOL (
    for /f "delims=" %%E in ('dir /b /s "%~dp0esptool\esptool.exe" 2^>nul') do set "ESPTOOL=%%E"
)
if not defined ESPTOOL (
    echo esptool not found - downloading it from GitHub ^(one time, needs internet^)...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $ProgressPreference='SilentlyContinue'; $h=@{'User-Agent'='qmx-flasher'}; $r=Invoke-RestMethod -TimeoutSec 30 -Headers $h -Uri 'https://api.github.com/repos/espressif/esptool/releases/latest'; $a=$r.assets | Where-Object { $_.name -like 'esptool*' -and $_.name -like '*.zip' -and ($_.name -like '*amd64*' -or $_.name -like '*win64*') } | Select-Object -First 1; if(-not $a){ exit 3 }; $zip=(Join-Path $env:TEMP $a.name); Invoke-WebRequest -TimeoutSec 300 -Headers $h -Uri $a.browser_download_url -OutFile $zip; $dest=(Join-Path '%~dp0' 'esptool'); if(Test-Path $dest){ Remove-Item -Recurse -Force $dest }; Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force; Remove-Item $zip -Force } catch { exit 1 }"
    for /f "delims=" %%E in ('dir /b /s "%~dp0esptool\esptool.exe" 2^>nul') do set "ESPTOOL=%%E"
)
if not defined ESPTOOL (
    echo(
    echo ERROR: could not obtain esptool.
    echo   - You need internet on the FIRST run so it can be downloaded, OR
    echo   - put esptool.exe next to this script yourself, from:
    echo     https://github.com/espressif/esptool/releases
    echo(
    goto :end
)

rem --- 2. get the firmware: download the latest GitHub release, falling back
rem        to a local qmx_panadapter_merged_*.bin if there's no internet -------
set "REPO=SteffenLav/qmx-panadapter"
echo Checking GitHub for the latest firmware ^(needs internet^)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $ProgressPreference='SilentlyContinue'; $h=@{'User-Agent'='qmx-flasher'}; $r=Invoke-RestMethod -TimeoutSec 20 -Headers $h -Uri 'https://api.github.com/repos/%REPO%/releases/latest'; $a=$r.assets | Where-Object { $_.name -like 'qmx_panadapter_merged_*.bin' } | Select-Object -First 1; if(-not $a){ exit 2 }; Write-Host ('  latest release: ' + $r.tag_name + '  (' + $a.name + ')'); $out=(Join-Path '%~dp0' $a.name); $tmp=$out + '.part'; Invoke-WebRequest -TimeoutSec 180 -Headers $h -Uri $a.browser_download_url -OutFile $tmp; Move-Item -Force $tmp $out; Write-Host '  download OK.' } catch { exit 1 }"
if errorlevel 1 (
    echo   could not fetch from GitHub ^(offline?^) - looking for a local copy...
)

rem Pick the newest .bin in this folder: the freshly-downloaded latest, or a
rem bundled local copy if the download was skipped. Newest by file time, so a
rem leftover older .bin can't win on a lexical version-string sort.
set "FW="
for /f "delims=" %%F in ('dir /b /o-d "%~dp0qmx_panadapter_merged_*.bin" 2^>nul') do (
    if not defined FW set "FW=%~dp0%%F"
)
if not defined FW (
    echo(
    echo ERROR: no firmware available.
    echo   - Connect this PC to the internet so the latest can be downloaded, OR
    echo   - put a qmx_panadapter_merged_*.bin in this folder for offline use.
    goto :end
)

for %%F in ("%FW%") do set "FWNAME=%%~nxF"
echo Firmware found: !FWNAME!
echo(
echo Before you continue:
echo   1. Plug the Tab5 into this PC with a USB-C DATA cable
echo      ^(a charge-only cable will NOT work^).
echo   2. Close any serial monitor, Arduino IDE, or other app using the port.
echo(
pause

echo(
echo Flashing - do NOT unplug the Tab5...
echo(

rem Try COM ports low-number-first (the Tab5 is usually on a low COM number),
rem one quick connect attempt each, so we hit the right port fast and don't sit
rem through long retries on the wrong ones. Falls back to esptool's own
rem auto-detect if no ports could be listed.
set "PORTS="
set "PLIST=%TEMP%\qmx_ports_%RANDOM%.txt"
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object Length,{ $_ } | Set-Content -Encoding ascii -LiteralPath '%PLIST%'"
if exist "%PLIST%" (
    for /f "usebackq delims=" %%P in ("%PLIST%") do set "PORTS=!PORTS! %%P"
    del "%PLIST%" >nul 2>nul
)

rem Detect esptool version: v5+ uses write-flash (hyphen); older uses write_flash
set "WRITE_FLASH=write_flash"
"%ESPTOOL%" version 2>&1 | findstr /r "v[5-9]\." >nul 2>nul
if not errorlevel 1 set "WRITE_FLASH=write-flash"

set "RC=1"
if defined PORTS (
    for %%P in (!PORTS!) do (
        if not "!RC!"=="0" (
            echo   trying %%P ...
            "%ESPTOOL%" --chip esp32p4 -p %%P -b 460800 --connect-attempts 1 !WRITE_FLASH! 0x0 "%FW%"
            if not errorlevel 1 set "RC=0"
        )
    )
) else (
    "%ESPTOOL%" --chip esp32p4 -b 460800 --connect-attempts 1 !WRITE_FLASH! 0x0 "%FW%"
    if not errorlevel 1 set "RC=0"
)

echo(
if "!RC!"=="0" (
    echo ============================================================
    echo    SUCCESS - the Tab5 is restarting with the new firmware.
    echo ============================================================
) else (
    echo ============================================================
    echo    FLASH FAILED - could not flash the Tab5 on any COM port.
    echo    - Use a different USB-C cable - it must carry DATA, not
    echo      just power. Many cheap cables are charge-only.
    echo    - Close any program using the serial port and try again.
    echo    - Unplug, replug, then re-run this script.
    echo ============================================================
)

:end
echo(
pause
endlocal
