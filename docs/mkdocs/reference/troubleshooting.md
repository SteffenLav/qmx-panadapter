# Troubleshooting

## Common Issues

### Spectrum is flat, no signal

**Symptoms:** Top bar shows `Band: ---`, spectrum shows no activity.

**Causes:**

1. **Charge-only USB cable** — the #1 issue
2. QMX not powered on
3. QMX not responding to CAT (firmware too old)

**Fix:**

1. Try a **different USB cable** — use one you know works for data (USB stick, phone file transfer, etc.)
2. Power cycle the QMX (off 5 seconds, back on)
3. Power cycle the Tab5
4. Check QMX firmware version is v1.03.002 or newer (the v1.04.002 beta also works)

If still flat after 10 seconds, proceed to [Collecting Diagnostics](#collecting-diagnostics).

### Spectrum signal is shifted/mirrored, or slides across the whole window as you tune

**Symptoms:** The signal isn't where it should be — it appears shifted, and turning the QMX's own VFO knob slides it across the *entire* 48 kHz window instead of just nudging it. Audio is often silent until you tune the signal back into the visible range.

**Cause:** The QMX never confirmed IQ mode for this session. Without it, the radio streams plain (non-IQ) audio instead of a properly centred baseband, which produces exactly this symptom.

**Fix (v0.19.3+):**

1. The panadapter automatically retries the IQ-mode handshake up to 4 times at connect, so this usually resolves itself within a second of the QMX showing up — no action needed.
2. If it still happens, a **red banner appears across the top of the screen** telling you immediately. When you see it:
   - Power-cycle the QMX (forces a fresh handshake on reconnect), **or**
   - Check the QMX's own **System Config → IQ Mode** setting is enabled
3. On firmware older than v0.19.3, this failure was silent — only visible in the diagnostic log as `QMX IQ mode NOT confirmed`. Updating is the simplest fix.

### QMX loses CAT connection after 1–2 minutes

**Symptoms:** Frequency/mode/BW stop updating, FT8 can't transmit.

**Cause:** QMX firmware too old or CDC-ACM driver timeout.

**Fix:**

1. Verify QMX firmware is v1.03.002 or newer (see Step 1 in [Quick Start](../quick-start.md); the v1.04.002 beta also works)
2. Try a shorter/higher-quality USB cable
3. Restart both devices

### WiFi won't connect

**Symptoms:** WiFi modal shows "Connecting..." for 30+ seconds, then fails.

**Causes:**

1. Wrong SSID or password
2. WiFi network uses 5 GHz (Tab5 prefers 2.4 GHz, but 5 GHz works)
3. WiFi network blocks client-to-client traffic (if you're trying to access the web UI from the same network)
4. Tab5 WiFi module issue (rare)

**Fix:**

1. Double-check SSID spelling and password (copy-paste if possible)
2. Try your **2.4 GHz WiFi network** instead (if you have dual-band)
3. Forget the network and reconnect: Settings → WiFi → tap network name → Forget → re-add
4. Restart Tab5
5. If still failing, grab the always-on **diagnostic log** — see [Collecting Diagnostics](#collecting-diagnostics)

### WiFi connects, then stops working after a few minutes

**Symptoms:** WiFi (web UI, uploads) worked at first, then went dead after some minutes — while FT8 and radio control kept working normally. On older firmware the only fix was a reboot.

**This is fixed in v0.20.0.** The cause was a low-level lock-up in the link to the WiFi co-processor; the device now recovers from it automatically (dropping one packet, which is simply re-sent) instead of staying wedged. If you're on **v0.20.0 or later** and still see WiFi die permanently, it's a new issue — please capture the diagnostic log (below) and report it. If you're on an **earlier version**, update the firmware.

### FT8 decoding is slow or stops

**Symptoms:** Decode list isn't updating, or only 1–2 decodes per slot.

**Causes:**

1. WiFi interference (WiFi and USB host compete for DMA on the C6)
2. CPU overload (rare; usually other tasks eating CPU)
3. Audio USB disconnection (hidden; spectrum still updates, but FT8 decode silently stalls)

**Fix:**

1. **Turn WiFi off** temporarily and see if decodes improve
2. Restart the panadapter (go Panadapter → settings → About → Reset)
3. Check USB cable is firmly connected
4. Download the diagnostic log (web UI **Diag ↓**) after a session and post it on GitHub

### FT8 transmit doesn't key the QMX

**Symptoms:** You tap "Transmit", the modal closes, but nothing happens (no TX light on QMX, no tone on the air).

**Causes:**

1. QMX firmware doesn't support Kenwood `TX;` command (v1.03.002+ only)
2. CAT connection is dead (see [QMX loses CAT connection](#qmx-loses-cat-connection-after-12-minutes) above)
3. Tab5 is in simulation mode (deliberate safety feature)
4. QMX is stuck in a menu (rare)

**Fix:**

1. Check QMX firmware version (see Quick Start)
2. Try a manual CAT command via web API: `curl "http://<ip>/api/cat" -d '{"cmd": "TX;"}' -H "Content-Type: application/json"`
3. If that fails, you have a CAT issue (see above)
4. Check settings → FT8 → Simulation Mode is **off**
5. Restart both devices

### Battery drains very fast

**Symptoms:** Battery goes from 100% to 0% in under 1 hour with normal use.

**Causes:**

1. Display brightness at 100%
2. WiFi on (drains ~30% more than without WiFi)
3. FT8 decoding (uses more CPU than panadapter alone)
4. Battery is old or defective

**Fix:**

1. Lower display brightness: Settings → Display → Brightness → 50–70%
2. Turn off WiFi if you don't need the web UI: Settings → WiFi → off
3. FT8 decoding uses more power; this is expected
4. If battery still drains in <2 hours with low brightness + no WiFi, the battery may be failing

### Time is wrong, FT8 doesn't work

**Symptoms:** Bottom bar shows wrong time, FT8 decoding says "0" and then stops.

**Cause:** Time is off by >1 second (FT8 is time-critical).

**Fix:**

1. Check if WiFi is connected (should auto-sync time via SNTP)
2. Manually set time: Settings → Time Sync → Set Manual Time → enter UTC time
3. If no WiFi and no RTC set, the time will be wrong (see [Time Sync](../guide/time-sync.md))
4. Enable FT8-derived sync: Settings → Time Sync → Use FT8-Derived Sync (optional, helps if WiFi isn't available)

For POTA/portable operation without WiFi:
- Set the RTC **before you leave home** (Settings → Time Sync → Set Manual Time)
- The RTC holds time for 30–40 hours without power

### Web UI won't load

**Symptoms:** Browser says "Connection refused" or "Can't reach this page".

**Causes:**

1. Tab5 is not on WiFi
2. You're using the wrong IP address
3. WiFi network has client-isolation enabled
4. Web server crashed (rare)

**Fix:**

1. Check WiFi is **on** (settings drawer)
2. Check the **IP address** shown in settings (e.g., 192.168.1.50)
3. Use that IP in your browser: `http://192.168.1.50`
4. If still failing, try `http://192.168.1.50:80` explicitly
5. Restart the Tab5

### Settings disappear after restart

**Symptoms:** You set your callsign/grid/WiFi password, but after power cycle they're gone.

**Causes:**

1. **Not normal** — settings should persist to NVS (non-volatile storage)
2. Recent firmware update may have reset NVS
3. NVS is corrupted (very rare)

**Fix:**

1. Re-enter your settings
2. If they disappear again, download the diagnostic log (web UI **Diag ↓**) and report on GitHub
3. As a last resort: export your config via web UI (Config ↓), then do a full factory reset, then re-import

## Collecting Diagnostics

If you're stuck, capture a **diagnostic log**:

### From the Web UI (Wireless)

The diagnostic log is **always on** — nothing to enable.

1. Reproduce the issue (let it sit for 30 seconds)
2. In the web UI bottom bar, click **Diag ↓** to download the live session log — or **Diag(saved) ↓** for the copy persisted from before the last reboot (useful if the device crashed or was power-cycled)
3. Alternatively, if a microSD card is inserted, pull `/qmx-panadapter/qmx-log.txt` from the card
4. Post the `.txt` file on [GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) or the [QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643)

### From Serial Console (Offline)

If WiFi isn't working:

1. Plug Tab5 into your computer with a **USB-C data cable**
2. Run: `tools/capture_serial_log.ps1` (Windows) or equivalent
3. Let it capture for 30 seconds while you reproduce the issue
4. Post the log file on [GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) or the [QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643)

The log includes your firmware version, QMX firmware, callsign, grid, hardware revision, and all serial output from the system.

## Still Stuck?

1. **Check the [Quick Start](../quick-start.md)** — most issues are covered there
2. **Read [Settings](../guide/settings.md)** — verify your configuration
3. **Download the diagnostic log** (always on) — it often reveals the root cause
4. **Post on [GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) or the [QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643)** — include:
   - Your symptoms (what you saw, what you expected)
   - Your hardware (Tab5 model, QMX/QMX+, antenna)
   - Your firmware versions (read from the About screen)
   - Your diagnostic log (if applicable)

The maintainer (OZ1LAV) and other users are very responsive to well-documented issues.

---

**Next:** Explore [Build from Source](../build/build.md) if you want to modify the firmware.
