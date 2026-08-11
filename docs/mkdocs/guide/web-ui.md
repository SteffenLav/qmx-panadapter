# Web UI

Access the panadapter remotely from any browser on your WiFi network.

## Quick Start

1. Enable **WiFi** in the settings drawer
2. Open your browser to **`http://qmx.local`**
3. You'll see the same spectrum and waterfall as the Tab5

No installation, no configuration, and **no IP address to write down**.

> **Why a name and not an address.** The Tab5 picks its own network - it remembers
> up to six and moves to whichever is on the air - so its IP address changes
> without anyone deciding it should, and the only place it is shown is the Tab5's
> own bottom bar, which is no help when the Tab5 is in the shack and you are not.
> `qmx.local` does not change.
>
> If `qmx.local` does not resolve, the IP still works exactly as before - read it
> off the Tab5's bottom bar. Windows and macOS resolve `.local` names out of the
> box; a few Linux setups need `avahi-daemon` installed, and some guest/hotel
> networks block the multicast that makes it work.

## Remote Control

The web UI provides full remote control:

- **Frequency** — tap the VFO to open the numeric keypad (drag it by its title bar to reposition; toggle between a 10-key and a phone-style digit layout), or click the spectrum to tune
- **Mode** — buttons for USB, LSB, CW, DiGi
- **Bandwidth** — selectors for SSB filter width or CW passband
- **Band** — jump between configured bands
- **Memory** — recall, edit and clear your 32 saved channels (see [Memory channels](#memory-channels) below)
- **Zoom** — pinch or scroll to zoom the spectrum

Everything mirrors the Tab5 display with sub-second latency.

## Spectrum Waterfall

The waterfall is **live-streamed** from the Tab5 every ~100 ms. You see the same real-time signal activity as on the device itself.

Click anywhere on the spectrum to tune to that frequency. The waterfall updates continuously — no refresh needed.

**Whole-band plan strip.** Along the bottom of the view (above the status bar) is a colour-coded CW/Digi/Phone strip spanning the entire current band, mirroring the one on the Tab5. A draggable "visible window" marks the slice currently on screen — drag it, or tap anywhere on the strip, to retune — and a marker shows the VFO position.

**Adjustable split.** Drag the divider between the spectrum and the waterfall to give either one more room. Your chosen split is remembered in the browser.

> **In FT8 or FT4 mode the live stream pauses.** The browser stops streaming the spectrum/waterfall and shows a notice plus the log and upload controls instead. This is deliberate — while you're operating digital modes the stream would compete with the on-device decoder and the WiFi link, so pausing it keeps FT8 decoding and WiFi noticeably steadier (new in v0.20.0). Switch the Tab5 back to Panadapter mode and the stream resumes automatically.

## FT8 Control

When the Tab5 is in **FT8/FT4 mode**, the browser pauses the live spectrum stream (see [Spectrum Waterfall](#spectrum-waterfall) above) and instead shows a **live TX status banner**, a **Call CQ** button, and the **log and upload controls** — download your ADIF, upload to QRZ/eQSL/LoTW, grab the diagnostic log. Everything else about operating FT8 (watching the decode list, tapping a station to reply) happens **on the Tab5 itself**.

The status banner (new in v1.3.6) mirrors the Tab5's own TX label so you can watch the radio from another room: **red** while transmitting — including the "call 2 of 4" counter when a [CQ stop limit](ft8-tx.md) is set — **amber** when a transmission is armed or a QSO is waiting, **green** on QSO complete, **orange** on timeout, and the persistent **"CQ stopped after N calls - no answer"** once an auto-stopped CQ run ends. The browser tab's title also shows a red dot while transmitting, so even a background tab signals when the radio is on the air.

### Live spots on the browser spectrum

The **POTA** and **RBN** spots the Tab5 draws on its own
trace are now drawn on the browser's spectrum too, from the same store on the
device - so the two screens can never disagree about who is on the air.

- **Amber** is a POTA activation, **green** an RBN (CW skimmer) spot, and **grey**
  a station you have already worked *on this band*.
- Spots fade with age and are gone at 30 minutes, exactly as on the Tab5.
- **Click a callsign to tune to it** - frequency *and* mode (CW, DiGi, or USB/LSB
  by band). Bandwidth is deliberately left alone: the QMX keeps a filter per mode.
- Counts at the bottom corners - `< spots (3)` - say how many more are on this
  band just outside your window; click one to jump to the nearest.
- On a crowded band only so many callsigns fit; unworked and fresher spots keep
  their labels, and a spot that cannot fit one is not drawn at all rather than
  left as a line pointing at nothing.

Spots are sent only while the Tab5 is showing the panadapter, and only when they
have actually changed, so they cost the WiFi link almost nothing between refreshes.

### The decode list, from another room

In FT8/FT4 the browser used to show a transmit banner and
nothing else - you could see that your radio was transmitting, but not who was
answering. The FT8 panel now carries the **decode list**: callsign, message, grid,
SNR, DT, audio tone, slot (E/O) and age.

It is the **same list the Tab5 shows**, not a second opinion: the ordering comes
from the device (the station you are working first, then anything addressed to
you, then CQ calls, then strongest signal) and your Filter-window settings are
applied to it. The station being worked is highlighted, and anything carrying your
own callsign stands out, as on the Tab5.

**Click a station to work it** (at the operator's own
request). The confirm dialog states what will happen, and then the browser runs
the **same intelligent Transmit a Tab5 row-tap runs**: a fresh CQ starts the full
automatic QSO (you are not in front of the radio to click each exchange step);
anything mid-exchange sends the one correct next message. The outcome comes back
in plain words under the TX banner - "Armed: ...", "Busy: working X", or a refusal
if the station has aged out of the list, because transmitting at a station that
may have left is worse than asking you to click again.

**Who is calling you.** Below the table, the **pileup** - stations answering you
while you are busy - each clickable with the same confirm. And the **grey-list**
(stations the auto pickers skip after repeated no-answers) is shown with a
one-click clear, so "why is it ignoring that station?" is answerable from
another room.

### Switching the Tab5 back to the panadapter

If you left the Tab5 in FT8/FT4 and want the spectrum
back, **Show Panadapter** on the FT8 panel does it without walking to the radio.

### Call CQ from the browser

Asked for by Dennis WN4FLA. A CQ run that has timed out, or that has reached its [CQ stop limit](ft8-tx.md), otherwise needs a walk back to the Tab5 to start it again. The **Call CQ** button under the status banner does it from wherever you are.

- **It asks first.** The button **keys the radio**, and a mis-click from another room should not put a carrier on the air, so it confirms ("Start calling CQ on the Tab5?") before anything is sent.
- **It calls exactly what the Tab5 would.** The active CQ preset, the current TX tone (honouring **TX Hold**, or picking the nearest clear slot as usual) and the **TXCQ ANY / EVEN / ODD** parity are all the ones set on the device — the two buttons share one code path, so they cannot drift apart. To change any of those, long-press **Call CQ** on the Tab5.
- **It takes about a second.** The request is handed to the Tab5's own display task rather than acted on inside the web request, so the button greys out and reads "Calling..." briefly. Watch the status banner, not the button, to see the CQ start.
- **Only in FT8/FT4 mode.** The button is part of the FT8 panel, and a request that arrives when the Tab5 is not in FT8 is discarded rather than queued — so it can never fire minutes later, unasked, when you next switch modes.
- If your callsign and grid are not set, the Tab5 shows the reason on its own screen and nothing is transmitted.

> **Not yet confirmed on the air.** The endpoint, the hand-off, the preset/tone/parity reuse and the error path are all verified on hardware; the final key-down is inferred from sharing the Tab5 button's code. Please report how it behaves.

Apart from Call CQ, transmit is still initiated on the Tab5 — replies and pounces need the decode list in front of you, and only one interface should be keying the QMX.

## Choosing your TX tone

**TX tone** on the FT8 panel opens the same picker the
Tab5 has, against the same live occupancy of the 200-2800 Hz window - it is the
device's own map, the one its automatic clear-slot picker uses, so a slot shown
busy here is one the device would avoid anyway.

- **Green** is free, **red** a station or its guard band, **pink** the station you
  are working, **white** where you will transmit.
- **Click a slot** to choose it - a mouse can go straight there, where the Tab5
  needs a drag. **&minus;50 / +50** nudge, and **Find a clear slot** walks outward
  to the nearest free one, exactly as the automatic picker does.
- **TX Hold** keeps the exact tone you picked. With it off, each transmission
  takes the nearest clear slot, and a CQ that gets clashed moves itself.
- **Grey means unknown, not free.** If nothing has been decoded yet the device has
  no picture of the band, and the strip says so rather than showing reassuring
  green.

Changing the tone mid-QSO is fine - your partner tracks your time slot, not your
audio frequency - but it is refused **mid-burst**, and the reason is shown rather
than the change silently half-applying.

**Both time windows, always** (asked for by Roy KI0ER).
The strip is two rows - **EVEN above, ODD below** - because two stations only
collide if they transmit in the *same* window, and only you know which window you
are about to pounce into. The verdict says where your pick stands: *"Clear in
EVEN - busy in ODD"*, or judges just your own window once a transmission has
fixed it. The Tab5's own mini strip and picker show the same two rows.

## Memory channels

The **Memory** button opens your 32 channels as a grid.
**Tune** recalls one - frequency and mode together - and **Edit** changes or
clears it. An empty slot offers the frequency you are on now, so storing the
current VFO is two clicks.

A frequency outside an amateur band is **refused**, exactly as the Tab5 refuses
it: a channel you cannot legally tune is worse than no channel.

## Antenna Tune from the browser (QMX 1.04+)

With a QMX on firmware 1.04+, an **Antenna Tune** button
appears in the bottom bar. It **keys the radio with a steady carrier**, so it
confirms first - and while running, the button itself becomes the readout:
**live power and SWR**, click again to stop. Both ends carry a **60-second
safety stop**: the device's own timer fires even if the browser tab dies, and
the radio's prior mode is restored, never left keyed. Stopping Tune from the
QMX's own front panel is honoured too.

## Settings, with a real keyboard

The **Settings** button in the bottom bar edits the
things you *type* - which is exactly what the Tab5's touchscreen is worst at:

- **Your callsign and grid.** Previously reachable from a browser only by
  downloading the config file, editing it and uploading it back.
- **The three CQ messages**, which preset is active, and the CQ stop limit.
- **The FT8 include/exclude filter terms** (both pairs) and the filter toggles.
- **Everyday switches**: POTA and RBN spots, PSK Reporter, grey-listing, distance
  units, I/Q balance, band-plan region, and the QMX volume in dB.
- **WiFi**: add another network from a laptop. The Tab5 remembers up to six.

Saved straight to the Tab5 - its own settings drawer shows the same values next
time you open it.

Also here: the **Display & waterfall** group -
brightness, waterfall black level, contrast, adaptive floor, FFT window, colour
map, display sleep, the dB scale range and spectrum smoothing - the controls you
want to tune from a laptop *while watching the Tab5's screen*. Every change
applies live and is stored, exactly as the drawer's own sliders do. The
**auto-answer robot** switch is here too, carrying the same permanent warning the
Tab5 shows beside it: it transmits unattended - never leave it running
unsupervised.

One deliberate omission: **your WiFi password is never sent to the browser**, so
the field is always blank; leave it blank and the stored one is kept. And
Flip-180 stays on the Tab5 - you set that standing at the device, because it
depends on how it is mounted.

## Help, in the browser

The **Help ?** button in the bottom bar opens the same two
doors the Tab5 has, served **by the Tab5 itself** - so it works with no internet at
all, and the text always matches the firmware you are running.

- **A list of symptoms and questions in plain words** - "My radio is not showing
  up", "Nothing appears in the decode list", "How do I change what my CQ says?".
  Pick the one that fits and the manual opens at the section that answers it.
- **Rows the Tab5 can see are happening right now are highlighted** and moved to
  the top, exactly as on the device. It ranks; you choose.
- **The whole manual**, with a Contents list, Back, and links between chapters.

See [Getting Help](../getting-help.md) for what the device can detect and why it
never navigates for you.

## CAT Control (Advanced)

The **CAT** section lets you send raw Kenwood-style commands directly to the QMX:

```
FA;        → reads current frequency
FA14074000;  → sets frequency to 14.074 MHz
MD;        → reads current mode
MD2;       → sets USB mode
```

This is for advanced troubleshooting — most users don't need it.

## Bottom Bar Menus

The bottom bar groups its actions into four popup menus, plus a battery indicator (e.g. `🔋 87% (8.0V)`):

**QSO Logs (n) ▲** — only visible when QSOs exist; *n* is the QSO count:

- **ADIF download ↓** — QSO log as an ADIF file (import into WSJT-X, EQSL, etc.)
- **QRZ upload ↑** — upload ADIF to QRZ Logbook (requires API key on first use, saved for future sessions)
- **eQSL upload ↑** — upload ADIF to eQSL (requires username/password on first use, saved)
- **LoTW setup** / **LoTW ↑** — upload ADIF to ARRL's Logbook of The World (see [LoTW Upload](#lotw-upload) below)
- **View / edit log** — opens the QSO log right in the browser (call, mode, band, frequency, date/time, reports, grid — newest first). **Click any column header to sort** by it (click again to reverse) — sorting by Date groups an activation's QSOs together. Each row has a ✕ to delete that one record, and a **Delete all** button clears the whole log (no undo, so it asks you to type `DELETE` to confirm — download the ADIF first if you want a copy). Handy before a POTA activation: start with an empty log and the ADIF at the end is exactly the file you submit

**Files ▲**:

- **Config download ↓** — all settings as a text file (backup or transfer to another Tab5)
- **Config upload ↑** — restore settings from a backup file
- **SD Files** — opens the **microSD file browser** (`http://<tab5-ip>/files`, new in v1.3.0): browse the card from your computer without pulling it — download logs and config backups, upload files, delete
- **Diagnostic download ↓** — downloads **both** diagnostic logs: the live session log (always on, nothing to enable) and the flash-persisted copy from before the last reboot/power-off

**Radio ▲** (new in v1.6.0) — the things that reach the radio itself:

- **Switch to FT8/FT4** / **Switch to Panadapter** — changes the Tab5's own screen from the browser. The label always says where you are going, and it follows the Tab5 if you switch there instead
- **Memory channels** — your 32 channels: click one to tune to it, drag one onto another slot to move it, or onto the bin to clear it
- **Let me use the QMX menus** — stops the Tab5 talking to the QMX so you can use the radio's own menus or its Band Configuration terminal. A bar appears until you hand it back. See [Settings](settings.md#radio)
- **Antenna Tune** — QMX 1.04+ only; keys a steady carrier with live power and SWR, and stops itself after 60 seconds

**Miscellaneous ▲**:

- **Tab5 screenshot** — current display as PNG, including any open pop-up (band/mode dropdown), not just the base screen
- **Reset settings** — clear stored settings back to defaults (see [Troubleshooting](../reference/troubleshooting.md))
- **Reset WiFi** — clear just the WiFi/network state

## LoTW Upload

Uploading to ARRL's **Logbook of The World** requires an existing LoTW account and a callsign certificate (made with ARRL's TQSL program).

**One-time setup:** open the **QSO Logs** menu → **LoTW setup**. A guided two-page window walks you through it:

1. **Page 1** explains how to export your callsign certificate from the TQSL program on your PC (**Callsign Certificate → Save the Callsign Certificate**, which produces a `.p12` file), with a button that opens ARRL's own instructions.
2. **Page 2** imports the `.p12` file, its passphrase, and your DXCC entity (plus optional CQ/ITU zones, and **US state and county** — see below). The `.p12` is parsed **in the browser** — the passphrase never reaches the device.

After setup the button reads **LoTW ↑**. Each click signs all not-yet-uploaded QSOs on the device with your certificate and uploads them to lotw.arrl.org.

**US stations: fill in state and county** (new in v1.3.3). These were not being sent at all before, which meant US operators' uploaded QSOs earned **no Worked All States and no county credit** — for them or for the stations they worked. Fill them in if your TQSL station location has them. The county is the **name on its own** (`Arlington`), not `VA,Arlington`. Operators outside the US can leave both blank.

**Certificate renewal** — LoTW certificates expire roughly every 3 years. **Ctrl-click** the **LoTW ↑** button to re-run setup. From v1.3.3, re-submitting the *same* certificate no longer re-uploads your whole log — only an actually-changed certificate resets the upload position, because a new key means every QSO has to be re-signed. So you can go back in to add a state and county without resending everything.

**If an upload does not appear on LoTW**, check [ARRL's queue status](https://www.arrl.org/logbook-queue-status) before assuming a fault — the queue has run hours behind at busy times. LoTW rejects a malformed file at upload time, so anything that reached the queue was signed correctly.

## Upload Behaviour

Uploads work **while FT8 or FT4 is actively running** — the panadapter briefly pauses the FFT and SD-archive activity during the HTTPS transfer, then resumes automatically. A result is shown once the upload completes, reporting how many QSOs were sent.

Each upload remembers where it left off — re-uploading skips QSOs that were already sent in a previous session.

## Network Requirements

- **WiFi must be on** (settings drawer)
- **IP address shown** in settings (or static IP if you prefer)
- **Both Tab5 and browser on the same LAN** (no internet needed)
- **`qmx.local`** resolves on most networks; guest/hotel WiFi often blocks the multicast it needs, in which case use the IP
- **5 GHz WiFi works** but 2.4 GHz is recommended (longer range)

## Limitations

- **Transmit from web** — **Call CQ** only (see [Call CQ from the browser](#call-cq-from-the-browser)); replying to a station, pouncing and the TX tone/preset/parity settings are all on the Tab5
- **Real-time chat** — no operator messaging
- **Export formats** — ADIF only (import to EQSL, WSJT-X, etc. yourself)
- **Latency** — ~200 ms typical (WiFi dependent)
- **Stale connection recovery** — if the browser's network drops without a clean disconnect (e.g., putting a laptop to sleep), the web view may freeze briefly before recovering. Recovery is automatic and capped at ~5 seconds.

---

**Next:** Set up [Time Sync](time-sync.md) or explore [Settings](settings.md).
