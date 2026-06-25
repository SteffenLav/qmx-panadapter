#!/bin/bash
set -e

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

echo ""
echo "════════════════════════════════════════════════════════════════"
echo " STEP 1: FULL CHIP ERASE"
echo "════════════════════════════════════════════════════════════════"
echo ""

python3 "$ESPTOOL" --chip esp32p4 -p /dev/cu.usbserial-* -b 460800 erase_flash || {
    echo "ERROR: Erase failed. Check USB connection."
    exit 1
}

echo ""
echo "════════════════════════════════════════════════════════════════"
echo " STEP 2: FLASHING BOOTLOADER + PARTITION TABLE + APP"
echo "════════════════════════════════════════════════════════════════"
echo ""

python3 "$ESPTOOL" --chip esp32p4 -p /dev/cu.usbserial-* -b 460800 \
    write_flash \
    0x2000 "$SCRIPT_DIR/bootloader.bin" \
    0x10000 "$SCRIPT_DIR/qmx_panadapter_merged_v0.18.5-hotfix.bin" \
    0x8000 "$SCRIPT_DIR/partition-table.bin"

if [ $? -ne 0 ]; then
    echo "ERROR: Flash failed"
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
