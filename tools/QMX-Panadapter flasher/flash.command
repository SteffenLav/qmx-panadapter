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

# --- esptool version ------------------------------------------------------
# Two things depend on it:
#   1. v5+ renamed the subcommands (write_flash -> write-flash).
#   2. the ESP32-P4 flasher stub. Mark Hogan N5OBC (Linux Mint, esptool 4.7.0
#      from the distro package) got as far as "Running stub..." and the flash
#      then sat there forever - the stub is uploaded, the ROM jumps to it, and
#      it never answers. We build and test with 4.12.0. Rather than guess a
#      cutoff version, the script PROBES the stub below and falls back to
#      --no-stub when it does not answer.
ESPTOOL_VER="$("${ESPTOOL}" version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
case "${ESPTOOL_VER}" in
    [5-9].*|[1-9][0-9].*) WRITE_FLASH="write-flash"; FLASH_ID="flash-id" ;;
    *)                    WRITE_FLASH="write_flash"; FLASH_ID="flash_id" ;;
esac

# --- Verify firmware components are available -----
NEEDED="bootloader.bin partition-table.bin qmx_panadapter.bin ota_data_initial.bin"
for file in ${NEEDED}; do
    if [ ! -f "$file" ]; then
        echo
        echo "ERROR: $file not found."
        echo "This flasher requires:"
        for n in ${NEEDED}; do echo "  - $n"; done
        echo
        read -r -p "Press Enter to close..."
        exit 1
    fi
done

echo "Firmware components found:"
for n in ${NEEDED}; do echo "  - $n"; done
echo
echo "esptool: ${ESPTOOL} ${ESPTOOL_VER:-(unknown version)}"
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

# --- pick the port, and decide whether the stub works on it ----------------
# Probing with flash_id costs a second or two and writes NOTHING, so it is safe
# to put a hard time limit on it. The real write_flash is never time limited -
# killing that mid-write would leave a half-written flash.
# 'timeout' is coreutils: present on Linux, usually absent on stock macOS.
TIMEOUT=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 45"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT="gtimeout 45"
fi

PORTS="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | sort)"
PORT=""
STUB_OPT=""
if [ -n "${TIMEOUT}" ] && [ -n "${PORTS}" ]; then
    echo
    echo "Looking for the Tab5..."
    for P in ${PORTS}; do
        echo "  probing ${P} ..."
        if ${TIMEOUT} "${ESPTOOL}" --chip esp32p4 -p "${P}" --connect-attempts 1 "${FLASH_ID}" >/dev/null 2>&1; then
            PORT="${P}"; STUB_OPT=""; break
        fi
        if ${TIMEOUT} "${ESPTOOL}" --chip esp32p4 -p "${P}" --connect-attempts 1 --no-stub "${FLASH_ID}" >/dev/null 2>&1; then
            PORT="${P}"; STUB_OPT="--no-stub"
            echo
            echo "  NOTE: esptool's flasher stub does not run on this Tab5, so the"
            echo "        flash will use --no-stub. It is slower but it works."
            echo "        A newer esptool usually fixes it: pip3 install -U esptool"
            echo
            break
        fi
    done
fi

# --no-stub cannot erase: erase_flash and erase_region are stub-only commands
# in esptool (verified in esptool/loader.py - both carry @stub_function_only).
# Flash the firmware anyway rather than leave the user with nothing, and say
# plainly afterwards that the settings were NOT erased.
NO_ERASE_WARN=0
if [ "${STUB_OPT}" = "--no-stub" ] && [ -n "${ERASE_OPT}" ]; then
    ERASE_OPT=""
    NO_ERASE_WARN=1
fi

echo
echo "Flashing - do NOT unplug the Tab5..."
echo

RC=1
if [ -n "${PORT}" ]; then
    "${ESPTOOL}" --chip esp32p4 -p "${PORT}" -b 460800 --connect-attempts 1 ${STUB_OPT} "${WRITE_FLASH}" ${ERASE_OPT} 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 qmx_panadapter.bin 0x920000 ota_data_initial.bin
    RC=$?
elif [ -n "${PORTS}" ]; then
    # No probe (no 'timeout' on this machine) or nothing answered it: try each
    # of the likely devices in turn, one quick connect attempt each.
    for P in ${PORTS}; do
        echo "  trying ${P} ..."
        if "${ESPTOOL}" --chip esp32p4 -p "${P}" -b 460800 --connect-attempts 1 "${WRITE_FLASH}" ${ERASE_OPT} 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 qmx_panadapter.bin 0x920000 ota_data_initial.bin; then
            RC=0
            break
        fi
    done
else
    "${ESPTOOL}" --chip esp32p4 -b 460800 --connect-attempts 1 "${WRITE_FLASH}" ${ERASE_OPT} 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 qmx_panadapter.bin 0x920000 ota_data_initial.bin
    RC=$?
fi

echo
if [ "${RC}" -eq 0 ]; then
    echo "============================================================"
    echo "   SUCCESS - the Tab5 is restarting with the new firmware."
    echo "============================================================"
    if [ "${NO_ERASE_WARN}" -eq 1 ]; then
        echo
        echo "   *** Your settings were NOT erased. ***"
        echo "   You asked for a CLEAN flash, but erasing needs esptool's"
        echo "   flasher stub, and the stub does not run on this machine."
        echo "   The firmware IS installed. For a real clean flash run:"
        echo "       pip3 install -U esptool"
        echo "   then start this flasher again and type E."
    fi
else
    echo "============================================================"
    echo "   FLASH FAILED - could not flash the Tab5 on any port."
    echo "   - Use a different USB-C cable - it must carry DATA, not"
    echo "     just power. Many cheap cables are charge-only."
    echo "   - Close any program using the serial port and try again."
    echo "   - Update esptool:  pip3 install -U esptool"
    echo "     (you have ${ESPTOOL_VER:-an unknown version}; the firmware is"
    echo "      built and tested with 4.12)"
    echo "   - Linux: if the port is denied, add yourself to dialout:"
    echo "       sudo usermod -aG dialout \$USER   (then log out/in)"
    echo "   - Linux: ModemManager can grab /dev/ttyACM0 as it appears. If"
    echo "     the flash stalls, try: sudo systemctl stop ModemManager"
    echo "============================================================"
fi
echo
read -r -p "Press Enter to close..."
