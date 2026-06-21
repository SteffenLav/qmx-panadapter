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

# --- get firmware: download the latest GitHub release, else use a local copy -
REPO="SteffenLav/qmx-panadapter"
echo "Checking GitHub for the latest firmware (needs internet)..."
URL="$(curl -fsSL --max-time 20 -H 'User-Agent: qmx-flasher' \
       "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null \
       | grep -o 'https://[^"]*qmx_panadapter_merged_[^"]*\.bin' | head -n 1)"
if [ -n "${URL}" ]; then
    NAME="$(basename "${URL}")"
    echo "  latest: ${NAME}"
    if curl -fSL --max-time 180 -H 'User-Agent: qmx-flasher' -o "${NAME}.part" "${URL}"; then
        mv -f "${NAME}.part" "${NAME}"
        echo "  download OK."
    else
        rm -f "${NAME}.part"
        echo "  download failed - looking for a local copy..."
    fi
else
    echo "  could not reach GitHub (offline?) - looking for a local copy..."
fi

# Newest .bin in this folder: the freshly-downloaded latest, or a bundled copy.
FW="$(ls -t qmx_panadapter_merged_*.bin 2>/dev/null | head -n 1)"
if [ -z "${FW}" ]; then
    echo
    echo "ERROR: no firmware available."
    echo "  - Connect to the internet so the latest can be downloaded, OR"
    echo "  - put a qmx_panadapter_merged_*.bin in this folder for offline use."
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
        if "${ESPTOOL}" --chip esp32p4 -p "${P}" -b 460800 --connect-attempts 1 "${WRITE_FLASH}" 0x0 "${FW}"; then
            RC=0
            break
        fi
    done
else
    "${ESPTOOL}" --chip esp32p4 -b 460800 --connect-attempts 1 "${WRITE_FLASH}" 0x0 "${FW}"
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
