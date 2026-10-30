# Audio Features — Tab5 Real-Time Decoding (Beta)

**Audio decoding is a BETA feature.** These implementations are first-generation and subject to further development. Real-world feedback shapes the roadmap.

## Getting audio — start here if you hear nothing

Silence is usually one of five things, and they are listed here in the order they
actually catch people out. Work down the list; each step says where the control is.

```qmxdiagram
type: flow
title: From the radio to your ears, and where each control acts
node: QMX / QMX+ - receiving, set to DiGi / IQ mode
branch: QMX RF gain - per band, 0-99 dB. Measured: does NOT reach the I/Q, so it moves neither the spectrum nor the audio
branch: QMX volume - the radio's own AF gain. Reaches everything downstream, the browser stream included
node: USB - 48 000 I/Q pairs per second, one cable
node: Tab5 DSP - 48 kHz. The same I/Q feeds the spectrum and the demodulator
node: AGC - ceiling, attack and release, or a fixed gain
node: the demodulated audio goes to ONE of these at a time
branch: Tab5 speaker and headphones - I2S codec, 48 kHz stereo. RX Volume acts here, and nowhere else
branch: a browser - 12 kHz mono mu-law over the WebSocket, 3 s buffer. Your computer's own volume acts here
note: dim | one listener at a time, deliberately: while a browser holds the stream the Tab5 mutes its own speaker, and hands it back when you stop
```

**Three controls act at three different places**, which is why "turn it up" is not one
question. The RF gain is a fourth, and the measurement below is why it is not in that
list.

### 1. Check the mode

Only **CW, CW-R, USB, LSB and AM** produce audio. **DiGi and FM produce none, by
design** — see [Which modes produce audio](#which-modes-produce-audio).

This catches more people than anything else, because **FT8, FT4, JS8 and WSPR all run
in DiGi**. If you are on a digital mode and expecting to hear the band, you will not.
Switch to USB to listen.

### 2. RX audio is off by default, and enabling it needs a restart

Open the **settings drawer** → **Audio Settings** and tick **RX Audio
(speaker/headphone)**. Then **restart the Tab5**.

The restart is not optional and it is not a bug: the audio codec is opened at boot,
before the USB host claims the DMA memory, so it cannot be opened safely part-way
through a session. From **v1.20.1** the Tab5 tells you this with a toast when you tick
the box. Before that it said so only in the serial log, which is how three operators
arrived at silent audio in one week.

The same switch and volume are on the web API from v1.20.1, in the `rxaudio` group of
`/api/settings`.

### 3. In the browser, press the play button

The browser does **not** start playing on its own. Click the **▶** button at the top
right of the page, just to the right of **F**. It turns into **■** while it plays.

This is the single most-missed step — two users reported "no web audio" in one week
and in both cases the stream was working and the button had never been pressed. See
[Listen in the browser](#listen-in-the-browser).

### The audio panel - one tap on the S-meter

New in v1.20.5, after Samuel W7STF counted four gestures to change the volume.
**Tap the S-meter in the top bar.** The panel holds:

- **Mute** - instant, both ways. Not remembered across a reboot, deliberately:
  a mute that survived one would be indistinguishable from a dead speaker.
- **Tab5 speaker** - the same volume the drawer's RX Volume sets.
- **Radio AF gain** - the QMX's own, in the decibels its LCD shows. It is read
  back from the radio when the panel opens, so it shows what the radio is
  actually set to, including a change you made on its own knob. A `--` means
  the radio has not answered yet.
- **Radio RF gain** - 0-99 dB, the same control as
  Drawer -> Radio. The QMX stores it **per band**, so it is written when you
  let go of the slider rather than on every tick of a drag. ⚠ It has **no
  effect on the spectrum or on the audio** - see the warning below.
- **AGC Fast / Slow / Off** - the three settings worth
  having in one tap. **Fast** is a 1 ms attack and a 30 ms release, for CW;
  **Slow** is 5 ms and 400 ms; **Off** pins the gain at the
  [AGC Ceiling](#the-agc-controls) rather than running a very slow AGC. The
  row shows which one is in force, and the drawer's own sliders still hold
  anything in between.

Both gain figures used to sit at `--`. They were never lost: the panel asked
the radio and painted once, while the answer arrives on the CAT poll a moment
later with nothing left running to draw it. It now keeps looking for about ten
seconds and re-asks if the first query was lost to a busy CAT link.

A tap anywhere outside closes it.

!!! warning "Mute is not the RX audio switch"
    **Mute** silences the speaker at once and brings it back at once. The
    drawer's **RX audio** switch is different: the audio hardware is opened
    when the Tab5 boots, or not at all, so turning that switch on part-way
    through a session does nothing until you restart. The panel says so when
    that is the case, rather than letting you move sliders against silence.

### 4. Know which volume control does what

There are three, and they act in different places — see
[the chain above](#getting-audio-start-here-if-you-hear-nothing) for where:

| Control | Where | What it changes |
|---|---|---|
| **QMX volume** | Drawer → Radio | The **radio's own AF gain**, over CAT — the same number the QMX shows when you click its volume knob. Affects everything downstream, including the browser stream |
| **RX Volume** | Drawer → Audio Settings | The **Tab5's** speaker and headphone level only. Does not affect the browser stream |
| Your browser's own volume | The browser or the operating system | The browser stream only |

**QMX RF gain** (Drawer → Radio, or the audio panel) is per band, 0–99 dB, QMX
default 54. It looks like a fourth control in this chain. It is not:

!!! warning "RF gain does not change what the Tab5 shows or plays"
    Measured 2026-10-27: taking the RF gain from 54 to 0 moved the spectrum
    noise floor by **0.06 dB** over 81 frames. The radio accepts the command
    and reads the new value back, so it is a real setting in the QMX - it just
    does not reach the I/Q the Tab5 receives. The spectrum, the waterfall, the
    decoders and the Tab5's own audio are all derived from that one stream, so
    none of them can respond to it. If signals sound weak, the control that
    acts is the [AGC ceiling](#the-agc-controls). Reported by Samuel W7STF,
    who was right; announced in v1.21.0 as a way to change what reaches your
    ears, which was wrong.

    **Why, from QRP Labs.** Stan KC7XE, 2026-10-28: I/Q mode delivers the raw
    48 kHz sampled stream from the PCM1804 converter, and every adjustment
    and all the DSP happen further down the radio's own processing chain. So
    the control is real and it works - on the QMX's own headphone and speaker
    output. It is kept for that reason rather than removed.

    It is deliberately **not** faked by scaling the samples on the Tab5.
    Multiplying every sample by the same number scales the signal and the
    noise together, so the signal-to-noise ratio does not change and nothing
    becomes audible that was not already there - it would only move the trace
    up and down the dB scale, which the floor and ceiling controls already do.


!!! note "Full volume and still quiet?"

    The level is governed by an **automatic gain control**, not by the volume slider.
    If a signal is quieter than you expect at full volume, the AGC ceiling is holding
    it back — see [The AGC controls](#the-agc-controls).

### 5. The speaker and the browser are exclusive

While a browser is listening, the Tab5 **mutes its own speaker**, and hands the audio
back when you stop listening or close the page. One listener at a time is deliberate.
So if the Tab5 went quiet, check whether a browser somewhere on your network is holding
the stream.

## Panoramic CW Split

Listen to two CW stations on different frequencies, panned left and right across the stereo stage. The CW receiver demodulates each sideband independently, allowing separation by frequency.

### How It Works

The RF passband is split into two channels:
- **Left**: Lower-frequency station (panned fully left)
- **Right**: Upper-frequency station (panned fully right)
- **Center blend**: Dry/wet mix between the two

Three live controls tune the effect mid-QSO without leaving the exchange, grouped under **Panoramic split — adjust while listening**:

- **Width** — Frequency spread between the two stations. Wider = greater separation.
- **Blend** — Dry/wet audio mix. Left is pure lower channel; right is pure upper channel.
- **Overlap** — Frequency range for each station. Wider ranges allow more signal capture.

### Enabling Panoramic CW

1. Open the settings drawer and tap **Audio Settings** (just below SelfSpotter)
2. Tick **RX Audio (speaker/headphone)**, then **Binaural CW (stereo separation)**

!!! warning "Switching RX audio on needs a restart of the Tab5"

    The audio hardware claims its memory during start-up, before the USB host
    takes that pool, so it is started at boot or not at all. If you boot with
    **RX Audio** off and tick it later, the setting is saved but you will hear
    nothing until the Tab5 is restarted — it tells you so on screen when you
    tick it. Your QMX will need a power cycle after that restart, as it does
    after any Tab5 restart.

!!! note "Which modes produce audio"

    **CW, CW-R, USB, LSB and AM.** **FM is still silent** — there is no FM
    demodulator, so an FM signal has to be listened to in USB or not at all.

    AM is demodulated on the Tab5 itself, from the I/Q the radio sends, so the
    bandwidth is set by the Tab5 and not by the filter in the radio. Tap **BW**
    in the top bar to choose it: **2.0, 3.0, 4.0, 6.0, 8.0 or 9.6 kHz**,
    quoted as the width of the whole signal, the way amateur AM
    figures always are. 6.0 kHz is the default and the standard — 3 kHz per
    sideband, what most commercial rigs hard-code. The same list is on the
    web page's **BW** button.

    Go **narrower** when someone is sitting on top of you: 2.0 and 3.0 kHz
    sound like a telephone line and that is the point, they keep a signal
    copyable in heavy QRM. 6.0 kHz is the widest on offer and the one to use
    on a clear channel. Broadcast channels are spaced 5 kHz, so 6.0 already
    reaches into the neighbour's carrier on a crowded shortwave evening.

    **9.6 kHz is the widest the Tab5 offers, from v1.20.2.** Two wider
    settings were available earlier and have been withdrawn: **10.0 and
    20.0 kHz** ran the receive chain at half the decimation the others use,
    and measured on the bench that leaves the second CPU core with no idle
    time at all and the audio stutters.

    **8.0 and 9.6 kHz cost nothing extra** - matched 60-second windows
    measured AM at 4.0 and at 6.0 kHz at the same processor load, so width is
    free up to that point. They were briefly withdrawn in v1.20.1 as
    unproven and are back in v1.20.2: one operator runs 9.6 kHz daily and
    another asked for 8.0 kHz, which settles whether they work.

    CW and SSB are untouched at any setting.

The three pan controls sit directly under the Binaural CW row in the same window. They stay greyed out until both boxes above are ticked, because they shape a split that is not being produced otherwise.

### On the Air

All three controls are **live-tunable** during an exchange:
- Slide them while the QSO is running
- No reboot, no mode change required
- Changes take effect immediately

## Binaural CW — First Implementation

**Frequency-based station separation in CW.** Each station gets a distinct audio tone, allowing the ear to distinguish them without losing either signal.

### Bandwidth Requirements

Binaural separation works best with **bandwidths of 300 Hz and above**.

**Do not expect good separation below 200 Hz** — the audio bandwidth is the limiting factor. At 200 Hz or narrower, the two stations' spectra start to overlap, and separation breaks down. This is a fundamental constraint of the audio bandwidth, not the algorithm.

### Recommended Bandwidth Settings

| Bandwidth | Clarity | Use Case |
|-----------|---------|----------|
| 300 Hz+   | Excellent | General CW operation; two stations easily distinguished |
| 200–300 Hz | Fair | Tight pileup; separation possible but marginal |
| <200 Hz   | Poor | Not recommended; too much spectral overlap |

## Audio Settings

Audio decoding shares the Tab5's compute and memory budget with the spectrum, waterfall, and FT8/FT4 decode paths. Not all features can run simultaneously.

!!! note "Renamed in v1.16.4"

    This window used to be called **Resource Management**. It is **Audio
    Settings** now, because that is what it is — volume, AGC, binaural and the
    panoramic split. The old name described how it works rather than what it does.

### Opening Audio Settings

Open the **settings drawer** and tap **Audio Settings**, directly below SelfSpotter.

!!! note "Top-bar shortcut removed in v1.16.8"

    A long-press on the top bar used to open Audio Settings too. It shared its
    area with the Band, Mode, BW and Frequency pickers, so it was easy to open
    the wrong thing by accident. The drawer button is now the only way in.

The window lists RX audio and the background network feeds that compete with it for the same scarce memory, each with a tick box:

- **RX Audio (speaker/headphone)** — with RX Volume and the three AGC controls described below
- **Binaural CW (stereo separation)** — with the three panoramic-split controls
- **SelfSpotter**, **RBN**, **DX cluster**, **PSK Reporter — who's hearing me** — these grey out and are held off while RX audio is on
- **POTA / SOTA spots**, **PSK Reporter — report my decodes** — these keep running

The list scrolls; the **Close** button stays put at the bottom.

**Toggle audio on and off as needed** — the resource footprint is immediate, and disabling a feature instantly frees that headroom.

### Typical Constraints

- **Audio ON, Waterfall ON, Decode ON** → Some margin left; FT8/FT4 decode still runs steadily
- **Audio ON, Waterfall ON, Decode ON, Web streaming ON** → Tight; any spike can cause temporary frame drops
- **Audio ON, full WiFi + web load** → Audio is the first to suffer if the load spikes; consider disabling audio or the waterfall if you need reliable web streaming

## Headphones

Plug headphones into the Tab5's 3.5 mm jack and the **internal speaker goes
quiet on its own**. Unplug them and it comes back. Nothing to switch.

Before v1.16.4 you heard both at once, which made headphones close to useless in
a quiet shack *(Roy KI0ER)*.

!!! tip "The internal speaker is small and trebly"

    Several operators have said so, and they are right — it is a tablet
    speaker. Headphones or a powered external speaker on the jack are a large
    improvement, and now that the internal one mutes itself either is
    practical.

## Which modes produce audio

Audio and the panoramic split are not the same thing, and it is worth being clear about which you get where.

| Mode | Audio | Panoramic / binaural split |
|------|-------|----------------------------|
| CW, CW-R | Yes | **Yes** |
| USB, LSB | **Yes** | No — single channel, both ears the same |
| AM | **Yes**, to about 3 kHz | No — AM has no stereo content to split |
| DiGi | No | No |
| FM | No | No |

So you **do** get audio on SSB and AM; what you do not get there is the left/right split, because that is a CW feature. In DiGi and FM there is no audio at all — nothing is broken if you hear silence in those.

## Known Limitations

- **Audio breaks up while a web page is open.** Known and unfixed as of v1.16.3. With a browser connected to the panadapter page the audio task misses some of its deadlines and you hear a regular crackle; closing the page clears it. Measured, but the cause is not yet found — if you need clean audio, close the web page.
- **Stereo rendering requires a headset or speaker with stereo output.** Mono output collapses both channels; panning does not work.
- **The split is CW-only** (see the table above). On SSB you hear the signal, but it is not placed left or right.
- **Limited to the audio passband.** If the audio filter is 300 Hz wide, the maximum usable separation is about 300 Hz. A wider filter in the radio settings increases the available range.
- **Level is set by an automatic gain control, not by the volume slider.** If a signal is quieter than you expect even at full volume, the AGC's ceiling is what is holding it back. All three AGC controls are adjustable live — see below.

## The AGC controls

All four audio level controls live in **Audio Settings** (settings drawer → Audio Settings), on their own rows under **RX Audio**:

| Control | Range | Default | What it does |
|---|---|---|---|
| **RX Volume** | 0 – 100 | 60 | Output level to the speaker or headphones. |
| **AGC Ceiling** | 50 – 1500 | 200 | How far the AGC may lift a *weak* signal. Raise it if quiet stations stay quiet. |
| **AGC Attack** | 1 – 50 ms | 3 ms | How fast the gain comes *down* when a signal arrives. Shorter catches sharp CW edges; too short can sound clipped. |
| **AGC Release** | 10 – 500 ms | 150 ms | How fast the gain comes back *up* afterwards. Longer is steadier on CW; shorter follows speech better. |

All four are remembered across restarts and travel in a config backup.

**AGC presets.** Four buttons under the sliders set the timing in one tap:

| Preset | Attack | Release | For |
|---|---|---|---|
| **Fast** | 1 ms | 30 ms | CW |
| **Med** | 3 ms | 150 ms | general, and the shipped default |
| **Slow** | 5 ms | 400 ms | SSB, where a fast release pumps on speech pauses |
| **Off** | — | — | no automatic gain at all |

Move either slider and the caption reads **Custom** — the sliders *are* the
custom setting, so there is no fifth button.

**Off is a real bypass, not a very slow AGC.** The gain follows the signal at
every attack and release value, so no combination of the sliders switches it
off. With **Off** selected the gain is pinned at **AGC Ceiling**, and that
slider becomes a plain manual gain control.

⚠ **What to expect when you try them.** The AGC only starts regulating once a
signal is strong enough; below that it already sits at the ceiling, so the gain
is the same number whether the AGC is on or off. On a quiet band **every preset
correctly sounds identical** — the difference is heard in how fast the noise
returns between signals when the band is busy. The attack values (1–5 ms) are
all far quicker than the ear resolves; **Release** is the audible one.

**Volume and Ceiling are not the same thing.** Volume sets how loud the output is; Ceiling decides how hard a weak signal is lifted before it gets there. Running Volume to 100 does not mean you have run out of level — if it is still too quiet, raise **AGC Ceiling**.

### What changed in v1.16.3

- **AGC Ceiling was called "Gain".** The name was wrong: it never was a plain gain, it is the ceiling on the AGC's own weak-signal boost, and calling it Gain led people to look for an AGC that was already there. Its range also went up from 800 to 1500, because the Tab5's internal speaker wants more headroom than the old ceiling allowed.
- **Attack and Release are new** — the AGC's timing was fixed before, and is now yours to set.
- **Presets arrived in v1.16.5** *(Samuel W7STF)*, along with a genuine **Off**.
- **RX Volume moved here from the settings drawer**, and the drawer's RX Audio section is gone entirely. That includes its on/off box: **Audio Settings is now the only place to switch RX audio on or off.**

### What changed in v1.16.4

- **The window is called Audio Settings**, and it has a door of its own in the
  settings drawer. The top-bar long-press still works.
- **Headphones mute the internal speaker** automatically.

## Strong signals

A peak limiter sits after the automatic gain, so a loud station cannot arrive at full scale and startle you in headphones. It holds the peaks down and leaves the average level alone, which means raising **AGC Ceiling** to hear weak stations does not make the strong ones painful.

This is the part of the beta I most want reports on: which mode, roughly how strong the signal was, and whether more ceiling fixed it.

## Where the receive signal comes from

Until now the panadapter always listened to the **radio's USB audio**. There is now a
**receive source** selector, on the Tab5 and in the browser, with a level meter and a
clip indicator beside it.

| Source | State |
|---|---|
| **QMX USB** | The default, and what every earlier release did. |
| **Line in (3.5 mm jack)** | **Declared beta** - see the warning below. |
| Generic I/Q | Declared, not implemented. Selecting it is refused. |
| Internal mics | Declared, not implemented. Selecting it is refused. |

**Line in** *(asked for by Tony, @tony1tf, in issue #17)* feeds the panadapter from the
Tab5's own 3.5 mm headphone jack instead of the radio's USB. Tony ran a test build on his
own Tab5 to find where the jack lands — the mic ring arrives on the audio chip's third
channel — which is the measurement the whole feature rests on. The jack is a single real audio channel, not I/Q, so the second
channel is synthesised: a 255-tap filter builds the quadrature pair the decoders need.
The jack wiring is drawn on screen so you can see which contact carries what.

⚠ **No signal has been decoded through line in yet.** The path runs, samples flow and
the levels read correctly with nothing plugged in - that is the whole of what has been
verified. Treat it as unfinished. Changing the source needs a restart, and the Tab5
does the restart itself when you press **Save**.

⛔ **In line-in mode the USB radio connection is off by design.** One or the other, not
both - so CAT and the S-meter go quiet while line in is selected. The Tab5 stops
pretending to have a dial with it: the frequency readout reads **Line-in mode**, the
band shows `---` and the band-plan strip is hidden, because an audio spectrum has no
band plan and no RF frequency. The frequency axis under the display reads audio Hz.

The left half of the spectrum is empty and there is a step in the middle. That is
correct and it is the honest picture: the jack carries one real channel, the second is
synthesised, and a synthesised channel cannot recover what the missing one would have
told you. The step sits at audio 0 Hz, which is the centre of the span.

!!! info "New in v1.20.3 — the source and its gain are in the browser too"

    **Audio source** and **Line-in gain** are on the web Settings page, under
    *Radio & display*. Gain moves in the 3 dB steps the codec actually has, and
    takes effect immediately while line-in is running.

    ⚠ Changing the **source** from the browser only **stores** the choice — it
    takes effect at the next restart. The Tab5's own *Audio Settings* window
    restarts for you; the browser deliberately does not reboot your radio's
    host from across the house.

## Listen in the browser

From **v1.20.0** the panadapter streams its own demodulated audio to any browser on
your network. Open the web page and click the **▶** button at the top right, just to
the right of **F**. It becomes a **■** while it is playing; click it again to stop.

What you hear is exactly what the Tab5's own speaker gets — the same demodulated
audio, tapped after the filter and the AGC — so the two can never disagree about what
the radio is doing. It works even if the Tab5 booted with its speaker turned off.

**The speaker and the browser are exclusive.** While a browser is listening the Tab5
mutes its own speaker, and it hands the audio back when you stop listening or close the
page. There is no setting for this: one listener at a time was the point.

Only the modes the Tab5 demodulates produce audio: **CW, CW-R, USB, LSB and AM**. In any
other mode the button lights up but nothing arrives, and hovering it says so.

**Hover the ■ button for the health of the stream** — it reports frame gaps, re-primes
and chunks dropped to clock drift. On a quiet network all three stay at zero.

Notes on what this costs and what it does not:

- It runs over the web page's existing connection, so it needs no extra port and no
  extra setup.
- It costs about **9 percentage points of one CPU core** — measured — and nothing
  measurable on the core that serves the web page.
- Audio is 12 kHz µ-law, which is a telephone-grade codec chosen because it survives a
  lossy Wi-Fi link without a jitter buffer deep enough to be annoying.
- **Only one browser gets the page at a time.** Opening the panadapter page in a second
  browser takes the connection from the first, which takes the audio with it.

## Feedback

This feature is **beta and in active development.** Your feedback shapes the next iteration:
- Does panoramic CW work in the field?
- What bandwidth do you typically use?
- What other audio features would you like to see?
- Did you hit the resource limits?

Send feedback to the [discussions page](https://github.com/SteffenLav/qmx-panadapter/discussions) or [open an issue](https://github.com/SteffenLav/qmx-panadapter/issues).
