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

# Find esptool - check PATH first (homebrew), then local
ESPTOOL=""
if command -v esptool.py &> /dev/null; then
    ESPTOOL="esptool.py"
elif command -v esptool &> /dev/null; then
    ESPTOOL="esptool"
fi

if [ -z "$ESPTOOL" ]; then
    echo ""
    echo "ERROR: esptool not found in PATH"
    echo ""
    echo "Install via homebrew:"
    echo "  brew install esptool"
    echo ""
    exit 1
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
echo "     (it will power on automatically)"
echo "  2. Close any serial monitor programs"
echo ""
read -p "Press Enter when ready..." dummy

echo ""
echo "════════════════════════════════════════════════════════════════"
echo " STEP 1: FULL CHIP ERASE"
echo "════════════════════════════════════════════════════════════════"
echo ""

"$ESPTOOL" --chip esp32p4 -b 460800 erase_flash || {
    echo "ERROR: Erase failed. Check USB connection."
    exit 1
}

echo ""
echo "════════════════════════════════════════════════════════════════"
echo " STEP 2: FLASHING BOOTLOADER + PARTITION TABLE + APP"
echo "════════════════════════════════════════════════════════════════"
echo ""

"$ESPTOOL" --chip esp32p4 -b 460800 \
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
