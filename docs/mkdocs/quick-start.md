# Quick Start

Get your QMX Panadapter on air in 10 minutes.

## Prerequisites

- **M5Stack Tab5** (ESP32-P4, 5" display)
- **QRP Labs QMX or QMX+** transceiver
- **USB-C power cable** (5 V / 2 A minimum)
- **USB-A to USB-C data cable** (connects Tab5 to QMX)
- **WiFi network** (optional, for remote control and logging)

## Step 1: Verify QMX Firmware

Power the QMX on standalone and check the firmware version displayed on its screen. You need **v1.03.002**. If yours is older, update the QMX first — everything that follows depends on recent firmware. The **v1.04 beta** is not yet verified with the panadapter — stick with v1.03.002 for now if you want a known-good combination.

## Step 2: Flash the Tab5

Download the latest flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases):

=== "Windows"

    1. Plug Tab5 into your computer with a **USB-C data cable**
    2. Double-click `flash.bat`
    3. Press **Enter** for a normal flash (keeps your settings) or type **E** for a clean flash (wipes everything)
    4. Wait for `SUCCESS`

=== "macOS"

    1. Plug Tab5 into your computer with a **USB-C data cable**
    2. Open Terminal and run: `bash flash.command`
    3. If you haven't installed esptool: `brew install esptool`
    4. Press **Enter** for normal flash
    5. Wait for `SUCCESS`

=== "Linux"

    1. Plug Tab5 into your computer with a **USB-C data cable**
    2. Run: `bash flash.command`
    3. If you haven't installed esptool: `pip3 install esptool`
    4. Press **Enter** for normal flash
    5. Wait for `SUCCESS`

The flasher automatically downloads the latest firmware from GitHub. If you're offline, place a `qmx_panadapter_merged_*.bin` file next to the flasher and it will use that instead.

## Step 3: Connect the Cables

You need **two USB connections**:

| Connection | Tab5 Port | Cable | Carries |
|---|---|---|---|
| Tab5 ← QMX | USB-A (host) | USB-A to USB-C, **data cable** | I/Q audio + CAT control |
| Tab5 ← Power | USB-C | Any USB-C power cable | 5V power |

⚠️ **Cable gotcha:** Many USB-C cables are charge-only. If the spectrum stays flat and the top bar shows `Band: ---`, you have a charge-only cable. Swap it for one you know does data (USB stick, phone sync, etc.).

## Step 4: Power On

1. **Tab5 first** — turn it on and wait for it to fully load
2. **QMX second** — turn on the QMX
3. Within a few seconds the top bar should show Band / Mode / BW, and the spectrum should come alive

## Step 5: Fill in Your Settings

On first boot you'll be prompted for:

- **Callsign** — your amateur radio callsign
- **Grid** — your Maidenhead grid square (e.g., JO45)
- **WiFi** — SSID and password (optional, skip if you're portable)

You can change these anytime by swiping right from the screen edge to open the settings drawer.

## Step 6: Navigate the Screen

The entire app runs on **edge swipes and taps on the top bar**:

| Action | Does |
|---|---|
| Swipe → from **left edge** | Toggle Panadapter ↔ FT8 view |
| Swipe ← from **right edge** | Open settings drawer |
| Swipe ↑ from **bottom edge** | Open memory channel picker |
| Tap any **top bar item** | Open that item's selector (Freq, Mode, BW, etc.) |

You'll see faint "breathing" grip handles on the edges — they show you where to swipe.

## Step 7: Start Listening

You're now receiving. The **spectrum** shows signal strength across the band, and the **waterfall** scrolls down showing signal activity over time.

- **Tap the spectrum** to tune to that frequency
- **Pinch and drag** to zoom in/out
- **Tap Band** to switch between your configured bands
- **Tap Mode** to switch USB/LSB/CW/DiGi

## Step 8: FT8 Mode (Optional)

Swipe → from the left edge to switch to **FT8 view**. You'll see:

- **Live decode list** — all FT8 stations heard on frequency
- **Waterfall** — same real-time spectrum waterfall
- **Call CQ** button — transmit a CQ (requires QMX to be on the air)
- **Reply** rows — tap a station to reply to their CQ

⚠️ **FT8/FT4 transmit is beta** — functional but not yet soaked for multi-hour sessions (FT4 TX is new in v0.19.0). Use a dummy load for your first tests.

## Step 9: WiFi & Web UI (Optional)

If you set up WiFi:

1. Open your browser to `http://<ip-address>` (Tab5 displays its IP in the settings)
2. You'll see a remote spectrum, waterfall, and control panel
3. No installation or configuration needed — just open and go

## Troubleshooting

**Spectrum is flat?**
- Check the USB cable between Tab5 and QMX — charge-only cables won't work
- Verify QMX is powered on and not in standby
- Restart both devices (Tab5 first, then QMX)

**QMX doesn't appear?**
- Check QMX firmware version (Step 1 above) — should be v1.03.002
- Try a different USB cable

**WiFi won't connect?**
- Check your SSID and password spelling
- Restart the Tab5 and try again
- See [Troubleshooting](reference/troubleshooting.md) for more

**Still stuck?**
- Enable the **Diagnostic log** in the settings drawer, then report it on [GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) or the [QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643)
- Download the log via the web UI and include it with your report

---

**Next steps:** Read the full [User Guide](guide/panadapter.md) or dive into [FT8 Transmit](guide/ft8-tx.md).
