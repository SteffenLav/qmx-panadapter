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
    echo "    pip3 install esptool        (any OS with Python)"
    echo "    brew install esptool        (macOS with Homebrew)"
    echo
    read -r -p "Press Enter to close..."
    exit 1
fi

# --- find the firmware .bin (newest by mtime) next to this script -----------
FW="$(ls -t qmx_panadapter_merged_*.bin 2>/dev/null | head -n 1)"
if [ -z "${FW}" ]; then
    echo "ERROR: No qmx_panadapter_merged_*.bin found in this folder."
    echo "Put this script in the same folder as the firmware .bin file."
    echo
    read -r -p "Press Enter to close..."
    exit 1
fi

echo "Firmware found: ${FW}"
echo
echo "Before you continue:"
echo "  1. Plug the Tab5 into this computer with a USB-C DATA cable"
echo "     (a charge-only cable will NOT work)."
echo "  2. Close any serial monitor or other app using the port."
echo
read -r -p "Press Enter to flash (or Ctrl+C to cancel)... "

echo
echo "Flashing - do NOT unplug the Tab5..."
echo
"${ESPTOOL}" --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash 0x0 "${FW}"
RC=$?

echo
if [ "${RC}" -eq 0 ]; then
    echo "============================================================"
    echo "   SUCCESS - the Tab5 is restarting with the new firmware."
    echo "============================================================"
else
    echo "============================================================"
    echo "   FLASH FAILED  (exit code ${RC})"
    echo "   - Use a different USB-C cable - it must carry DATA, not"
    echo "     just power. Many cheap cables are charge-only."
    echo "   - Close any program using the serial port and try again."
    echo "   - Linux: if the port is denied, add yourself to dialout:"
    echo "       sudo usermod -aG dialout \$USER   (then log out/in)"
    echo "============================================================"
fi
echo
read -r -p "Press Enter to close..."
