@echo off
setlocal enabledelayedexpansion
title QMX Panadapter - RECOVERY FLASHER (Bootloader Fix)

echo.
echo ============================================================
echo    QMX Panadapter RECOVERY FLASHER - Bootloader Repair
echo ============================================================
echo.
echo This script RECOVERS a Tab5 with a corrupted bootloader
echo (from the faulty v0.18.5-hotfix flash).
echo.
echo WARNING: This will ERASE the entire Tab5 chip.
echo All settings, WiFi passwords, and logs will be DELETED.
echo.
pause

rem --- Get esptool (same as main flasher) ---
set "ESPTOOL="
if exist "%~dp0esptool.exe" set "ESPTOOL=%~dp0esptool.exe"
if not defined ESPTOOL (
    where esptool.exe >nul 2>nul && set "ESPTOOL=esptool.exe"
)
if not defined ESPTOOL (
    for /f "delims=" %%E in ('dir /b /s "%~dp0esptool\esptool.exe" 2^>nul') do set "ESPTOOL=%%E"
)
if not defined ESPTOOL (
    echo esptool not found - downloading from GitHub...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $ProgressPreference='SilentlyContinue'; $h=@{'User-Agent'='qmx-flasher'}; $r=Invoke-RestMethod -TimeoutSec 30 -Headers $h -Uri 'https://api.github.com/repos/espressif/esptool/releases/latest'; $a=$r.assets | Where-Object { $_.name -like 'esptool*' -and $_.name -like '*.zip' -and ($_.name -like '*amd64*' -or $_.name -like '*win64*') } | Select-Object -First 1; if(-not $a){ exit 3 }; $zip=(Join-Path $env:TEMP $a.name); Invoke-WebRequest -TimeoutSec 300 -Headers $h -Uri $a.browser_download_url -OutFile $zip; $dest=(Join-Path '%~dp0' 'esptool'); if(Test-Path $dest){ Remove-Item -Recurse -Force $dest }; Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force; Remove-Item $zip -Force } catch { exit 1 }"
    for /f "delims=" %%E in ('dir /b /s "%~dp0esptool\esptool.exe" 2^>nul') do set "ESPTOOL=%%E"
)

if not defined ESPTOOL (
    echo ERROR: esptool not available.
    pause
    goto :end
)

echo.
echo Before continuing:
echo   1. Plug Tab5 into this PC with a USB-C DATA cable
echo   2. The Tab5 should be powered ON (or power it on now)
echo   3. Close any serial monitor or other USB programs
echo.
pause

rem Try COM ports low-number-first (the Tab5 is usually on a low COM number),
rem one quick connect attempt each, so we hit the right port fast instead of
rem a hardcoded COM3 that may not match this PC. Falls back to esptool's own
rem auto-detect if no ports could be listed. Same approach as the main
rem flash.bat - recovery is exactly the moment you most need this to "just
rem work", since the user already had something go wrong once.
set "PORTS="
set "PLIST=%TEMP%\qmx_recovery_ports_%RANDOM%.txt"
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object Length,{ $_ } | Set-Content -Encoding ascii -LiteralPath '%PLIST%'"
if exist "%PLIST%" (
    for /f "usebackq delims=" %%P in ("%PLIST%") do set "PORTS=!PORTS! %%P"
    del "%PLIST%" >nul 2>nul
)

rem Detect esptool version: v5+ uses hyphenated subcommands (write-flash,
rem erase-flash); older uses underscores. See feedback_esptool_write_flash_hyphen.
set "WRITE_FLASH=write_flash"
set "ERASE_FLASH=erase_flash"
"%ESPTOOL%" version 2>&1 | findstr /r "v[5-9]\." >nul 2>nul
if not errorlevel 1 (
    set "WRITE_FLASH=write-flash"
    set "ERASE_FLASH=erase-flash"
)

echo.
echo ============================================================
echo  STEP 1: FULL CHIP ERASE
echo ============================================================
echo.

set "RC=1"
if defined PORTS (
    for %%P in (!PORTS!) do (
        if not "!RC!"=="0" (
            echo   trying %%P ...
            "%ESPTOOL%" --chip esp32p4 -p %%P -b 460800 --connect-attempts 1 !ERASE_FLASH!
            if not errorlevel 1 set "RC=0"
        )
    )
) else (
    "%ESPTOOL%" --chip esp32p4 -b 460800 --connect-attempts 1 !ERASE_FLASH!
    if not errorlevel 1 set "RC=0"
)

if not "!RC!"=="0" (
    echo ERROR: Erase failed on every detected COM port. Check USB connection
    echo ^(must be a DATA cable, not charge-only^) and that no other program
    echo ^(serial monitor, etc.^) is using the port.
    pause
    goto :end
)

echo.
echo ============================================================
echo  STEP 2: FLASHING BOOTLOADER + PARTITION TABLE + APP
echo ============================================================
echo.

rem Check if we have the recovery files locally
set "BOOTLOADER=%~dp0bootloader.bin"
set "APP_BIN=%~dp0qmx_panadapter_merged_v0.18.5-hotfix.bin"
set "PARTITION=%~dp0partition-table.bin"

if not exist "%BOOTLOADER%" (
    echo ERROR: bootloader.bin not found in flasher directory
    echo.
    echo To recover, you need the v0.18.5-hotfix release files:
    echo - Download from: https://github.com/SteffenLav/qmx-panadapter/releases/tag/v0.18.5-hotfix
    echo - Extract QMX-Panadapter-v0.18.5-hotfix-flasher.zip
    echo - Run flash-recovery.bat from that folder
    pause
    goto :end
)

echo Flashing with correct bootloader layout...
echo.

set "RC=1"
if defined PORTS (
    for %%P in (!PORTS!) do (
        if not "!RC!"=="0" (
            echo   trying %%P ...
            "%ESPTOOL%" --chip esp32p4 -p %%P -b 460800 --connect-attempts 1 ^
              !WRITE_FLASH! ^
              0x2000 "%BOOTLOADER%" ^
              0x10000 "%APP_BIN%" ^
              0x8000 "%PARTITION%"
            if not errorlevel 1 set "RC=0"
        )
    )
) else (
    "%ESPTOOL%" --chip esp32p4 -b 460800 --connect-attempts 1 ^
      !WRITE_FLASH! ^
      0x2000 "%BOOTLOADER%" ^
      0x10000 "%APP_BIN%" ^
      0x8000 "%PARTITION%"
    if not errorlevel 1 set "RC=0"
)

if not "!RC!"=="0" (
    echo ERROR: Flash failed on every detected COM port.
    pause
    goto :end
)

echo.
echo ============================================================
echo    ✅ RECOVERY COMPLETE
echo ============================================================
echo.
echo The Tab5 is now recovering... it should power on within 5 seconds.
echo You will see the WiFi setup screen (normal for first boot).
echo.
pause

:end
endlocal
