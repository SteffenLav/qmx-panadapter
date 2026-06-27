#!/usr/bin/env bash
# QMX Panadapter - offline firmware flasher for macOS / Linux.
#   macOS: double-click this file (Finder opens it in Terminal).
#   Linux: run  ./flash.command   (or  bash flash.command )
# Needs esptool installed once:  pip3 install esptool   (or  brew install esptool )

set -u
cd "$(dirname "$0")" || exit 1

echo "============================================================"
echo "   QMX Panadapter firmware flasher  (M5Stack Tab5)"
echo "============================================================"
echo

# --- locate esptool (new 'esptool' name, then legacy 'esptool.py') ----------
if command -v esptool >/dev/null 2>&1; then
    ESPTOOL="esptool"
elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL="esptool.py"
else
    echo "ERROR: esptool is not installed."
    echo
    echo "Install it once, then run this again:"
    echo "    macOS:  brew install esptool      (recommended)"
    echo "    Linux:  pip3 install esptool"
    echo
    echo "Note: on recent macOS 'pip3 install esptool' often fails with"
    echo "'externally-managed-environment' even when Python is installed."
    echo "Use Homebrew (brew install esptool) or:  pipx install esptool"
    echo
    read -r -p "Press Enter to close..."
    exit 1
fi

# --- Verify firmware components are available -----
for file in bootloader.bin partition-table.bin qmx_panadapter.bin; do
    if [ ! -f "$file" ]; then
        echo
        echo "ERROR: $file not found."
        echo "This flasher requires:"
        echo "  - bootloader.bin"
        echo "  - partition-table.bin"
        echo "  - qmx_panadapter.bin"
        echo
        read -r -p "Press Enter to close..."
        exit 1
    fi
done

echo "Firmware components found:"
echo "  - bootloader.bin"
echo "  - partition-table.bin"
echo "  - qmx_panadapter.bin"
echo
echo "Before you continue:"
echo "  1. Plug the Tab5 into this computer with a USB-C DATA cable"
echo "     (a charge-only cable will NOT work)."
echo "  2. Close any serial monitor or other app using the port."
echo
echo "------------------------------------------------------------"
echo " FLASH TYPE"
echo "------------------------------------------------------------"
echo "A NORMAL flash keeps all your saved settings."
echo
echo "A CLEAN flash WIPES the whole chip first.  *** WARNING: ***"
echo "    ALL of the following will be PERMANENTLY ERASED:"
echo "      - your WiFi network name AND password"
echo "      - your callsign and Maidenhead grid"
echo "      - ALL saved memory channels"
echo "      - your logged QSOs (the ADIF log)"
echo "    This cannot be undone - you will re-enter everything."
echo "    Only use it if something is stuck or corrupted"
echo "    (e.g. WiFi will not turn on no matter what)."
echo "------------------------------------------------------------"
ERASE_OPT=""
read -r -p "Type E for a CLEAN/ERASE flash, or just Enter for a normal flash: " CLEAN
case "${CLEAN}" in
    [Ee]*|[Yy]*)
        ERASE_OPT="-e"
        echo
        echo "  *** CLEAN FLASH SELECTED - WiFi credentials, callsign/grid, and ALL"
        echo "      memory channels WILL BE ERASED. Press Ctrl+C now to cancel. ***"
        ;;
esac
echo
read -r -p "Press Enter to flash (or Ctrl+C to cancel)... "

echo
echo "Flashing - do NOT unplug the Tab5..."
echo

# Detect esptool version: v5+ uses write-flash (hyphen); older uses write_flash
if "${ESPTOOL}" version 2>/dev/null | grep -qE 'v[5-9]\.'; then
    WRITE_FLASH="write-flash"
else
    WRITE_FLASH="write_flash"
fi

# Try the likely USB-serial devices first, one quick connect attempt each, so
# we hit the right one fast instead of waiting through long retries. Falls back
# to esptool's own auto-detect if none of the usual device names are present.
PORTS="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | sort)"
RC=1
if [ -n "${PORTS}" ]; then
    for P in ${PORTS}; do
        echo "  trying ${P} ..."
        if "${ESPTOOL}" --chip esp32p4 -p "${P}" -b 460800 --connect-attempts 1 "${WRITE_FLASH}" ${ERASE_OPT} 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 qmx_panadapter.bin; then
            RC=0
            break
        fi
    done
else
    "${ESPTOOL}" --chip esp32p4 -b 460800 --connect-attempts 1 "${WRITE_FLASH}" ${ERASE_OPT} 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 qmx_panadapter.bin
    RC=$?
fi

echo
if [ "${RC}" -eq 0 ]; then
    echo "============================================================"
    echo "   SUCCESS - the Tab5 is restarting with the new firmware."
    echo "============================================================"
else
    echo "============================================================"
    echo "   FLASH FAILED - could not flash the Tab5 on any port."
    echo "   - Use a different USB-C cable - it must carry DATA, not"
    echo "     just power. Many cheap cables are charge-only."
    echo "   - Close any program using the serial port and try again."
    echo "   - Linux: if the port is denied, add yourself to dialout:"
    echo "       sudo usermod -aG dialout \$USER   (then log out/in)"
    echo "============================================================"
fi
echo
read -r -p "Press Enter to close..."
