# Audio Features — Tab5 Real-Time Decoding (Beta)

**Audio decoding is a BETA feature.** These implementations are first-generation and subject to further development. Real-world feedback shapes the roadmap.

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

You can also **long-press the top bar** in the Panadapter view — any of the Band,
Mode, BW, Freq or Zoom areas. That still works, but those same areas open the
Band, Mode, BW and Frequency pickers on a short tap, so it is easy to land on one
of those instead. The drawer button cannot be mistaken for anything else.

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
| DiGi | No | No |
| AM, FM | No | No |

So you **do** get audio on SSB; what you do not get there is the left/right split, because that is a CW feature. In DiGi, AM and FM there is no audio at all yet — nothing is broken if you hear silence in those.

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

## Future Work — Web Audio (IQ Streaming)

The next phase will stream **I/Q channels only** from the Tab5 to a PC browser, allowing the browser's own CPU to do the audio processing locally. This approach:

- **Reduces Tab5 load** — the device sends raw I/Q data (~768 kbps at 48 kHz stereo), no processing
- **Offloads processing to the PC** — your computer does the heavy lifting
- **Allows richer audio features** — filtering, EQ, advanced separation, and recording
- **Works with any audio software** — your browser runs the decoder

Timeline: pending real-world feedback on the current implementation.

## Feedback

This feature is **beta and in active development.** Your feedback shapes the next iteration:
- Does panoramic CW work in the field?
- What bandwidth do you typically use?
- What other audio features would you like to see?
- Did you hit the resource limits?

Send feedback to the [discussions page](https://github.com/SteffenLav/qmx-panadapter/discussions) or [open an issue](https://github.com/SteffenLav/qmx-panadapter/issues).
