# Getting Help

The Tab5 carries its own help. There is no app to install, no QR code to scan and
no network needed: **this entire manual is built into the firmware**, so it works
on the very first boot, in a field with no signal, with no microSD card fitted.

There are three ways in, and they are all one tap from wherever you are.

| Way in | Where | Use it when |
|--------|-------|-------------|
| **User Manual** | Settings drawer, top button | You want to read about what you are looking at |
| **Need guidance?** | Settings drawer, directly below it | Something is wrong and you do not know what it is called |
| **Tap the warning** | On the warning itself | The Tab5 has just told you something is wrong |

To reach the drawer, swipe in from the **right edge** of the screen.

---

## The manual opens where you are

The **User Manual** button does not drop you at a contents page to hunt through.
It looks at what is on screen and opens the chapter that covers it:

| You are on | It opens |
|------------|----------|
| Panadapter | The panadapter chapter, at the screen layout |
| FT8 or FT4, receiving | The FT8 receive chapter, at the decode list |
| FT8 or FT4, with a transmission armed or running | The FT8 transmit chapter |

The transmit case is deliberate: someone mid-transmission is asking a different
question from someone watching the decode list, and the device already knows
which of the two you are.

Once you are in the manual you can go anywhere:

- **Contents** lists every chapter in two columns. Press and slide your finger
  down the list — a highlight bar follows your finger, and lifting opens the
  highlighted chapter.
- **Back** returns to the previous page you were reading. It only appears when
  there is somewhere to go back to.
- **Exit** leaves the manual and returns you to the panadapter or FT8 screen you
  came from.

While the manual is open, the edge swipes and top-bar taps are stood down, so a
stray touch cannot navigate the radio out from under you. Exit puts them back.

> If a newer firmware version is available, a banner appears at the top of the
> manual. It is informational only — flashing is always your choice.

---

## "Need guidance?" — describe the symptom, not the cause

The second drawer button opens a short list headed **"What do you need help
with?"**. Every row is written the way you would say it out loud:

```
My radio is not showing up
Nothing appears in the decode list
It never transmits
Decodes look late, or the timer is off
The spectrum is flat - no signals
Where are my contacts logged?
```

Pick the one that fits and it opens the manual at the section that answers it.
You never have to know that "my radio is not showing up" is a CAT link problem,
or which chapter the CAT link lives in.

The list holds **questions as well as faults** — how to change what your CQ says,
how to zoom the spectrum, what the coloured callsigns on the trace are, where
your contacts are logged. It scrolls, so keep going past the first few rows.

**Rows that are happening right now are highlighted** and moved to the top, with
a warning triangle. The Tab5 can see four things for itself:

| Highlighted row | What the device checked |
|-----------------|-------------------------|
| My radio is not showing up | No CAT link to the QMX |
| The spectrum looks mirrored or shifted | The QMX never confirmed IQ mode |
| Nothing appears in the decode list | FT8/FT4 running, radio present, nothing decoded |
| I cannot reach the web page | WiFi is switched on but not connected |

WiFi deliberately switched off — for POTA, or to save battery — is **not**
reported as a fault. The row stays in the list as a normal question; it just is
not flagged.

> **It ranks, you choose.** A highlighted row is a suggestion, never an
> instruction, and the Tab5 will never jump you into a chapter because it thinks
> it knows what you meant. If the highlighted row is not your problem, ignore it
> and pick another.

The rows offered depend on the screen you opened the panel from. In FT8 you will
not be asked about the spectrum, because there is no spectrum drawn there.

At the bottom, **Open the manual** takes you to the manual itself if none of the
rows fit, and **Close** (or a tap outside the panel) puts you back exactly where
you were.

---

## Warnings you can tap

A warning you cannot act on is only half a warning, so the two that matter are
buttons:

- **The red IQ-mode banner** across the top of the screen — "QMX IQ mode not
  confirmed - spectrum may be mirrored/shifted". Tap it and the manual opens at
  the section on exactly that. (It only spans the top of the screen while the
  warning is up, so it cannot swallow taps the rest of the time.)
- **"Waiting for QMX"** — the message shown before the radio is found carries a
  small **Need help?** button under it. It says *help* rather than *what is
  wrong* on purpose: a QMX that is switched off is often switched off
  deliberately, and you may simply be reading the manual while you set up.

---

## If the help itself misbehaves

**Hold the drawer's User Manual button for 3 seconds.** That resets the reader.
The manual itself is untouched — it is part of the firmware and cannot be
deleted, corrupted or left out of date, because it ships inside the same binary
as the code it describes. It also cannot describe a different version than the
one you are running.

If a chapter does not answer your question, the fault is ours. Say so on the
[QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643) or
[GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) — the
wording of these rows and chapters is written from what operators actually report,
so a "this made no sense to me" is genuinely useful.

For a fault that needs looking into, the **diagnostic log is always on** — see
[Collecting Diagnostics](reference/troubleshooting.md#collecting-diagnostics).

---

**Next:** [Quick Start](quick-start.md) if you are setting up, or
[Troubleshooting](reference/troubleshooting.md) for the full symptom list.
