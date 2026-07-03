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

**Use FT8-Derived Sync** — Estimate UTC offset from decoded FT8 signals (optional, for offline use).

**Sync from QMX GPS** — If enabled and your QMX has GPS, sync the RTC every 5 minutes.

## Display

**Flip 180°** — Invert the display for upside-down mounting or cable routing.

**Brightness** — Screen brightness (0–100%).

**Spectrum Mode** — 
- **Normal** — absolute dBm scale
- **Flat** — relative to per-bin noise floor (signals pop above baseline)

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

**Band-plan region** — Sets which region's band plan drives the coloured CW/Digi/Phone strip under the frequency axis. **Auto** derives it from your grid square; you can also force Region 1/2/3.

**Band Presets** — Add or remove custom bands. Standard bands (160–10 m) are always available.

## FT8 Settings

**FT8 On/Off** — Globally enable/disable FT8 mode.

**Field Day Mode** — ARRL Field Day mode (on/off, class, section).

**Simulation Mode** — Practice QSOs with phantom stations without keying a real QMX (red breathing border on screen).

**FT8 Filters** — Include/exclude stations, set auto-reply priority, enable robot mode (see [FT8 Receive](ft8-rx.md) for details).

**Keyboard** — M5Stack Tab5 snap-on keyboard support (if connected).

## Audio & DSP

**IQ Balance** — Adaptive I/Q phase correction (usually on). Suppresses mirror-image signals.

**CW Audio** — CW demodulation + speaker output. Currently shelved and hidden from the drawer pending a USB-audio pipeline fix.

## Diagnostic Logging

The diagnostic log is **always on** — there is nothing to enable. All firmware log output is captured to a 5 MB memory ring, with a rolling copy persisted to internal flash (survives a reboot or power-off) and, if a microSD card is inserted, mirrored continuously to the card. Download via:

- Web UI: **Diag ↓** (live session log) or **Diag(saved) ↓** (the flash-persisted copy from before the last reboot)
- microSD card: `/qmx-panadapter/qmx-log.txt`
- USB serial: `tools/capture_serial_log.ps1`

Useful for troubleshooting rare issues.

## microSD Auto-Archive

Insert a microSD card (FAT32 or exFAT, any size) and the Tab5 automatically mirrors three files to `/qmx-panadapter/` on the card:

| File | Contents |
|------|----------|
| `qmx-log.txt` | Diagnostic log, rolling (rotated at 5 MB) |
| `qso.adi` | ADIF QSO log, mirrored after each new entry |
| `qmx-config.txt` | All settings exported as INI text |

**No setup needed** — the Tab5 probes for a card on startup and whenever it can't reach one it expected. Insertion and removal are detected automatically.

A **green SD dot** in the bottom status bar confirms a card is mounted and being mirrored. If no card is inserted, the dot is absent (not an error).

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
