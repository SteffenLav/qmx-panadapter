# Build from Source

For developers who want to modify the firmware or contribute.

## Prerequisites

- **ESP-IDF v5.4.4** (exact version — pinned for ESP32-P4 ECO2 silicon)
- **Python 3.8+** (for idf.py build tools)
- **esptool** (for flashing)
- **Git**

### Install ESP-IDF (Windows)

Download and run the [ESP-IDF Windows Installer](https://github.com/espressif/idf-installer/releases):

1. Accept defaults
2. Installs to `C:\esp\v5.4.4\esp-idf`
3. Adds `idf.py` to your PATH

### Install ESP-IDF (macOS)

```bash
mkdir ~/esp
cd ~/esp
git clone -b v5.4.4 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh
source export.sh  # or: . ./export.ps1 (PowerShell)
```

### Install ESP-IDF (Linux)

```bash
mkdir ~/esp
cd ~/esp
git clone -b v5.4.4 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh
source export.sh
```

## Clone the Repository

```bash
git clone https://github.com/SteffenLav/qmx-panadapter.git
cd qmx-panadapter
```

## Build

**Activate the IDF environment first:**

=== "Windows (PowerShell)"

    ```powershell
    & "C:\esp\v5.4.4\esp-idf\export.ps1"
    idf.py build
    ```

=== "macOS/Linux (Bash)"

    ```bash
    source ~/esp/esp-idf/export.sh
    idf.py build
    ```

The build produces:

- `build/qmx_panadapter.elf` — executable (ELF format), the file you need to decode a panic backtrace
- `build/qmx_panadapter.bin` — the application image, flashed at `0x10000`
- `build/bootloader/bootloader.bin` — flashed at `0x2000`
- `build/partition_table/partition-table.bin` — flashed at `0x8000`

There is no combined single-file image. The three components are flashed to their
own addresses, which is also what the released flasher does.

## Flash

**Option 1: Manual flasher**

```bash
idf.py flash monitor
```

This flashes all three components to their own addresses and opens a serial monitor. Exit with `Ctrl+T` then `Ctrl+X`.

**Option 2: Released flasher**

Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases) — it's the same firmware, just packaged for non-developers.

## Configuration

The build uses **sdkconfig** for ESP-IDF settings:

```bash
idf.py menuconfig
```

**Do not change:**
- `CONFIG_ESP32P4_REV_MIN_0=y` — required for ESP32-P4 ECO2 silicon
- CPU clock (pinned at 360 MHz) — higher clocks violate ECO2 spec
- LVGL settings — pre-tuned for the display

## Architecture

See [Architecture](architecture.md) for the module map and data flow.

## Feedback

This is a solo project and isn't accepting outside code contributions right now, but bug reports and feature requests are very welcome. See [Contributing & Feedback](contributing.md).

## Clean Build

If you encounter strange build errors:

```bash
idf.py fullclean
idf.py build
```

This erases the build directory and rebuilds from scratch.

## Troubleshooting

### "IDF_PATH environment variable needs to be set"

The IDF environment is not active.

**Fix:** Activate it before building:

```powershell
& "C:\esp\v5.4.4\esp-idf\export.ps1" | Out-Null
```

(The `| Out-Null` suppresses verbose output.)

### "esptool not found"

esptool is not installed or not in PATH.

**Fix:**

```bash
pip install esptool
```

### "Board not detected" (serial port error)

The Tab5 is not connected or the USB driver is missing.

**Fix:**

1. Plug in Tab5 with a **data cable** (not charge-only)
2. Wait 2 seconds for enumeration
3. Check Device Manager (Windows) for a new COM port
4. Run `idf.py flash -p <COM#>` to specify the port explicitly

### Bootloader hangs / loops on startup

Almost always a wrong flash address. The application goes to **`0x10000`**, never
to `0x0` — `0x0` is where the bootloader's own image lives, and writing the
application over it leaves a device that cannot start. `idf.py flash` uses the
right addresses on its own; the risk is a hand-written `esptool write-flash`.

**Fix:**

Erase the entire chip and flash again:

```bash
esptool.py --chip esp32p4 -p /dev/ttyUSB0 erase_flash
idf.py flash monitor
```

---

**Next:** Understand the [Architecture](architecture.md) or read the [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md) technical guide.
