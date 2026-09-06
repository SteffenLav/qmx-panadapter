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

## On a phone

The layout is built for a landscape screen and reflows fine there. In
**portrait** on a phone (v1.9.4), the top and bottom bars now scroll
sideways if a control doesn't fit — swipe them like the decode list — and
the page itself scrolls vertically if the whole layout is taller than the
screen. Before this a portrait phone (Randy N4OPI, iPhone Safari) could
lose the top and bottom bar controls entirely with no way to reach them.

## Remote Control

The web UI provides full remote control:

- **Frequency** — tap the VFO to open the numeric keypad (drag it by its title bar to reposition; toggle between a 10-key and a phone-style digit layout), or click the spectrum to tune
- **Mode** — buttons for USB, LSB, CW, DiGi
- **Bandwidth** — selectors for SSB filter width or CW passband
- **Band** — jump between configured bands
- **Memory** — recall, edit and clear your 32 saved channels (see [Memory channels](#memory-channels) below)
- **Zoom** — pinch or scroll to zoom the spectrum
- **RIT** — click the **RIT** pill to arm it, then click a caller to receive off your
  transmit frequency without moving the dial. See
  [RIT](panadapter.md#rit-receiving-off-your-transmit-frequency)

Everything mirrors the Tab5 display with sub-second latency.

## Spectrum Waterfall

The waterfall is **live-streamed** from the Tab5 every ~100 ms. You see the same real-time signal activity as on the device itself.

Click anywhere on the spectrum to tune to that frequency. The waterfall updates continuously — no refresh needed.

**Whole-band plan strip.** Along the bottom of the view (above the status bar) is a colour-coded CW/Digi/Phone strip spanning the entire current band, mirroring the one on the Tab5. A draggable "visible window" marks the slice currently on screen — drag it, or tap anywhere on the strip, to retune — and a marker shows the VFO position.

**Adjustable split.** Drag the divider between the spectrum and the waterfall to give either one more room. Your chosen split is remembered in the browser.

> **In FT8 or FT4 mode the live stream pauses.** The browser stops streaming the spectrum/waterfall and shows a notice plus the log and upload controls instead. This is deliberate — while you're operating digital modes the stream would compete with the on-device decoder and the WiFi link, so pausing it keeps FT8 decoding and WiFi noticeably steadier (new in v0.20.0). Switch the Tab5 back to Panadapter mode and the stream resumes automatically.

## FT8 Control

When the Tab5 is in **FT8/FT4 mode**, the browser pauses the live spectrum stream (see [Spectrum Waterfall](#spectrum-waterfall) above) and instead shows a **live TX status banner**, a **Call CQ** button, and the **log and upload controls** — download your ADIF, upload to QRZ/eQSL/LoTW, grab the diagnostic log. Everything else about operating FT8 (watching the decode list, tapping a station to reply) happens **on the Tab5 itself**.

**Band presets.** A dropdown beside the TX tone lists the standard FT8 or FT4 calling frequencies — the same list the Tab5's own **Preset** button offers, and it switches sub-mode and frequency together, so picking an FT4 frequency puts the Tab5 into FT4. The list follows whichever mode the Tab5 is in.

The status banner (new in v1.3.6) mirrors the Tab5's own TX label so you can watch the radio from another room: **red** while transmitting — including the "call 2 of 4" counter when a [CQ stop limit](ft8-tx.md) is set — **amber** when a transmission is armed or a QSO is waiting, **green** on QSO complete, **orange** on timeout, and the persistent **"CQ stopped after N calls - no answer"** once an auto-stopped CQ run ends. The browser tab's title also shows a red dot while transmitting, so even a background tab signals when the radio is on the air.

**A finished QSO waits for you.** When an exchange completes, the browser shows
`<callsign>  QSO complete` in green and **leaves it there** until you do
something else — every other message in that line clears itself after twenty
seconds, this one does not. The radio does not wait: it returns to idle and
carries on. The message is a record that the contact finished, kept for whoever
walks back into the room *(Randy N4OPI)*. The Tab5's own label is unchanged; it
is only the browser that has to cope with nobody watching.

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
answering. The FT8 panel now carries the **decode list**: callsign, message, **country**,
SNR, DT, audio tone, **distance and bearing**, slot (E/O) and age.

The country column replaced a GRID column, because the grid was already sitting
in the message text next to it. The country is the one thing in that row you
cannot read off the message, and unlike the Tab5 — where it has to squeeze into
three letters — the browser has room to spell it out. Both screens work it out
the same way from the callsign, so they cannot disagree. *(Randy N4OPI)*

The **KM** and **BRG** columns arrived in v1.8.3 (asked for by Tony Abbey). They sit
after the tone, in the same order the Tab5 uses, and the header reads **MI** instead
of KM if you have *Distance in miles* ticked. The Tab5 works the distance out and
sends it, rather than the browser calculating its own, so the two screens cannot
disagree — and where a station has not sent a grid the column shows a dash rather
than a made-up number.

It is the **same list the Tab5 shows**, not a second opinion: the ordering comes
from the device (the station you are working first, then anything addressed to
you, then CQ calls, then strongest signal) and your Options-window settings are
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
back, use the **Radio** menu in the bottom bar — it carries the
Panadapter ↔ FT8 switch.

Next to **Call CQ** is **TXCQ ANY / EVEN / ODD**, which chooses the 15-second
slot your CQ transmits in — the same three states as the button on the Tab5,
and the same setting underneath, so changing it on either surface is reflected
on the other. ANY takes whichever window comes first; EVEN and ODD lock it.

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
- **The occupancy picture refreshes itself every 3 seconds while the picker is
  open** (v1.9.4) — it used to be read once, when you opened it, so a slow
  decision (reading the strip, weighing E vs O) could cross a 15-second slot
  boundary and land you on a slot that filled in while you were choosing. Your
  own in-progress pick is never disturbed by the refresh, only the busy/clear
  colouring under it.

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
- **Show RIT button**, matching the Tab5 setting — hide the RIT button on the panadapter
  if you never use it.
- **The rest of the radio and display settings**, which used to be Tab5-only: CW pitch,
  IF calibration, SWR protection, the battery charge limit, the 180° screen flip,
  Fox/Hound mode, simulation mode and the spot mode filter.

Saved straight to the Tab5 - its own settings drawer shows the same values next
time you open it.

Also here: the **Display & waterfall** group -
brightness, waterfall black level, contrast, FFT window, colour map, display
sleep, the 180° flip, the dB scale range, spectrum smoothing and **spur
suppression** - the controls you
want to tune from a laptop *while watching the Tab5's screen*. Every change
applies live and is stored, exactly as the drawer's own sliders do, and the
browser's own spectrum and waterfall follow them as well. **Adaptive floor is
gone from both the browser and the Tab5 drawer** as of v1.8.3 — it could not
change anything (see
[Spectrum & Waterfall](panadapter.md#4-spectrum-waterfall)), and a control that
cannot change anything is worse than a missing one. The
**auto-answer robot** switch is here too, carrying the same permanent warning the
Tab5 shows beside it: it transmits unattended - never leave it running
unsupervised.

One deliberate omission: **your WiFi password is never sent to the browser**, so
the field is always blank; leave it blank and the stored one is kept.

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
- **Today only, dated file ↓** — just today's contacts, named `qso-YYYY-MM-DD.adi`. For anyone who files each day's log separately, the date is already in the filename rather than something to add by hand afterwards *(Gyula HA3HZ)*
- **ADIF restore ↑** — merges a previously downloaded (or any logger's) ADIF file back into the log, skipping any contact already there (matched on callsign, date and time). For after an erase-and-reinstall: **Config upload** never touches the QSO log on purpose, so this is the only way to get worked-station history back *(Randy N4OPI)*. A prompt lets you say whether the restored contacts should be marked as already uploaded to QRZ/eQSL/LoTW (the usual answer is yes, since "restore" almost always means a log that was already sent) or as not-yet-uploaded, so the next upload sends them
- **↳ Restore from SD card** — the same merge, but read straight off the microSD card the Tab5 already backs up to, with no file to find and nothing to choose. Contacts already logged are skipped, so pressing it twice does nothing. The card mirrors the *present*, so a deletion that survives a restart is on the card too — which is why the copy from just before the log last got smaller is kept beside it as `qso.prev.adi` (download it from **Files ▲ → SD Files** and feed it to **ADIF restore ↑** if you need it) *(Gyula HA3HZ)*
- **QRZ upload ↑** — upload ADIF to QRZ Logbook (requires API key on first use, saved for future sessions)
- **↳ Change QRZ API key** — appears once a key is stored, and replaces it. New in v1.8.3: before that the prompt only ever appeared when *nothing* was stored, so a key typed wrongly or later reissued could not be changed from the page at all (reported by Brian WA6JFK)
- **eQSL upload ↑** — upload ADIF to eQSL (requires username/password on first use, saved)
- **↳ Change eQSL login** — same, for the eQSL username and password
- **LoTW setup** / **LoTW ↑** — upload ADIF to ARRL's Logbook of The World (see [LoTW Upload](#lotw-upload) below)
- **Cloudlog upload ↑** — upload to your own Cloudlog or Wavelog. Asks for the server address, your API key and the station profile ID on first use. Plain `http://` is accepted for a server on the same network as the Tab5, given as a numeric address such as `http://192.168.1.20`; anything else needs `https://`. The check is repeated at every upload, so away from home the upload refuses instead of sending your key across someone else's network. If the server is on your own LAN this is the only upload that needs no internet at all *(Mark G4MEM)*
- **↳ Change Cloudlog server** — appears once a server is stored, and replaces the address, key and station profile
- **Check log** — looks through the log for records that are missing something a programme will want: a callsign, a date that is not in the right form, no station callsign, a reference that does not look like a reference, or — if you say you were activating — no reference of your own. It asks which of those two you mean when you press it. It can only tell you a record is not *obviously* incomplete: the Tab5 cannot see POTA's or SOTA's rules or their databases, so it never tells you a file will be accepted. Born from *"I wish POTA and SOTA had a feature allowing you to test your file for syntax and completeness without submitting it for credit"* — two trips to a park to get contacts to test with *(Don Adams WB0LQW)*. The Tab5's own Activation panel shows the same count under the contact total, so you can read it without a laptop.
- **View / edit log** — opens the QSO log right in the browser (call, mode, band, frequency, date/time, reports, grid — newest first). **Click any column header to sort** by it (click again to reverse) — sorting by Date groups an activation's QSOs together. **Search** filters the list as you type, matching any part of a callsign, band, mode, date, grid or reference; several words must all match, so `ea 20m` narrows to Spanish contacts on 20 m. A search that finds nothing says so — *"so this one has not been worked"* — which is the question worth asking of a log *(Gyula HA3HZ)*. **Tick the box on any row** and **Export selected** saves just those contacts as an ADIF file on your computer; the box in the header ticks everything **currently shown**, so a search and one tick gives you a single day, band or park as its own file. The export carries each record exactly as the Tab5 wrote it, so fields the table does not display are preserved. Each row has a ✕ to delete that one record, and a **Delete all** button clears the whole log (no undo, so it asks you to type `DELETE` to confirm — download the ADIF first if you want a copy). Handy before a POTA activation: start with an empty log and the ADIF at the end is exactly the file you submit. **Four columns are editable** — the two reports, **Grid** and **Their ref**. Click one and type the corrected value, or leave it empty (for a report, that records that none was exchanged). **Their ref** is the park or summit the *other* station was activating: while you are operating, that reference is on the POTA spots page on your phone and in nothing the radio sends you, so you write it down and enter it when you get home *(Don Adams WB0LQW)*. It fills itself in when the contact came from a spot, whether or not you were activating yourself — that is what the ADIF `SIG`/`SIG_INFO` fields mean, and a hunter's log should say which park they worked. It is only *Park-to-Park* when you were in a park too, which is why the column is not called that. **An empty cell is not a fault**: the reference comes from the POTA spot feed matched on the exact callsign, and not everyone activating in a digital mode registers the activation — some simply start calling `CQ POTA` — so an unregistered activator produces no reference at all rather than a wrong one *(Don Adams WB0LQW)*. Type it in by hand if you have it. Type the reference alone — `US-1241`, `G/LD-049`, `DLFF-0123` — and the Tab5 works out the programme from its shape and writes both `SIG` and `SIG_INFO`; clear the reference and both go with it. **Grid** is the other station's locator, correctable because it is the field most likely to be wrong through no fault of yours — a partner's grid arrives once, in their first message, and a marginal contact can complete without it ever being heard cleanly *(Gyula HA3HZ, who was correcting them in ADIFMaster)*. It is checked as a real Maidenhead locator, four or six characters, and refused otherwise: it drives distance and bearing on both screens and goes to three logbooks, so a typo would be a wrong measurement presented as a real one. Clearing it removes the field, which is the honest state when no grid was ever exchanged. Nothing else can be changed: callsign, band, mode, date and time are what QRZ, eQSL and LoTW match a contact on, so those stay read-only and a wrong one is a delete-and-re-log. If the QSO has already been uploaded, an edit corrects the Tab5's log only — the copy the logbook holds is unchanged *(Gyula HA3HZ)*

**Files ▲**:

- **Config download ↓** — all settings as a text file (backup or transfer to another Tab5). It is also the way to change a stored credential by hand: the `qrz_key`, `eqsl_user` and `eqsl_pass` lines are in there in plain text, and an edited file uploaded back takes effect on the next upload with no restart. Note it holds your WiFi password in plain text too, so treat the file accordingly
- **Config upload ↑** — restore settings from a backup file
- **SD Files** — opens the **microSD file browser** (`http://<tab5-ip>/files`, new in v1.3.0): browse the card from your computer without pulling it — download logs and config backups, upload files, delete
- **Diagnostic download ↓** — downloads **both** diagnostic logs: the live session log (always on, nothing to enable) and the flash-persisted copy from before the last reboot/power-off

**Miscellaneous ▲** also carries **Prepare for flashing** — closes the USB host cleanly before you unplug the radio or reflash the Tab5, so the QMX is not left half-open.

**Radio ▲** (new in v1.6.0) — the things that reach the radio itself:

- **Switch to FT8/FT4** / **Switch to Panadapter** — changes the Tab5's own screen from the browser. The label always says where you are going, and it follows the Tab5 if you switch there instead
- **Memory channels** — your 32 channels: click one to tune to it, drag one onto another slot to move it, or onto the bin to clear it
- **Let me use the QMX menus** — stops the Tab5 talking to the QMX so you can use the radio's own menus or its Band Configuration terminal. A bar appears until you hand it back. See [Settings](settings.md#radio)
- **Antenna Tune** — QMX 1.04+ only; keys a steady carrier with live power and SWR, and stops itself after 60 seconds
- **Activation** — start or stop a POTA/SOTA session from the browser. While one is running a badge appears in the top bar with the reference and the contact count, so it cannot be forgotten on the drive home
- **Radio menus** — the QMX's own 80×24 menu system, in the browser. Arrow keys, Enter and Back on screen, and your real keyboard works too. Needs the radio's second USB serial port switched on first. See [Radio Menus](radio-menus.md)

**Miscellaneous ▲**:

- **Tab5 screenshot** — current display as PNG, including any open pop-up (band/mode dropdown), not just the base screen
- **Power-cycle relay** (new in v1.10.9) — pulses one of two Tab5 GPIO pins (GPIO53 or GPIO54) for a chosen level and duration. Wire an external relay's trigger input to the pin and its contacts to your QMX's PWR_ON/GND **signals**, and this lets you power-cycle the radio remotely — the piece a remote firmware upgrade otherwise needs someone at the bench for, since the QMX always needs a manual power cycle after a Tab5 flash.

    !!! warning "Experimenter feature — the connector does not exist yet"
        **The QMX does not have a PWR_ON/GND jack.** Those signals have to be
        extended from the main board or the front panel out to a connector you
        add yourself. Do not go looking for an existing socket, and do not
        connect them to some other connector that looks plausible *(Randy
        N4OPI)*.

        Both pins are held as **driven outputs** from boot, resting LOW — they
        never float. A contact closure has to be a deliberate pulse, so a
        relay wired to them cannot be closed by the Tab5 booting, crashing or
        being reflashed.
- **Keyboard shortcuts** — assign what the Tab5's snap-on keyboard does (see below)
- **Reset settings** — clear stored settings back to defaults (see [Troubleshooting](../reference/troubleshooting.md))
- **Reset WiFi** — clear just the WiFi/network state

### Keyboard shortcuts

**Miscellaneous ▸ Keyboard shortcuts** assigns what the Tab5's snap-on keyboard
does. Each row is a modifier, a key and an action; **Add shortcut** makes a new
one, **Remove** deletes it, and **Restore defaults** puts back the nine the
firmware ships with.

There are **25 actions** — opening any of the setup windows, the manual, the log,
the radio menus, plus zoom in and out, brighter and dimmer, flat spectrum, and
releasing the radio to its own front panel. **Alt is left completely unused** by
the defaults, so it is free for your own bindings.

The editor is here rather than on the Tab5 for a simple reason: assigning
shortcuts is a setup job done once, and doing it on the Tab5 would mean typing
on the very keyboard you are configuring.

!!! note "Nothing that transmits can be bound"
    A button you deliberately press is one thing; a two-key combination that a
    slipped finger can produce is another.

## LoTW Upload

Uploading to ARRL's **Logbook of The World** requires an existing LoTW account and a callsign certificate (made with ARRL's TQSL program).

**One-time setup:** open the **QSO Logs** menu → **LoTW setup**. A guided two-page window walks you through it:

1. **Page 1** explains how to export your callsign certificate from the TQSL program on your PC (**Callsign Certificate → Save the Callsign Certificate**, which produces a `.p12` file), with a button that opens ARRL's own instructions.
2. **Page 2** imports the `.p12` file, its passphrase, and your DXCC entity (plus optional CQ/ITU zones, and **US state and county** — see below). The `.p12` is parsed **in the browser** — the passphrase never reaches the device.

After setup the button reads **LoTW ↑**. Each click signs all not-yet-uploaded QSOs on the device with your certificate and uploads them to lotw.arrl.org.

**US stations: fill in state and county** (new in v1.3.3). These were not being sent at all before, which meant US operators' uploaded QSOs earned **no Worked All States and no county credit** — for them or for the stations they worked. Fill them in if your TQSL station location has them. The county is the **name on its own** (`Arlington`), not `VA,Arlington`. Operators outside the US can leave both blank.

**Certificate renewal** — LoTW certificates expire roughly every 3 years. Use the **↳ Change LoTW certificate** row beneath the upload button to re-run setup (**Ctrl-click** on **LoTW ↑** still works, and was the only way in before v1.10.5). From v1.3.3, re-submitting the *same* certificate no longer re-uploads your whole log — only an actually-changed certificate resets the upload position, because a new key means every QSO has to be re-signed. So you can go back in to add a state and county without resending everything.

**What the result tells you.** From v1.10.5 the message shown after an upload is **LoTW's own reply**, not just our count. That matters: LoTW accepts a *file* and processes the contacts in it afterwards, so a figure like "22 uploaded" only ever described what was sent. If contacts are being rejected, the server's own words are what say why.

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

- **One browser at a time** — the Tab5 streams the spectrum to a single browser. Open a
  second one (or a phone, or another tab) and it takes over; the first says *"another
  browser took the live view — click to take it back"* and stops, rather than the two
  fighting over it. Click that message to reclaim it.
- **Clock sync** — the time-sync window is still on the Tab5 only
- **Real-time chat** — no operator messaging
- **Export formats** — ADIF only (import to EQSL, WSJT-X, etc. yourself)
- **Latency** — ~200 ms typical (WiFi dependent)
- **Stale connection recovery** — if the browser's network drops without a clean disconnect (e.g., putting a laptop to sleep), the web view may freeze briefly before recovering. Recovery is automatic and capped at ~5 seconds.

---

**Next:** Set up [Time Sync](time-sync.md) or explore [Settings](settings.md).
