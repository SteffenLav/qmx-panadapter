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
rem !! VALIDATE BEFORE ERASING. These checks used to sit in STEP 2,
rem AFTER the full chip erase in STEP 1 - so a missing file meant the
rem Tab5 was wiped and then left with nothing written to it. The erase
rem takes the WiFi credentials, the callsign, the memory channels, the
rem QSO log and the LoTW private key, and none of that comes back.
rem Nothing is destroyed now until every file needed to rebuild it is
rem confirmed present.

rem Check if we have the recovery files locally
set "BOOTLOADER=%~dp0bootloader.bin"
set "APP_BIN=%~dp0qmx_panadapter.bin"
set "OTADATA=%~dp0ota_data_initial.bin"
set "PARTITION=%~dp0partition-table.bin"

rem !! CHECK EVERY FILE. This used to test bootloader.bin alone while APP_BIN
rem named a merged v0.18.5 image that is NOT in this folder - so the guard
rem passed, STEP 1 erased the whole chip, and STEP 2 then failed on a missing
rem file. That is a wiped AND bricked Tab5: the erase takes the WiFi
rem credentials, the callsign, the memory channels, the QSO log and the LoTW
rem private key with it.
if not exist "%APP_BIN%" (
    echo ERROR: qmx_panadapter.bin not found in flasher directory
    pause
    goto :end
)
if not exist "%PARTITION%" (
    echo ERROR: partition-table.bin not found in flasher directory
    pause
    goto :end
)
if not exist "%OTADATA%" (
    echo ERROR: ota_data_initial.bin not found in flasher directory
    pause
    goto :end
)
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


echo ============================================================
echo  STEP 1: FULL CHIP ERASE
echo ============================================================
echo.
rem No -p: esptool finds the port itself. This said "-p COM3", which is this
rem bench's port and almost nobody else's - on any other machine the recovery
rem flasher simply could not reach the Tab5.
"%ESPTOOL%" --chip esp32p4 -b 460800 erase_flash
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

echo Flashing with correct bootloader layout...
echo.

"%ESPTOOL%" --chip esp32p4 -b 460800 ^
  write_flash ^
  0x2000 "%BOOTLOADER%" ^
  0x8000 "%PARTITION%" ^
  0x10000 "%APP_BIN%" ^
  0x920000 "%OTADATA%"

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
