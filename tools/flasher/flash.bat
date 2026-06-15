@echo off
setlocal enabledelayedexpansion
title QMX Panadapter - Tab5 Flasher

echo(
echo ============================================================
echo    QMX Panadapter firmware flasher  ^(M5Stack Tab5^)
echo ============================================================
echo(

rem --- 1. locate esptool.exe: prefer one next to this script, else PATH ------
set "ESPTOOL="
if exist "%~dp0esptool.exe" (
    set "ESPTOOL=%~dp0esptool.exe"
) else (
    where esptool.exe >nul 2>nul && set "ESPTOOL=esptool.exe"
)
if not defined ESPTOOL (
    echo ERROR: esptool.exe was not found.
    echo(
    echo Put esptool.exe in this same folder. Download the Windows build from:
    echo    https://github.com/espressif/esptool/releases
    echo ^(grab esptool-vX.X.X-windows-amd64.zip, unzip, copy esptool.exe here^)
    echo(
    goto :end
)

rem --- 2. find the firmware .bin in this folder (newest by file time, so a
rem        leftover older .bin can't win on a lexical version-string sort) -----
set "FW="
for /f "delims=" %%F in ('dir /b /o-d "%~dp0qmx_panadapter_merged_*.bin" 2^>nul') do (
    if not defined FW set "FW=%~dp0%%F"
)
if not defined FW (
    echo ERROR: No qmx_panadapter_merged_*.bin found in this folder.
    echo Put this script in the same folder as the firmware .bin file.
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
"%ESPTOOL%" --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash 0x0 "%FW%"
set "RC=%errorlevel%"

echo(
if "%RC%"=="0" (
    echo ============================================================
    echo    SUCCESS - the Tab5 is restarting with the new firmware.
    echo ============================================================
) else (
    echo ============================================================
    echo    FLASH FAILED  ^(exit code %RC%^)
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
