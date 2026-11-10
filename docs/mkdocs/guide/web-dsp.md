# Web DSP: Noise Reduction and Auto-Notch

From **v1.26.0** the web page can clean the audio before you hear it. There are
three tools, and all three run **in your browser**:

| Tool | What it does | Modes |
|---|---|---|
| **Noise reduction - Speech model** | A neural network trained on speech removes what is not a voice | USB, LSB, AM |
| **Noise reduction - Spectral** | Follows the noise floor in each 47 Hz slice of the audio and turns down the slices that hold only noise | USB, LSB, AM, CW, CW-R |
| **Auto-notch** | Finds a steady carrier and filters it out, up to four at once | USB, LSB, AM |

The Tab5 does none of this work. It sends the same audio as before, and the
browser processes it. So it costs the Tab5 no CPU and no memory, it cannot
disturb decoding, and it changes nothing on the Tab5's own speaker.

*Asked for by Samuel W7STF.*

## Switching it on

1. Open the web page and click **▶** (Listen) at the top right. The tools work on
   the audio that Listen plays, so there is nothing to hear without it. See
   [Listen in the browser](audio.md#listen-in-the-browser).
2. Open **Radio** in the bottom bar. The noise reduction row is in that menu.
3. Tick **Noise reduction**. Pick **Low**, **Med** or **High**. Picking a level
   also switches it on. It starts at **Med**.
4. Pick **Speech model** or **Spectral**. This choice applies to the voice modes
   only. CW always uses Spectral.
5. Tick **Auto-notch** if a carrier is in the way. It is a separate switch and
   works with noise reduction on or off.

The settings are stored in the browser, so each browser and each computer keeps
its own.

A line of grey text under the row tells you when a tool is not working in the
present mode:

| The row says | Meaning |
|---|---|
| `loading...` | The browser is fetching the speech model from the Tab5 |
| `CW uses Spectral` | Speech model is selected, the radio is in CW, and the page uses Spectral instead |
| `NR is not active in DIGI` | Noise reduction does nothing in this mode |
| `notch is not active in CW` | Auto-notch does nothing in this mode |
| `speech model could not load (...)` | The fetch failed. Noise reduction switches itself off. Reload the page to try again |

## Which one to use

| You are listening to | Use | Why |
|---|---|---|
| SSB or AM voice in band noise | **Speech model**, Med | The stronger of the two on a voice |
| SSB voice, and the speech model changes the voice too much | **Spectral**, Med | It makes no assumption about the signal, so it is gentler |
| CW | **Spectral** (automatic) | The speech model removes a CW note, because a note is not speech |
| SSB or AM with a carrier or a tuner on the frequency | **Auto-notch**, with or without noise reduction | Noise reduction is not built to remove one tone; the notch is |
| FT8, FT4, JS8, WSPR | Nothing | Nobody listens to them, and the decoders do not use this audio |

**High is not "better Med".** On a clean signal High removes part of the voice.
Use it when the noise is the problem, not as a default.

## Speech model

This is **RNNoise** (version 0.1.1, by Jean-Marc Valin / Xiph.Org, BSD licence),
compiled to WebAssembly. It is a small recurrent neural network that was trained
to tell speech from everything else.

- The first time you switch it on, the browser fetches it from the Tab5:
  **141 KB**, once. After that the browser keeps it. It works with no internet
  connection.
- The level is a mix of processed and unprocessed audio. Low leaves the noise
  about 6 dB down, Med about 13 dB, and High passes only what the model keeps.
- USB, LSB and AM only.

Measured on real SSB from 40 m and 20 m, in the pauses between words:

| Level | Noise in the pauses | Loss in the speech itself (upper bound) |
|---|---|---|
| Low | about 6 dB down | 2 dB |
| Med | about 13 dB down | 4 dB |
| High | 19 to 31 dB down | 4 to 5 dB |

The speech figure is an upper bound: the speech blocks hold noise too, and part
of what is removed there is that noise. On a test voice with a 20 dB
signal-to-noise ratio, High made the result worse than no processing. That is
why Med is the default.

## Spectral

This is classic spectral noise reduction, written in plain JavaScript as part of
the page. Nothing is fetched.

How it works:

1. The audio is cut into overlapping blocks and each block is split into
   **47 Hz** slices.
2. For each slice, the page tracks the noise floor. It takes the minimum of the
   smoothed power over the last 0.6 to 1.2 seconds, and it updates the floor
   only while the slice is near that minimum.
3. A slice that holds more than the floor is kept. A slice that holds only the
   floor is turned down.
4. The level sets the most that any slice can be turned down: **Low 6 dB,
   Med 12 dB, High 20 dB**.

It makes no assumption about what the signal is. A CW signal is kept because it
has gaps: the floor is measured in the gaps, and the note stands above it.

A carrier that never keys has no gaps. After about one second the tracker takes
it for noise and turns it down to the level's limit, in every mode.

Measured:

| Signal | Low | Med | High |
|---|---|---|---|
| Steady test noise | 6 dB down | 12 dB down | 18 dB down |
| Real SSB, noise between words | 4 to 6 dB down | 5 to 10 dB down | 6 to 14 dB down |
| Real 40 m noise through the CW filter | 6.0 dB down | 11.7 dB down | 16.3 to 16.9 dB down |
| Real CW, gaps of 60 ms and longer | 6 dB down | 12 dB down | 19 to 20 dB down |
| Real CW, all key-up time including the short gaps inside a character | 4.0 dB down | 5.9 dB down | 6.6 dB down |

Two things follow from the CW rows:

- The gaps between words and between characters go quiet. The short gaps inside
  a character do not, because the tracker cannot follow a gap that short.
- A strong note passes almost unchanged: it was 0.3 dB down.

On a CW test signal at Med, a clear signal gains 8 to 10 dB over the noise, a
weak one gains 1 to 3 dB, and a signal at the edge of copy gains nothing. A weak
note is also turned down, but less than the noise is. **Weak CW on the air was
not measured.**

## Auto-notch

Auto-notch removes a steady carrier from the audio: a station tuning up, a
broadcast carrier beside an SSB signal.

- It looks between **100 Hz and 3600 Hz** for a line that stands 9 dB or more
  above the audio near it and holds one frequency.
- A carrier is found in about **one second**. The page then puts a filter
  **40 Hz** wide on that frequency. Up to **four** carriers at once.
- Measured depth on a carrier added to real speech: **31 to 59 dB**.
- About **half a second** after the carrier stops, the filter is removed.
- With no carrier, the audio passes through unchanged, sample for sample.
  Measured on real SSB that holds no carrier.
- A voice does not trigger it. A voice harmonic does not hold one frequency for
  most of a second, and the gaps between words restart the count.
- It adds no delay.
- **USB, LSB and AM only.** In CW the signal you want is a carrier.

## Short dropouts are patched

The audio stream to the browser can hold short holes, about 11 ms long, when
the Tab5's WiFi is busy. With noise reduction **on**, the page fills a hole of
8 to 58 ms with a copy of the audio just before it, faded in and out. A longer
silence is left alone, because that is a closed squelch or a real outage.

With noise reduction **off**, the audio is passed through untouched.

## What it costs

| | |
|---|---|
| Tab5 CPU and memory | None |
| Delay | About **0.1 s** with noise reduction on (107 ms). The notch adds none |
| Browser CPU | Speech model: about 1.5 % of one core, measured on a desktop PC |
| Download | 141 KB, once, for the speech model only |
| Page size | The spectral code and the notch add 6.3 KB to the page |

The delay matters for one thing: if you send CW and listen to your own signal
through the browser, the sound is late. Use the radio's own sidetone for that.

## What is tested and what is not

| | Measured | Heard on the air |
|---|---|---|
| Speech model on SSB | Yes, on real SSB | Yes |
| Spectral on CW | Yes, on real CW (strong signal) | Yes |
| Spectral on SSB | Yes, on real SSB | **No** |
| Auto-notch | Yes, carrier added to real SSB | **No** |

The two rows marked **No** work by measurement, and nobody has yet judged them
by ear on a live band. If you use them, a report of what you hear is welcome:
[open an issue](https://github.com/SteffenLav/qmx-panadapter/issues) or post in
the QRP Labs group.

## Limits

- **One browser at a time.** Only one browser gets the page's live connection,
  and the audio goes with it.
- **The Tab5's own speaker and headphones get no noise reduction.** These tools
  exist only in the browser.
- **The radio's own headphone output is not affected** either.
- The speech model is trained on speech in general, not on SSB. It is not
  retrained for this use.
