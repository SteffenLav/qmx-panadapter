# Settings & Configuration

Swipe in from the right edge to open the settings drawer. All settings are saved automatically.

The top two buttons are not settings but the two ways into help: **User Manual**, which
opens this guide at the chapter for the screen you came from, and **Need guidance?**,
which lists symptoms and questions in plain words. See [Getting Help](../getting-help.md).

## Basic and Expert

The drawer is grouped under headings — **Station**, **Device**, **Radio**, **Network**,
**Display**, **FT8** and **Spectrum** — and the toggle beside the **Settings** title
chooses how much of it you see. It always says where you are and what a tap gives you:
tapping **BASIC (tap for Expert)** reveals the Spectrum and Device groups, which hold the
calibration and tuning controls you set once and rarely touch again.

Nothing is lost in Basic; it is only hidden. The choice is not remembered between
sessions, because it is a way of looking at the drawer rather than a preference.

## Radio

These reach the QMX itself over the CAT link.

**QMX volume** — the radio's own AF gain, in decibels, the same number the QMX shows on
its LCD when you click its volume knob. Read back from the radio when you open the
drawer, so it agrees with the rig even if you last changed it there.

**QMX RF gain** — the per-band RF gain from the radio's Band Configuration, 0–99 dB
(the QMX default is 54). Because it is **per band**, the Tab5 reads it from the radio
rather than remembering a number that would belong to whichever band you were on last;
it shows "reading…" until the radio answers. It writes when you let go of the slider, not
while you drag, because this is stored configuration rather than a session setting.
Changing it moves the noise floor, so the flat-spectrum reference is re-learned.

**CW transmit offset** — transmit a little away from the station you are listening to,
so a QRP call is not buried in the pile of everyone zero-beating the DX (suggested by
Roy KI0ER). The centre of the slider is off, and the sign chooses whether you transmit
above or below.

**Keep it small: around 100 Hz or less is typical, and the slider stops at ±300.** The
point is to land inside the other station's filter, not outside it — most CW operators
run a 500 Hz filter or narrower, 200 Hz is not unusual, and the passband is not equally
usable right to its edge: the tone gets muddy and a QRP signal has least energy exactly
where the filter is already attenuating (Michael KZ4LY). Roy KI0ER uses +60 Hz on his own
rig. Earlier versions of this page suggested 400–600 Hz, which is outside many operators'
filters altogether.

The QMX has no XIT, so this is done with **split**: you receive on VFO A and transmit on
VFO B, which the Tab5 holds at your receive frequency plus the offset. Set it once and it
follows you — a tap on the panadapter, a spot, a memory recall, a band change, the web
page, and the radio's own tuning knob. It applies in **CW and CW-R only**, and leaving CW
clears it. If you are running your own split, the Tab5 leaves it alone.

**Let me use the QMX menus** — hands the radio back. The QMX's own menu, and its Band
Configuration terminal application, talk over the same USB serial port the Tab5 polls
several times a second, so the two fight over it. Tap this before using the radio's own
menus: the Tab5 stops sending anything, the spectrum freezes, and a blue bar across the
top says so and gives the radio back when you tap it. Resuming re-checks IQ mode, which a
trip through the radio's menu can switch off.

While the radio is released the Tab5 will not transmit — an armed FT8 burst and Antenna
Tune are both refused rather than keying a radio you are holding.

## Operator Info

**Callsign** — Your amateur radio callsign (required for FT8/FT4 logging).

**Grid Square** — Your Maidenhead grid square (e.g., JO45; required for FT8/FT4 exchanges).

## WiFi

Tap **WiFi setup** in the settings drawer to open the WiFi window.

**WiFi On/Off** — the WiFi icon button in the WiFi window (shown with a diagonal red slash when off). Toggling it takes effect immediately — off disconnects and stays off; on reconnects right away. Turning WiFi off is useful for field operation (no WiFi overhead, extended battery life).

**SSID** — Your WiFi network name. Tap **Scan** to list nearby networks and pick one (as of v1.3.6 this also works when your stored network is out of range — hotels, POTA sites — where it previously always reported "No networks found").

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

The slider runs 0–50 dB rather than the QMX's full 0–199 dB protocol range, so the useful part of the travel is not crammed into a couple of centimetres. 50 dB is Randy N4OPI's figure from a real radio with the antenna connected — an earlier "not loud enough" reading turned out to have been taken with the antenna off, where the missing loudness was band noise, not gain. If you turn the radio past 50 dB with its own knob the slider knob sits at the end of its travel, but the number still shows the radio's true dB — it has to agree with the LCD.

Two things worth knowing, both from Randy N4OPI's side-by-side check against a real QMX (the numbers do read identically): the QMX only shows the figure on its own LCD when you give its knob a click, and the Tab5 slider reads the radio when the drawer *opens* — so if you turn the rig's knob while the drawer is on screen, the slider catches up the next time you open it rather than tracking live.

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

## Bluetooth mouse

A Bluetooth mouse is the one pointer that works **while the QMX is plugged in**.
A USB mouse cannot: the radio occupies the Tab5's only USB host, and sharing it
through a hub does not work on this hardware — both devices end up disabled. A
Bluetooth mouse never touches that port, so the radio keeps its connection.

For anyone operating with cold or unsteady hands, that is the point: every menu,
button and drawer control becomes a click instead of a precise tap on glass.

**Setting it up**

1. Tick **Bluetooth mouse → Enable** in the settings drawer.
2. **Restart the Tab5.** Bluetooth can only start once the radio link to the
   wireless co-processor is up, so the switch takes effect on the next boot —
   the toast says so at the time.
3. Put the mouse into pairing mode and wait a few seconds. It connects on its own.

You only do this once. The pairing is stored, survives a reboot and a firmware
update, and the mouse reconnects by itself from then on.

**The Bluetooth symbol in the bottom bar** sits just left of the WiFi indicator
and has three states:

| Symbol | Meaning |
|--------|---------|
| Dim grey | Bluetooth is switched off |
| Pale | On and looking for a mouse |
| **Blue** | A mouse is connected |

**What works:** moving the pointer, left click, and the scroll wheel — the wheel
scrolls whatever is under the pointer, so the decode list, the settings drawer
and the manual all scroll.

!!! note "The pointer disappears when the mouse sleeps"
    A Bluetooth mouse switches itself off after about half a minute of not being
    moved, to save its battery. The pointer vanishes with it and comes back the
    instant you touch the mouse — reconnecting takes under a third of a second.
    This is the mouse looking after its own battery, not a fault.

---

## Live Spots

Draws other stations on the spectrum at the frequency they are operating on. Full
details, including what the colours mean, in [Live Spots](spots.md).

**Live spots (POTA)** — On by default. Fetches Parks On The Air activations about
once a minute and draws them over the spectrum. Needs WiFi. Switching it off
leaves the spectrum completely clean.

**RBN spots (CW skimmers)** — **Off** by default, and deliberately so: RBN is a
continuous global feed over a persistent connection, unlike POTA's occasional
fetch. Adds stations the skimmer network is hearing right now, filtered to the
band you are on. **Requires your callsign** to be set (the feed asks for one on
connect — it identifies you, it is not a password).

**DX cluster spots (phone)** — **Off** by default, same reasoning: a second
persistent connection. This is the source that carries **SSB**. Skimmers are
machines, and no machine recognises a callsign spoken into a microphone, so
phone activity is structurally invisible to RBN however long you leave it on. A
cluster is people typing, so it carries voice contacts, and park and summit
references written into the comment. Mode is worked out from the spotter's
comment, or from your band plan when the comment does not say. Relayed skimmer
spots are dropped, so the cluster cannot double what RBN is already showing.

Whatever you switch on, **one station is one entry**: the same callsign within
2 kHz is merged, an activation spot outranks a plain sighting, and a dropped
duplicate folds back in as corroboration — drawn brighter, meaning a receiver
copied them just now rather than someone having typed it an hour ago.

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

## Activation (POTA / SOTA)

When you are activating a park or a summit, every contact you make has to be
logged as belonging to **that reference** — it is what POTA and SOTA read to
credit the activation, for you and for the people who worked you.

Open **Activation (POTA/SOTA)** in the settings drawer, pick the scheme, type the
reference, and tap **Start activation**. From then on every logged QSO carries it
automatically. There is nothing to remember per contact.

The screen shows your **contact count against the threshold** — ten for POTA,
four for SOTA — and turns green when you reach it. That number is read from the
log itself, so it survives a reboot and can never disagree with what was actually
written.

**Stopping matters as much as starting.** While a session is running the main
button is a red **Stop activation**, and the drawer button itself shows the live
reference rather than a generic label. The failure that actually happens is
driving home with it still switched on, quietly stamping every later contact with
a park you have left.

The session persists across a restart on purpose — a battery change in the field
should not lose it — but it is deliberately **not** included in a config backup,
so restoring an old backup can never re-activate a park you are nowhere near.

**Chasing** works without any setup: if you work a station the panadapter has
seen spotted at a park or summit, that reference is written into your log entry
automatically.

**Uploading just one activation:** the web UI's ADIF download can be limited to a
single reference, so you get that park's log rather than your entire file.

---

## SWR protection

While transmitting, the QMX reports its SWR back over the control link. If the
reading reaches the limit you set here the transmitter is **latched off** —
nothing will transmit again until you clear it by tapping the red warning on the
FT8 screen.

Choose **Off, 2.0:1, 2.5:1, 3.0:1 or 4.0:1**. The default is **3.0:1**.

**In FT8 the burst is cut short**; the SWR is sampled part-way through and the
transmission stops there. **In FT4 the check happens after the burst**, so the
one burst completes and every later one is blocked. That is not an oversight:
FT4 symbols are 48 ms and a control-link query takes about 50 ms, so asking
mid-burst would disturb the transmission it is trying to protect. A reading is
only acted on if there is real power behind it — the radio can report a
meaningless ratio when the amplifier is not actually loaded. That is high enough not to trip on a merely mediocre
match, and low enough to stop a burst going into a disconnected antenna, a
wrong-band antenna or a damaged feedline — which is how this usually goes wrong
in the field. An FT8 transmission is nearly thirteen seconds of continuous
key-down, so there is real time to save.

The latch is deliberately sticky. A bad antenna does not repair itself, and
resuming automatically would simply transmit into the same fault on the next
slot.

!!! note "Set it to Off if you use a tuner"
    With a matched antenna the protection never fires, but if you are
    deliberately loading something unusual you may prefer no interference at
    all.

---

## Propagation feedback — who is hearing me

This asks PSK Reporter which receivers have copied **your** callsign recently.
It is the reverse of the reports the panadapter *sends*, and the two are
separate settings — you can have either without the other.

It answers a question no amount of local processing can: **am I actually getting
out?** The valuable case is the mismatch. Stations you can hear that cannot hear
you is a transmit-side problem — antenna, power, a bad connector — and from the
receiving side alone it looks exactly like a dead band.

**This one lives in the browser, not on the Tab5.** There is no drawer control
for it — the answer is a list of stations with distances and bearings, which
wants a screen you are already sitting in front of. In the web UI open
**Settings → Spots & reporting** and tick **Propagation feedback (who is hearing
me)**, then **Miscellaneous → Who is hearing me** for the list: receiver,
country, distance, bearing and the signal report they gave you, sorted by
distance.

It is read-only and asks at most once every five minutes, which is the rate PSK
Reporter requests.

!!! note "An empty list is the normal answer if you have not transmitted"
    Nobody can report hearing you until you have been on the air. Give it a few
    minutes after a CQ run.

---

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

**Delete all** — the red-bordered button at the bottom-left erases the **whole** log. Two-tap confirm: the first tap arms it (the label changes to "ALL *N*?"), a second tap within 5 seconds deletes; wait and it disarms itself. There is no undo — download the ADIF from the web UI first if you want a copy. Handy before a POTA activation: start with an empty log and the ADIF at the end is exactly the file you submit.

Use the web UI to download the full ADIF file for import into WSJT-X, EQSL, or any other logging software — or view and edit it in the browser (**QSO Logs → View / edit log**).

**Exclude Worked Before** — When FT8 filtering, skip stations you've already logged QSOs with (requires you to import your own prior ADIF log first).

## Config Import/Export

**Config Download** — Export all settings + memory channels + ADIF log as a text file (INI format).

**Config Upload** — Restore settings from a backup file (settings only; memory channels merge).

Use this to:
- Back up your settings before a factory reset
- Transfer settings to another Tab5
- Recover from accidental changes

## Advanced / Expert

**These two live in the browser, not in the drawer** — a reset you can trigger by
mistake on a touchscreen in the field is a worse idea than one that needs a
computer. Both are in the web UI's **Miscellaneous ▲** menu.

**Reset settings** — Erases the stored settings and memory channels, returning
everything to defaults, and reboots. Use if something is stuck in a state you
cannot get out of. Take a **Config ↓** backup first and you can restore it in
seconds.

**Reset WiFi** — Erases the WiFi credentials and the stored radio/link state.
This is the one for a WiFi setup that will not come up no matter what you enter.

**Neither of them touches your QSO log.** The ADIF log lives in a separate area
of flash that is deliberately left alone, so a reset cannot cost you contacts —
and neither can a firmware update, which is why a normal flash keeps your log.
The only things that erase the log are the **Delete all** button in the ADIF
viewer, the same in the web log viewer, and a **clean/erase flash** from the
flasher.

---

**Next:** Troubleshoot an issue via [Troubleshooting](../reference/troubleshooting.md) or explore the [API](../reference/web-api.md).
