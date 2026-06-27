# Settings & Configuration

Swipe ← from the right edge to open the settings drawer. All settings are saved automatically.

## Operator Info

**Callsign** — Your amateur radio callsign (required for FT8 logging).

**Grid Square** — Your Maidenhead grid square (e.g., JO45; required for FT8 exchanges).

## WiFi

**WiFi On/Off** — Enable or disable WiFi. Useful for field operation (no WiFi overhead, extended battery life).

**SSID** — Your WiFi network name. Tap **Scan** to list nearby networks and pick one.

**Password** — Your WiFi password. Tap the eye icon to show/hide it.

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

**Snap to Peak** — Auto-tune to the strongest signal when you tap the spectrum (on by default).

**Distance in Miles** — Show FT8 distances in miles instead of km (off by default).

**Band Presets** — Add or remove custom bands. Standard bands (160–10 m) are always available.

## FT8 Settings

**FT8 On/Off** — Globally enable/disable FT8 mode.

**Field Day Mode** — ARRL Field Day mode (on/off, class, section).

**Simulation Mode** — Practice QSOs with phantom stations without keying a real QMX (red breathing border on screen).

**FT8 Filters** — Include/exclude stations, set auto-reply priority, enable robot mode (see [FT8 Receive](ft8-rx.md) for details).

**Keyboard** — M5Stack Tab5 snap-on keyboard support (if connected).

## Audio & DSP

**IQ Balance** — Adaptive I/Q phase correction (usually on). Reduces image rejection.

**CW Audio** — CW demodulation + speaker output (currently shelved; off by default).

## Diagnostic Logging

**Diagnostic Log** — Enable to capture **all** ESP_LOG output to a 512 KB ring buffer. Download via:
- Web UI: **Diag log ↓** button
- Settings: **Download Diagnostic Log**
- USB serial: `tools/capture_serial_log.ps1`

Useful for troubleshooting rare issues.

## ADIF & Logging

**ADIF Log** — View, edit, or clear the QSO log.

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
