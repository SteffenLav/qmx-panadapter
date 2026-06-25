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
echo      (it will power on automatically)
echo   2. Close any serial monitor or other USB programs
echo.
pause

echo.
echo ============================================================
echo  STEP 1: FULL CHIP ERASE
echo ============================================================
echo.
"%ESPTOOL%" --chip esp32p4 -p COM3 -b 460800 erase_flash
if errorlevel 1 (
    echo ERROR: Erase failed. Check USB connection.
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

"%ESPTOOL%" --chip esp32p4 -p COM3 -b 460800 ^
  write_flash ^
  0x2000 "%BOOTLOADER%" ^
  0x10000 "%APP_BIN%" ^
  0x8000 "%PARTITION%"

if errorlevel 1 (
    echo ERROR: Flash failed
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
