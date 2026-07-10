# Quick Start

Get your QMX Panadapter on air in 10 minutes.

## Prerequisites

- **M5Stack Tab5** (ESP32-P4, 5" display)
- **QRP Labs QMX or QMX+** transceiver
- **USB-C power cable** (5 V / 2 A minimum)
- **USB-A to USB-C data cable** (connects Tab5 to QMX)
- **WiFi network** (optional, for remote control and logging)

## Step 1: Verify QMX Firmware

Power the QMX on standalone and check the firmware version displayed on its screen. You need **v1.03.002 or newer**. If yours is older, update the QMX first — everything that follows depends on recent firmware. Both **v1.03.002** and the **v1.04.002 beta** work with the panadapter; on v1.04.002 you additionally get **AM mode** and an **Antenna Tune** button (SWR tune with a live power/SWR readout). Both stay hidden on v1.03.002, so either firmware is fine.

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

1. **Tab5 first** — turn it on. The screen fades up gently from black as it loads.
2. **QMX second** — turn on the QMX. Until the radio is connected, the Tab5 shows a full-screen **"turn on / reboot your QMX"** prompt — that's normal, not a fault, and it clears automatically once the QMX is talking.
3. Within a few seconds the top bar should show Band / Mode / BW, and the spectrum should come alive

## Step 5: Fill in Your Settings

On first boot you'll be prompted for:

- **Callsign** — your amateur radio callsign
- **Grid** — your Maidenhead grid square (e.g., JO45)
- **WiFi** — SSID and password (optional, skip if you're portable)

You can change these anytime by swiping right from the screen edge to open the settings drawer.

## Step 5b: Optional — microSD Card

> **⏸️ Automatic SD backup is disabled in v0.20.0** — mounting the card competes for memory with the FT8 decoder, so it's switched off this release (it returns once that cost is reduced). You can skip this step: your logs and settings live in the device's own storage regardless, and download the same way.

When enabled, inserting a microSD card into the Tab5's card slot mirrors three things to the card automatically:

- **Diagnostic log** — continuous rolling copy, survives power-off
- **ADIF QSO log** — mirrored automatically after each new QSO
- **Config export** — your settings, for easy transfer to another unit

A green **SD** dot in the bottom status bar confirms a card is mounted and being mirrored — it stays dark while the feature is disabled.

## Step 6: Navigate the Screen

The entire app runs on **edge swipes and taps on the top bar**:

| Action | Does |
|---|---|
| Swipe → from **left edge** | Toggle Panadapter ↔ FT8/FT4 view |
| Swipe ← from **right edge** | Open settings drawer |
| Swipe ↑ from **bottom edge** | Open memory channel picker |
| Tap any **top bar item** | Open that item's selector (Freq, Mode, BW, etc.) |

You'll see faint "breathing" grip handles on the edges — they show you where to swipe.

## Step 7: Start Listening

You're now receiving. The **spectrum** shows signal strength across the band, and the **waterfall** scrolls down showing signal activity over time.

- **Tap the spectrum or waterfall** to tune to that frequency
- **Pinch** to zoom in/out; **one-finger horizontal drag** to pan and retune
- **Tap the coloured band-plan strip** (along the bottom, just above the status bar) to jump to a frequency, or **drag along it** to scrub through the band
- **Tap the Freq label** on the top bar to open the frequency keypad — drag the "Enter freq" title bar to reposition it, pinch or swipe up/down to resize it
- **Swipe ↑ from the bottom edge** to open memory channels — tap an empty slot to create one, long-press a filled slot to edit it, long-press and drag to move a channel (or drag it onto the **wastebin** cell to delete it). A new device comes with a few example channels and plays a one-time tour of these gestures the first time you open the picker
- **Tap Band** to switch between your configured bands
- **Tap Mode** to switch USB/LSB/CW/DiGi

## Step 8: FT8 Mode (Optional)

Swipe → from the left edge to switch to **FT8 view**. You'll see:

- **Live decode list** — all FT8 stations heard on frequency
- **Waterfall** — same real-time spectrum waterfall
- **Call CQ** button — transmit a CQ (requires QMX to be on the air)
- **Reply** rows — tap a station to reply to their CQ

> **⏸️ FT4 is temporarily disabled in v0.20.0** — it was exhausting the device's internal memory and crashing, so it's switched off this release. Fully reversible, and FT8 is unaffected.

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
- Check QMX firmware version (Step 1 above) — should be v1.03.002 or newer (the v1.04.002 beta also works)
- Try a different USB cable

**WiFi won't connect?**
- Check your SSID and password spelling
- Restart the Tab5 and try again
- See [Troubleshooting](reference/troubleshooting.md) for more

**Still stuck?**
- The **diagnostic log is always on** — nothing to enable. Download it via the web UI (**Diag ↓** in the bottom bar; **Diag(saved) ↓** for the copy from before the last reboot), or pull `qmx-log.txt` from an inserted microSD card
- Report the issue on [GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) or the [QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643) and attach the log

---

**Next steps:** Read the full [User Guide](guide/panadapter.md) or dive into [FT8 Transmit](guide/ft8-tx.md).
