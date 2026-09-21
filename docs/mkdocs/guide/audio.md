# Audio Features — Tab5 Real-Time Decoding (Beta)

**Audio decoding is a BETA feature.** These implementations are first-generation and subject to further development. Real-world feedback shapes the roadmap.

## Panoramic CW Split

Listen to two CW stations on different frequencies, panned left and right across the stereo stage. The CW receiver demodulates each sideband independently, allowing separation by frequency.

### How It Works

The RF passband is split into two channels:
- **Left**: Lower-frequency station (panned fully left)
- **Right**: Upper-frequency station (panned fully right)
- **Center blend**: Dry/wet mix between the two

Three live controls tune the effect mid-QSO without leaving the exchange:

- **Pan Width** — Frequency spread between the two stations. Wider = greater separation.
- **Pan Blend** — Dry/wet audio mix. Left is pure lower channel; right is pure upper channel.
- **Overlap** — Frequency range for each station. Wider ranges allow more signal capture.

### Enabling Panoramic CW

1. Tap and hold the **top bar** in the Panadapter view
2. Open **Resource Management**
3. Enable **CW Audio**

The three pan controls appear in the Panadapter view once enabled.

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

## Resource Management

Audio decoding shares the Tab5's compute and memory budget with the spectrum, waterfall, and FT8/FT4 decode paths. Not all features can run simultaneously.

### Opening Resource Management

**Tap and hold the top bar in the Panadapter view**, then select **Resource Management**. 

The window shows:
- Current resource usage by each component
- Available headroom
- Which features can be toggled on/off
- Warnings if enabling a feature would exceed capacity

**Toggle audio on and off as needed** — the resource footprint is immediate, and disabling a feature instantly frees that headroom.

### Typical Constraints

- **Audio ON, Waterfall ON, Decode ON** → Some margin left; FT8/FT4 decode still runs steadily
- **Audio ON, Waterfall ON, Decode ON, Web streaming ON** → Tight; any spike can cause temporary frame drops
- **Audio ON, full WiFi + web load** → Audio is the first to suffer if the load spikes; consider disabling audio or the waterfall if you need reliable web streaming

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

- **Stereo rendering requires a headset or speaker with stereo output.** Mono output collapses both channels; panning does not work.
- **The split is CW-only** (see the table above). On SSB you hear the signal, but it is not placed left or right.
- **Limited to the audio passband.** If the audio filter is 300 Hz wide, the maximum usable separation is about 300 Hz. A wider filter in the radio settings increases the available range.
- **Level depends on an AGC that is still being tuned.** If a signal is quieter than you expect even at full volume, that is the AGC's gain ceiling rather than your volume control. It is adjustable live — see below.

## If the level is too low

The gain ceiling, target and clip point can all be changed while listening, with no reflash, from the browser:

```
POST /api/cmd   {"action":"rxaudio","agc_gain_max":400}
```

Send `{"action":"rxaudio"}` with no other fields to read the current values back. That reply also carries the diagnostics that say whether audio is arriving at all — in particular **`read_timeouts`**, which is what to look at first: anything above zero means the audio feed is being starved, and no AGC setting can compensate for samples that are not there.

This is the part of the beta I most want reports on.

## Future Work — Web Audio (IQ Streaming)

The next phase will stream **I/Q channels only** from the Tab5 to a PC browser, allowing the browser's own CPU to do the audio processing locally. This approach:

- **Reduces Tab5 load** — the device sends raw I/Q data (~768 kbps at 48 kHz stereo), no processing
- **Offloads processing to the PC** — your computer does the heavy lifting
- **Allows richer audio features** — filtering, EQ, advanced separation, and recording
- **Works with any audio software** — your browser runs the decoder

Timeline: **After v1.16.0**, pending real-world feedback on the current implementation.

## Feedback

This feature is **beta and in active development.** Your feedback shapes the next iteration:
- Does panoramic CW work in the field?
- What bandwidth do you typically use?
- What other audio features would you like to see?
- Did you hit the resource limits?

Send feedback to the [discussions page](https://github.com/SteffenLav/qmx-panadapter/discussions) or [open an issue](https://github.com/SteffenLav/qmx-panadapter/issues).
