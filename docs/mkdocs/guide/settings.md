# Settings & Configuration

Swipe ← from the right edge to open the settings drawer. All settings are saved automatically.

## Operator Info

**Callsign** — Your amateur radio callsign (required for FT8/FT4 logging).

**Grid Square** — Your Maidenhead grid square (e.g., JO45; required for FT8/FT4 exchanges).

## WiFi

Tap **WiFi setup** in the settings drawer to open the WiFi window.

**WiFi On/Off** — the WiFi icon button in the WiFi window (shown with a diagonal red slash when off). Toggling it takes effect immediately — off disconnects and stays off; on reconnects right away. Turning WiFi off is useful for field operation (no WiFi overhead, extended battery life).

**SSID** — Your WiFi network name. Tap **Scan** to list nearby networks and pick one.

**Password** — Your WiFi password (pre-filled with your saved password; tap the eye icon to show/hide it). Tap **Save** to store it and connect.

Once connected, the settings show your **IP address** — use this to access the web UI from a browser.

## Time Sync

**SNTP Server** — NTP pool (usually `pool.ntp.org`). Change only if you have a local NTP server.

**Set Manual Time** — Manually enter UTC time (hours, minutes, seconds).

**FT8-Derived Sync** — Estimates the UTC offset from decoded FT8 signal timing. **Offline fallback only**: it is automatically ignored while SNTP or GPS is available (those are authoritative), and only nudges the clock when you're off-grid with no better source. See [Time Sync](../guide/time-sync.md).

**QMX GPS** — Detected **automatically**, no setting to toggle. If your QMX (typically a QMX+) is GPS-disciplined, the Tab5 recognises it at connect by comparing the QMX's own second-tick against SNTP, and then phase-locks to the GPS second boundary (~10 ms) as an offline time source. On a non-GPS QMX nothing happens. The bottom-bar clock shows **UTC(GPS)** when a GPS-disciplined QMX is the active source.

## Display

**Flip 180°** — Invert the display for upside-down mounting or cable routing.

**QMX volume** — The radio's own AF gain, **in decibels: the same number the QMX shows on its own LCD**, not a percentage. Available in both Panadapter and FT8 modes. It reads the radio back each time you open the drawer, so if you turn the volume with the rig's own knob the slider follows instead of disagreeing with it. This exists mainly for QMX+ builds with no control panel, where there is no volume knob at all (Randy N4OPI's request). Nothing is sent to the radio until you move the slider, so switching on can never change your volume unexpectedly.

**Display sleep** — Dropdown (Off / 1 / 2 / 5 / 10 / 30 min). After the chosen idle time the backlight turns off; FT8, the radio link, and the web UI keep running. Tap the screen to wake — the wake tap is swallowed, so it can't tune or press anything. A **two-finger double-tap** blanks the display immediately.

**Brightness** — Screen brightness (0–100%).

**Spectrum Mode** — 
- **Normal** — absolute dBm scale
- **Flat** — relative to per-bin noise floor (signals pop above baseline)

## Battery Care

**Charge Limit** — Optionally stop charging once the battery reaches a set percentage (default **80%**), to reduce long-term wear on the pack. Enable it and choose the target in the settings drawer; charging restarts automatically if the level later falls well below the target (5% hysteresis). Leave it off to always charge to 100%.

**Accurate charge reading while charging** — the displayed battery percentage (and the charge-limit trigger) now compensate for the voltage rise that occurs while charging current is flowing. Previously this made the reading jump around while plugged in, and made the charge limit either stick just short of the target or oscillate; both are fixed.

## Waterfall Controls

**Black Level (dB)** — How far above noise floor to show as black (default 9 dB). Lower = more colour detail.

**Contrast (dB)** — Span of the colour ramp (default 45 dB). Lower = more contrast, higher = more gradation.

**Adaptive Floor (%)** — Blend between per-bin noise floor (100%) and global mean floor (0%; default 100%).

**FFT Window** — 
- **Blackman-Harris** (default) — best frequency resolution
- **Hann** — smoother peaks
- **Nuttall** — sharpest edges

## Panadapter & Zoom

**Distance in Miles** — Show FT8 distances in miles instead of km (off by default).

**Band-plan region** — Sets which region's band plan drives the coloured CW/Digi/Phone strip along the bottom of the screen. **Auto** derives it from your grid square; you can also force Region 1/2/3.

**Band Presets** — Add or remove custom bands. Standard bands (160–10 m) are always available.

## FT8 Settings


**FT8 On/Off** — Globally enable/disable FT8 mode.

**Field Day Mode** — ARRL Field Day mode (on/off, class, section).

**Simulation Mode** — Practice QSOs with six phantom stations — no QMX needed at all, radio never keyed (red breathing border on screen). See [FT8 Transmit](ft8-tx.md#4-ft8-simulation-mode-practice-qsos).

**Fast pounce (early decode)** — On by default. Decodes surface ~1.8 s *before* the slot boundary (WSJT-X style), so a fresh CQ can be answered in the very next slot and mid-QSO replies land on the beat. Trade-off: the capture window closes early, so a station transmitting late in the slot can occasionally be missed. ⚠️ *Not yet A/B-verified on a live band — if your decodes-per-slot drop with it on, turn it off and please report your numbers.*

**Distance in miles** — Show the decode list's distance column in miles instead of kilometres.

**Report to PSK Reporter** — **On by default.** Uploads the stations you decode to [PSK Reporter](https://pskreporter.info), the same as WSJT-X, so you appear on the map as a monitoring station and other operators can see where they were heard.

What is sent, over the internet only and **never on the air**: your callsign and grid square, and for each station you decode their callsign, their grid (if their message contained one), the frequency, the signal report and the mode. Reports are batched and sent at most once every five minutes.

It does nothing at all until both your **callsign and grid** are set, and it is disabled outright in **Simulation Mode**, so practice contacts can never reach the public map. Uncheck this box to switch it off entirely.

> The first report goes out 5–5½ minutes after switching on, so nothing appears immediately. To check it is working, look for **QMX Panadapter** under "Software in use" at [pskreporter.info/cgi-bin/pskstats.pl](https://pskreporter.info/cgi-bin/pskstats.pl).

**FT8 Filters** — Include/exclude stations, set auto-reply priority, enable robot mode, grey-listing (see [FT8 Receive](ft8-rx.md) for details).

**Keyboard** — M5Stack Tab5 snap-on keyboard support (if connected).

## Audio & DSP

**IQ Balance** — Adaptive I/Q phase correction (usually on). Suppresses mirror-image signals.

## Diagnostic Logging

The diagnostic log is **always on** — there is nothing to enable. All firmware log output is captured to a 5 MB memory ring, with a rolling copy persisted to internal flash (survives a reboot or power-off) and, if a microSD card is inserted, mirrored to the card as well (see [microSD Auto-Archive](#microsd-auto-archive-station-backup) for when the card copy is written — the flash copy is always complete). Download via:

- Web UI: open the **Files** menu in the bottom bar and click **Diagnostic download ↓** (downloads both the live session log and the flash-persisted copy from before the last reboot)
- microSD card: `/qmx-panadapter/qmx-log.txt`
- USB serial: `tools/capture_serial_log.ps1`

Useful for troubleshooting rare issues.

## microSD Auto-Archive — Station Backup

Insert a microSD card (FAT32 or exFAT, any size — a plain 32 GB FAT32 card is ideal) **before switching the Tab5 on** and it automatically mirrors your whole station to `/qmx-panadapter/` on the card. It's a **grab-and-go backup**: pull the card into a PC (or another Tab5) to back up or move your setup — no computer needed in the field.

| File | Contents |
|------|----------|
| `qso.adi` | ADIF QSO log — after each new entry with WiFi off, otherwise at the next start-up |
| `qmx-config.txt` | All settings + memory channels, as INI text (restore via **Config** upload) |
| `lotw_cert.b64`, `lotw_key.b64` | Your LoTW signing certificate + private key, so a restored device can sign for LoTW |
| `qmx-log.txt` (+`.1`) | Diagnostic log, rolling (rotated at 5 MB) |
| `README.txt` | A plain-text description of every file, written on each mount |

**Insert the card before switching the Tab5 on.** A card pushed in later is not picked up until the next start-up — the Tab5 can only claim the card during a short window early in boot.

### When the mirror runs

The microSD card and the WiFi co-processor share a bus on this hardware and cannot both use it reliably. Rather than fail at an unpredictable moment, the Tab5 picks the behaviour that works:

| | What happens | SD dot |
|---|---|---|
| **WiFi off** (POTA/SOTA) | Continuous mirroring the whole time the card is in | **Green** |
| **WiFi on** | One complete backup within a few seconds of switching on, then mirroring stops | **Yellow** |

Either way your QSO log, config, and LoTW certificate and key are backed up. With WiFi on, QSOs made later in that session reach the card at the **next start-up** — so if you have been operating with WiFi up and want them on the card now, restart the Tab5.

If no card is inserted the dot is absent, which is not an error.

> **⚠️ The card holds credentials.** A full backup that can *restore* a station necessarily includes secrets: `qmx-config.txt` stores your WiFi password and QRZ/eQSL logins in clear text, and `lotw_key.b64` is your LoTW **private key**. Keep the card as physically secure as a house key. (The on-card `README.txt` repeats this warning.)

> The diagnostic log is always-on regardless of whether an SD card is present. If no card is inserted, the log still persists to internal flash (see [Diagnostic Logging](#diagnostic-logging) above) and survives a power-off.

## ADIF & Logging

**ADIF Log** — View the QSO log on-device. The viewer shows a proper column table:

| Column | Content |
|--------|---------|
| Call | Callsign |
| Country | DXCC entity (looked up from the callsign prefix) |
| Mode | FT8 or FT4 |
| Band | Band (20m, 40m, …) |
| Date | UTC date |
| Time | UTC time |
| Sent | Your signal report (SNR) |
| Rcvd | Their signal report (SNR) |

A sticky header row stays pinned while you scroll. Even-numbered rows are lightly shaded so long logs stay easy to scan.

**Today/All filter** — the viewer opens on **Today** (falling back to All when nothing was logged today). The toggle button shows the view you *switch to* by pressing it; the title shows the current view with counts.

**POTA activation counter** — in the Today view the title reads "Today: N (M total)" and turns **green** once today reaches 10 QSOs — a valid POTA activation.

**Delete a single record** — **long-press** a QSO row: the row highlights red and list scrolling locks. Drag up/down to move the highlight, then release — a Delete/Cancel bar appears at the bottom. **Delete** removes just that one record (useful for duplicates).

Tap **Clear** to erase the log (irreversible). Use the web UI to download the full ADIF file for import into WSJT-X, EQSL, or any other logging software.

**Exclude Worked Before** — When FT8 filtering, skip stations you've already logged QSOs with (requires you to import your own prior ADIF log first).

## Config Import/Export

**Config Download** — Export all settings + memory channels + ADIF log as a text file (INI format).

**Config Upload** — Restore settings from a backup file (settings only; memory channels merge).

Use this to:
- Back up your settings before a factory reset
- Transfer settings to another Tab5
- Recover from accidental changes

## Advanced / Expert

**Reset to Defaults** — Wipe all user settings and return to factory defaults. ADIF log is preserved. Use only if something is stuck.

**Factory Reset (Full)** — Erase everything including ADIF log. Use only as a last resort.

---

**Next:** Troubleshoot an issue via [Troubleshooting](../reference/troubleshooting.md) or explore the [API](../reference/web-api.md).
