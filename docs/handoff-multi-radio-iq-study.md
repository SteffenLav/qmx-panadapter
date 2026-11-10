# HANDOFF - multi-radio I/Q panadapter study (written 2026-11-10)

For a NEW session, started by the operator (Steffen, OZ1LAV) on a feature branch,
possibly on another account. Nothing here has been researched yet. No code exists.
v1.26.0 is released and is not part of this task.

## The ask (operator's words, 2026-11-10)

"A high level study / plan of how to implement a general I/Q stream from the many
available SDR front-ended HAM transceivers from ICOM, Yaesu, Elecraft, Kenwood,
FlexRadio etc. First sort out what kind of parameter list or matrix is needed to be
mapped, and then how this could be tackled and wired to the Panadapter on the Tab5.
I would like the same or better functionality as the QMX-Panadapter."

## Deliverable

ONE document, `docs/iq-multi-radio-study.md`, same style as `docs/rtty-feasibility.md`
and `docs/js8-feasibility.md` (TL;DR, tables, phased plan with session estimates,
ranked risks, source links). No firmware code. Order is fixed by the operator:

1. **The parameter matrix first** - which fields describe a radio's I/Q source, one
   row per radio/model family. Then, and only then,
2. **How to tackle it** - an abstraction, a reuse map against the current code, a
   phased plan, and the wiring to the Tab5 panadapter.
3. **Functionality parity** - a table of every QMX-Panadapter feature and whether it
   survives, degrades or is lost per radio class. "Same or better" is the target;
   say plainly where it cannot be met.

## Parameters the matrix probably needs (starting list - extend it, do not trust it)

- I/Q source: none / USB audio class (UAC) I/Q / USB vendor protocol / LAN (VITA-49,
  proprietary UDP/TCP) / analog IF output (needs an ADC) / spectrum-scope data only
  (not I/Q at all - many rigs give only a scope line, not samples).
- Sample rate, bit depth, channel layout (I/Q as L/R, interleaved, packed), who owns
  the clock, latency, and the I/Q centre frequency relationship to the dial frequency.
- Bandwidth actually delivered (a 48 kHz I/Q is not a wide panadapter).
- Control path: CAT over USB serial (CDC) / LAN / CI-V / vendor API; command set for
  frequency, mode, filter, PTT, step, AGC, preamp/att, band.
- Does the I/Q follow the VFO (tracking) or is it fixed (wideband)? Does the radio
  report the tuned centre so the Tab5 can place the passband?
- RX only, or TX I/Q too. Can I/Q and the radio's own audio run together?
- Host requirement: USB host from the Tab5 (what it supports today is UAC + CDC-ACM
  to one device), hub support, power draw, LAN-only radios need the Tab5 on the
  same network (WiFi via the C6, bandwidth limit).
- Licensing/protocol openness: documented, reverse-engineered, or closed.

## Candidate radios to check (UNVERIFIED - the operator named brands, not models)

Find the facts for each; do not rely on this list or on memory:
- ICOM: IC-7300, IC-7610, IC-705, IC-9700 (I/Q over USB? LAN remote? scope data format).
- Yaesu: FTDX101, FT-710, FTDX10 (SDR-style front ends; what they expose).
- Elecraft: KX3 (I/Q jack, analog), K4 (network/ remote, I/Q?).
- Kenwood: TS-590SG / TS-890S (scope, I/Q?).
- FlexRadio: 6000/8000 series (VITA-49 over LAN, SmartSDR API) - the obvious wide one.
- Others worth a line: Hermes/Red Pitaya/ANAN (OpenHPSDR), SDRplay/RTL-SDR (receivers),
  QRP Labs QDX/QMX (the current target, for the baseline), uSDX/truSDX.

## What the QMX path is today (read the code, do not rely on this summary)

- USB host captures I/Q as UAC audio and CAT over CDC-ACM; FFT runs on the Tab5;
  spectrum + waterfall render on the 1280x720 display. Sample rate measured at
  about 48 kHz pairs/s. Pointers: `main/audio/audio.c`, `main/dsp/dsp.c`,
  `main/cat/cat.c`, `main/ft8_*`, `main/ui/ui.c`, `components/m5stack_tab5/`.
- CLAUDE.md and the memory files (see below) hold hard-won hazards that bound any
  design: core 0 CPU budget, internal-RAM `.bss` budget, DMA pool being closed,
  USB host sharing, WiFi vs SD. Read `graphify-out/GRAPH_REPORT.md` first.
- Many QMX-specific behaviours are hard-coded (CAT dialect, `MD6`/`TA` digital TX,
  band guard, QMX power-cycle quirks). The study must list which of these are QMX
  facts and which are general, because that split IS the abstraction.

## Method rules (operator's standing rules - they apply here)

- Never guess. Every radio fact needs a source (manual, protocol document, vendor
  page, or a working open-source implementation such as hamlib, wfview, SparkSDR,
  openHPSDR). Cite it in the document. Mark anything unsourced "UNVERIFIED".
- A radio that gives a scope line but no I/Q is a different class; do not blur them.
- Say where "same or better than QMX" is NOT achievable (e.g. a 48 kHz I/Q cannot
  beat a 3 MHz scope; a LAN-only radio may not fit the Tab5's WiFi).
- Write in plain engineering language (ASD-STE100 style): short sentences, no
  rhetoric, observation -> principle -> conclusion. See `~/.claude/CLAUDE.md`.
- Do not push, tag or release. Do not build or flash. Do not touch a bench.
- Costly web research is fine for this task; use a subagent for per-vendor
  protocol digging if useful, but verify what it returns.

## Suggested structure of the document

1. TL;DR with the honest answer: which radio classes are feasible on the Tab5.
2. Radio classes (A: UAC I/Q like QMX; B: LAN VITA-49 like Flex; C: scope-data only;
   D: analog IF/needs ADC) - and the matrix, one row per model.
3. The abstraction: an `iq_source` interface (start/stop, sample format, centre
   frequency, rate, drop counters) and a `rig_control` interface (CAT dialects).
   Map each to current files; mark what is QMX-specific today.
4. Feature-parity table against QMX-Panadapter (panadapter, waterfall, FT8/FT4/JS8/
   WSPR, CW page, web radio/web audio, spots, band guard, log, OTA, web UI).
5. Resource budget: CPU on core 0, RAM, USB, WiFi bandwidth per class.
6. Phased plan with session estimates and a go/no-go spike per class (cheapest
   decisive instrument first, per operator rules).
7. Risks, ranked. Sources.

## Where the project knowledge lives (this machine)

- `C:\dev\qmx-panadapter\CLAUDE.md` and `TODO.md`.
- Memory index: `C:\Users\Steffen\.claude\projects\C--dev-qmx-panadapter\memory\MEMORY.md`
  (only on the operator's main account; if this session cannot see it, rely on the
  repo: `docs/`, `docs/qmx-reference/`, `graphify-out/GRAPH_REPORT.md`).
- Related studies: `docs/rtty-feasibility.md`, `docs/js8-feasibility.md`,
  `docs/cw-page-design.md`.

## Branch

The operator starts this on a feature branch. Work only under `docs/`. Commit the
study there; do not merge to main without his word.
