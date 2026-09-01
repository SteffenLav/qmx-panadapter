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

rem --- 2. Verify firmware components are available -----
set "BL=%~dp0bootloader.bin"
set "PT=%~dp0partition-table.bin"
set "APP=%~dp0qmx_panadapter.bin"
rem OTA data. Written so a cable flash actually BOOTS what it just wrote:
rem the app goes to the factory slot at 0x10000, but if the device has an
rem update staged in ota_0 the bootloader goes there instead and the operator
rem sees the OLD firmware come back. Caught on the bench flashing v1.9.3 - the
rem device booted a staged v1.9.2. Matters far more from v1.9.3 on, because it
rem downloads updates in the background by default.
set "OTAD=%~dp0ota_data_initial.bin"

if not exist "%BL%" (
    echo(
    echo ERROR: bootloader.bin not found.
    echo   This flasher requires:
    echo     - bootloader.bin
    echo     - partition-table.bin
    echo     - qmx_panadapter.bin
    goto :end
)
if not exist "%PT%" (
    echo ERROR: partition-table.bin not found.
    goto :end
)
if not exist "%APP%" (
    echo ERROR: qmx_panadapter.bin not found.
    goto :end
)

echo Firmware components found:
echo   - bootloader.bin
echo   - partition-table.bin
echo   - qmx_panadapter.bin
echo(
echo Before you continue:
echo   1. Plug the Tab5 into this PC with a USB-C DATA cable
echo      ^(a charge-only cable will NOT work^).
echo   2. Close any serial monitor, Arduino IDE, or other app using the port.
echo(
echo ------------------------------------------------------------
echo  FLASH TYPE
echo ------------------------------------------------------------
echo A NORMAL flash keeps all your saved settings.
echo(
echo A CLEAN flash WIPES the whole chip first.  *** WARNING ***
echo   *** ALL of the following will be PERMANENTLY ERASED: ***
echo       - your WiFi network name AND password
echo       - your callsign and Maidenhead grid
echo       - ALL saved memory channels
echo       - your logged QSOs ^(the ADIF log^)
echo   This cannot be undone - you will re-enter everything.
echo   Only use it if something is stuck or corrupted
echo   (e.g. WiFi will not turn on no matter what).
echo ------------------------------------------------------------
set "ERASEOPT="
set "CLEAN="
set /p "CLEAN=Type E for a CLEAN/ERASE flash, or just Enter for a normal flash: "
if /i "!CLEAN:~0,1!"=="e" set "ERASEOPT=-e"
if /i "!CLEAN:~0,1!"=="y" set "ERASEOPT=-e"
if defined ERASEOPT (
    echo(
    echo   *** CLEAN FLASH SELECTED - WiFi credentials, callsign/grid, and ALL
    echo       memory channels WILL BE ERASED. Press Ctrl+C now to cancel. ***
)
echo(
pause

echo(
if defined ERASEOPT (
    echo Erasing + flashing - do NOT unplug the Tab5 ^(the full erase adds a bit^)...
) else (
    echo Flashing - do NOT unplug the Tab5...
)
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
            "%ESPTOOL%" --chip esp32p4 -p %%P -b 460800 --connect-attempts 1 !WRITE_FLASH! !ERASEOPT! 0x2000 "%BL%" 0x8000 "%PT%" 0x10000 "%APP%" 0x920000 "%OTAD%"
            if not errorlevel 1 set "RC=0"
        )
    )
) else (
    "%ESPTOOL%" --chip esp32p4 -b 460800 --connect-attempts 1 !WRITE_FLASH! !ERASEOPT! 0x2000 "%BL%" 0x8000 "%PT%" 0x10000 "%APP%" 0x920000 "%OTAD%"
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
