#!/bin/bash
# NOTE: deliberately no "set -e" - the port-retry loop below expects
# individual esptool attempts to fail (wrong port) without killing the whole
# script before it gets to try the next one or print the proper error.

clear
cat << 'EOF'
════════════════════════════════════════════════════════════════
   QMX Panadapter RECOVERY FLASHER - Bootloader Repair (macOS)
════════════════════════════════════════════════════════════════

This script RECOVERS a Tab5 with a corrupted bootloader
(from the faulty v0.18.5-hotfix flash).

WARNING: This will ERASE the entire Tab5 chip.
All settings, WiFi passwords, and logs will be DELETED.

Press Enter to continue, or Ctrl+C to abort...
EOF
read -p "" dummy

# Detect script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Find esptool
ESPTOOL=""
if [ -f "$SCRIPT_DIR/esptool" ]; then
    ESPTOOL="$SCRIPT_DIR/esptool"
elif command -v esptool &> /dev/null; then
    ESPTOOL="esptool"
elif command -v esptool.py &> /dev/null; then
    ESPTOOL="esptool.py"
fi

if [ -z "$ESPTOOL" ]; then
    echo ""
    echo "esptool not found. Downloading from GitHub..."

    # Download esptool
    TEMP_ZIP=$(mktemp /tmp/esptool.XXXXXX.zip)
    curl -s https://api.github.com/repos/espressif/esptool/releases/latest \
        | grep "browser_download_url.*macos" | head -1 | cut -d '"' -f 4 | xargs curl -L -o "$TEMP_ZIP"

    unzip -q "$TEMP_ZIP" -d "$SCRIPT_DIR/esptool_tmp"
    ESPTOOL="$SCRIPT_DIR/esptool_tmp/esptool.py"
    rm "$TEMP_ZIP"
fi

# Check for recovery files
if [ ! -f "$SCRIPT_DIR/bootloader.bin" ]; then
    echo ""
    echo "ERROR: bootloader.bin not found"
    echo ""
    echo "You need the v0.18.5-hotfix recovery ZIP:"
    echo "Download: https://github.com/SteffenLav/qmx-panadapter/releases/tag/v0.18.5-hotfix-RECOVERY"
    echo "Extract and run this script from inside the extracted folder"
    exit 1
fi

echo ""
echo "Before continuing:"
echo "  1. Plug Tab5 into this Mac with a USB-C DATA cable"
echo "  2. Tab5 should be powered ON"
echo "  3. Close any serial monitor programs"
echo ""
read -p "Press Enter when ready..." dummy

# Detect esptool version: v5+ uses hyphenated subcommands (write-flash,
# erase-flash); older uses underscores. See feedback_esptool_write_flash_hyphen.
if "$ESPTOOL" version 2>/dev/null | grep -qE 'v[5-9]\.'; then
    WRITE_FLASH="write-flash"
    ERASE_FLASH="erase-flash"
else
    WRITE_FLASH="write_flash"
    ERASE_FLASH="erase_flash"
fi

# Try the likely USB-serial devices first, one quick connect attempt each -
# /dev/cu.usbserial-* alone was wrong for this hardware (the Tab5's
# ESP32-P4 USB-Serial/JTAG typically enumerates as usbmodem, not usbserial,
# on macOS). Falls back to esptool's own auto-detect if none match.
PORTS="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | sort)"

echo ""
echo "════════════════════════════════════════════════════════════════"
echo " STEP 1: FULL CHIP ERASE"
echo "════════════════════════════════════════════════════════════════"
echo ""

RC=1
if [ -n "${PORTS}" ]; then
    for P in ${PORTS}; do
        echo "  trying ${P} ..."
        if "$ESPTOOL" --chip esp32p4 -p "${P}" -b 460800 --connect-attempts 1 "${ERASE_FLASH}"; then
            RC=0
            break
        fi
    done
else
    "$ESPTOOL" --chip esp32p4 -b 460800 --connect-attempts 1 "${ERASE_FLASH}"
    RC=$?
fi

if [ "${RC}" -ne 0 ]; then
    echo "ERROR: Erase failed on every detected port. Check USB connection"
    echo "(must be a DATA cable, not charge-only) and that no other program"
    echo "(serial monitor, etc.) is using the port."
    exit 1
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
echo " STEP 2: FLASHING BOOTLOADER + PARTITION TABLE + APP"
echo "════════════════════════════════════════════════════════════════"
echo ""

RC=1
if [ -n "${PORTS}" ]; then
    for P in ${PORTS}; do
        echo "  trying ${P} ..."
        if "$ESPTOOL" --chip esp32p4 -p "${P}" -b 460800 --connect-attempts 1 \
            "${WRITE_FLASH}" \
            0x2000 "$SCRIPT_DIR/bootloader.bin" \
            0x10000 "$SCRIPT_DIR/qmx_panadapter_merged_v0.18.5-hotfix.bin" \
            0x8000 "$SCRIPT_DIR/partition-table.bin"; then
            RC=0
            break
        fi
    done
else
    "$ESPTOOL" --chip esp32p4 -b 460800 --connect-attempts 1 \
        "${WRITE_FLASH}" \
        0x2000 "$SCRIPT_DIR/bootloader.bin" \
        0x10000 "$SCRIPT_DIR/qmx_panadapter_merged_v0.18.5-hotfix.bin" \
        0x8000 "$SCRIPT_DIR/partition-table.bin"
    RC=$?
fi

if [ "${RC}" -ne 0 ]; then
    echo "ERROR: Flash failed on every detected port"
    exit 1
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "    ✅ RECOVERY COMPLETE"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "The Tab5 is now recovering... it should power on within 5 seconds."
echo "You will see the WiFi setup screen (normal for first boot)."
echo ""
read -p "Press Enter when done..." dummy
