# JS8 mode — feasibility & implementation plan

Scoping pass for adding JS8 ("Normal" submode) as a second digital mode alongside FT8.
Based on reading the actual JS8Call 2.3.1 source (`js8call-2.3.1.zip`, 2025-06-27 release)
against this project's `components/ft8_lib` (kgoba/ft8_lib) and the existing `main/ft8_*`
app layer. No code written yet — this is the "is it worth it, and how big" answer.

## TL;DR

JS8's **Normal submode** is, at the physical/timing layer, FT8 with a different Costas
array and a different LDPC code — same sample rate, same symbol rate, same frame length,
same 15 s TX cycle. `ft8_lib` already has a protocol-abstraction layer (it supports FT4
alongside FT8 today), so adding JS8 follows an existing pattern rather than requiring a
parallel pipeline. The genuinely new work is: one new LDPC table set, a 12-bit CRC, and
~230 lines of message pack/unpack for two JS8 frame types — both now fully spec'd below.

**Estimate: ~5–7 work sessions** (sized against this project's actual FT8 delivery,
`v0.10.0-beta1` → `v0.15.1`, 2026-06-05 → 2026-06-11). Roughly the size of two FT8
mid-version bumps (e.g. v0.13.0 + v0.14.0 combined), not a rebuild.

JS8Call's "Fast/Turbo/Slow/Ultra" submodes, compound callsigns, multi-frame Huffman
free-text chat, and the heartbeat/relay network are explicitly **out of scope** — see
[Out of scope](#out-of-scope-tier-2).

---

## Physical layer: JS8 Normal ≈ FT8 with different tables

| Parameter | FT8 (`ft8_lib`, this project) | JS8 Normal (submode A) |
|---|---|---|
| Sample rate | 12000 Hz | 12000 Hz |
| Samples/symbol (`NSPS`) | 1920 (160 ms, 6.25 Hz tone spacing) | 1920 — **identical** |
| Frame structure | 79 symbols = 21 sync + 58 data | 79 symbols = 21 sync + 58 data — **identical** |
| Sync layout | 3×7 Costas at symbols 0, 36, 72 | 3×7 Costas at symbols 0, 36, 73 — **same layout** |
| Costas array | `{3,1,4,0,6,5,2}` (`kFT8_Costas_pattern`) | `{4,2,5,6,1,3,0}` (NCOSTAS=1, "original") — different permutation, same array used for all 3 sync blocks |
| TX cycle | 15 s | 15 s — **identical** |
| LDPC code | (174,91), M=83 (`FTX_LDPC_*`) | (174,87), M=87 — rate exactly 1/2, **different code** |
| CRC | 14-bit | 12-bit |
| Message framing | i3/n3 fixed-position type field (`ftx_message_type_t`) | 3-bit `itype` (TransmissionType) + 3-bit `FrameType` header |

JS8's other submodes (Fast=B, Turbo=C, Slow=E, Ultra=I) use a "MODIFIED" Costas type
(3 *different* 7-arrays per `genjs8.f90`) and different `NSPS`/timing — each is its own
physical-layer profile. Not pursued here.

### Architectural precedent: `ft8_lib` is already multi-protocol

`decode.h`/`decode.c` already define `ftx_protocol_t` and branch on `wf->protocol` for
sync function, Costas table, `num_tones`, Gray map, and an FT4-only XOR sequence
([decode.c:192](../components/ft8_lib/ft8/decode.c)). Adding `FTX_PROTOCOL_JS8` is a
third branch in an existing pattern, not a new abstraction.

`ldpc.c`'s sum-product loops are generic over `FTX_LDPC_M`/`FTX_LDPC_N` and the
`kFTX_LDPC_Nm`/`Mn`/`Num_rows`/`generator` tables — they don't hardcode FT8's (174,91)
shape beyond those constants/tables. A second table set (`kJS8_LDPC_*`, M=87) plus a
protocol switch is mechanical.

JS8's decode pipeline (`js8dec.f90`) is: `syncjs8` → `bpdecode174` (LDPC belief
propagation, same algorithm family as `ldpc.c`) → optional `osd174` fallback for weak
signals → CRC-12 check → `extractmessage174` (unpack). OSD is a second-pass sensitivity
boost JS8Call uses when plain BP fails — `ft8_lib` doesn't implement OSD, and skipping it
just means JS8-here ≈ same sensitivity tier as FT8-here already gets. Not blocking.

---

## Reuse map

| Layer | Verdict | Notes |
|---|---|---|
| Audio capture / ring buffer (`audio.c`, `dsp_ft8_capture`) | **Reuse unchanged** | Sample rate & frame timing identical |
| v0.15.1 UTC-boundary capture-window anchoring | **Reuse unchanged** | JS8 Normal's 15 s cycle matches FT8's exactly |
| `decode.c` FFT / candidate search / downsample | **Reuse, +1 protocol branch** | Same NSPS/NN/NS/ND; needs JS8 Costas array as 3rd case (FT4 precedent) |
| `ldpc.c` sum-product engine | **Reuse engine, new tables** | New `kJS8_LDPC_*` (174,87) tables from `ldpc_174_87_params.f90` |
| `crc.c` | **Add CRC-12 variant** | Alongside existing CRC-14 |
| `encode.c` GFSK tone synth | **Reuse, +1 protocol branch** | Same waveform shape, different tone table |
| `ft8_tx.c` CAT burst (`TA<freq>;` @ 160 ms, 6.25 Hz/tone) | **Reuse unchanged** | Tone spacing/cadence identical |
| `ft8_test.c` slot loop | **Reuse, +protocol selector** | 15 s cycle unchanged |
| `ft8_qso.c` QSO state machine | **Reuse, ladder adapted** | See [QSO ladder mapping](#qso-ladder-mapping) |
| `ft8_screen*.c`, `ft8_tx_modal.c`, `ft8_status.c` | **Reuse, label/vocab tweaks** | Decode list, row selection, CQ-filter, TX modal all reusable as-is |
| Message pack/unpack | **New** (~230 lines) | Fully spec'd below; `pack28`/`unpack28`-style 28-bit callsign codec likely portable from `ft8_lib/ft8/message.c` |

---

## Message formats (fully spec'd)

JS8's 87-bit LDPC payload (`KK=87`) = 75-bit "message" + 12-bit CRC. The 75-bit message =
3-bit `itype` (TransmissionType: 0 = standalone frame, the only case needed here) + a
72-bit Varicode frame. Two frame types cover a complete CQ→exchange→sign-off QSO:

### `FrameDirected` (0b011) — report exchange / sign-off

```
75 bits = itype(3)=000 + FrameType(3)=011 + from_call(28) + to_call(28) + cmd(5)
          + portable_from(1) + portable_to(1) + num(6)
```

- `from_call`/`to_call`: 28-bit callsign, packed via `Varicode::packCallsign` — base
  conversion `mod 27 / mod 27 / mod 27 / mod 10 / mod 36`, `/P` stripped into the
  separate `portable_*` bit, special-case rewrites for 3DA0 (Swaziland) and 3X
  (Guinea) prefixes, small `basecalls` lookup for tokens. **Structurally identical**
  to `ft8_lib`'s existing `pack28`/`unpack28`
  ([message.c:810](../components/ft8_lib/ft8/message.c),
  [message.c:870](../components/ft8_lib/ft8/message.c)) — likely a near-port.
- `cmd` (5 bits, 0–31): index into the `directed_cmds` vocabulary (below).
- `num` (6 bits): `packNum` clamps to `-30..+31`, stores as `+31` offset (`1..62`).
  Used for signal reports via `formatSNR`.

### `FrameHeartbeat` (0b000) — CQ / HB beacon (carries grid)

```
75 bits = itype(3)=000 + FrameType(3)=000 + callsign(50) + extra_hi(11)
          + extra_lo(5) + bits3(3)
        ( extra = extra_hi<<5 | extra_lo, 16 bits total )
        ( extra: bit15 = isAlt (0=HB, 1=CQ); bits 0-14 = packGrid(grid) )
        ( bits3: index into `cqs` if isAlt=1, else `hbs` )
```

- `callsign`: **50-bit** `packAlphaNumeric50` — different codec from Directed frames.
  Pads/splits into an 11-char field with optional `/` at positions 3 and 7 (compound
  call support), then mixed-radix base-38 encode over a shared 38-char `alphanumeric`
  alphabet (digits + A-Z + space + 1 more). For a plain 6-char call this is just
  `"K1A BC     "` → base-38 — no special-casing for standard calls. New code, but
  mechanical (~50 lines), symmetric pack/unpack.
- `packGrid`/`unpackGrid` (15 bits used of 16): `((ilong+180)/2)*180 + (ilat+90)` —
  ~10 lines, already fully read.
- `cqs` (bits3 when isAlt=1): `0=CQ CQ CQ, 1=CQ DX, 2=CQ QRP, 3=CQ CONTEST, 4=CQ FIELD,
  5=CQ FD, 6=CQ CQ, 7=CQ`. For a CQ-run analog of the current FT8 behavior, `cqs[0]`
  ("CQ CQ CQ") is the natural default.

### `directed_cmds` vocabulary (5-bit, 32 slots — relevant subset)

| Code | Token | Meaning |
|---|---|---|
| 21 | `RR` | roger roger |
| 25 | `SNR` | signal report (uses the 6-bit `num` field) |
| 28 | `73` | best regards / end of contact |
| 26 / 27 | `NO` / `YES` | confirm / deny |
| 0 | `SNR?` / `?` | query signal report |
| 4 / 15 | `GRID?` / `GRID` | query / state grid (not used in the ladder below — grid travels only via `FrameHeartbeat`) |

**No combined "RR73" token** — JS8's closest equivalents are `RR`(21) and `73`(28) as
separate codes.

---

## QSO ladder mapping

JS8's ladder is **shorter** than FT8's (no separate grid-then-report step), which helps
`ft8_qso.c` reuse:

| Step | FT8 (current `ft8_qso.c`) | JS8 equivalent |
|---|---|---|
| CQ | `CQ CQ <me> <grid>` | `FrameHeartbeat`, isAlt=1, cqs=0 ("CQ CQ CQ") — `<me>` + `<grid>` |
| Reply / our report | `TX1 <them><me><grid>` then `TX2 R<rpt>` (2 steps) | `FrameDirected` cmd=`SNR`, num=rpt — **1 step** |
| Their report back | `R<rpt>` | `FrameDirected` cmd=`SNR`, num=rpt |
| Sign-off | `RR73` then `73` | `FrameDirected` cmd=`73` (and/or `RR`) |

Net effect on `ft8_qso.c`: CQ uses a different frame type (`FrameHeartbeat`, with grid)
than the three exchange messages (`FrameDirected`, no grid) — two pack/unpack pairs
needed, but the WAIT_RPT/WAIT_RR73/WAIT_DONE state shape carries over. `fmt_report`
needs its clamp range adjusted to JS8's `-30..+31` (vs FT8's wider range).

---

## Out of scope (Tier 2)

JS8Call's actual size (113 KB `JS8.cpp`, 69 KB `varicode.cpp`) is dominated by features
that don't fit a panadapter's FT8-style UI:

- **Compound callsigns** (`FrameCompound`/`FrameCompoundDirected`) — non-standard calls
  like `PJ4/W1ABC` needing the 50-bit codec in Directed-style exchanges too.
- **Multi-frame free-text chat** (`FrameData`/`FrameDataCompressed`) — Huffman/Varicode
  alphabet across consecutive frames with continuation bits. This is JS8's "killer
  feature" but is a chat-client UX, not a QSO-ladder UX.
- **Heartbeat/relay network, store-and-forward, APRS gateway.**
- **Fast/Turbo/Slow/Ultra submodes** — separate physical-layer profiles (different
  `NSPS`, "MODIFIED" Costas arrays).
- **OSD174 second-pass decoding** — sensitivity boost beyond plain LDPC BP.

---

## Phased plan & effort estimates

Sized against this project's actual FT8 delivery arc (`v0.10.0-beta1` → `v0.15.1`,
2026-06-05 → 2026-06-11: 6 calendar days / ~6 version bumps, ~6500 lines across
codec+app+UI, including a fair amount of unrelated panadapter UI work).

| Phase | Scope | FT8 precedent | Estimate |
|---|---|---|---|
| **J1 — Codec core** | Convert `ldpc_174_87_params.f90` → C tables (`kJS8_LDPC_*`); add CRC-12; add JS8 Costas array; add `FTX_PROTOCOL_JS8` to decode.c/encode.c/constants (mirrors existing FT4 branch); synthetic encode→decode round-trip self-test | `86f7638`/`0615d6f`/`d0ee6b2` (vendor + prune + self-test, v0.10.0-beta1) | **1 session** (+1 if LDPC tables don't converge first try) |
| **J2 — Message protocol** | New `js8_message.c`: `FrameDirected` + `FrameHeartbeat` pack/unpack (~230 lines total); shared `alphanumeric` alphabet, `packGrid`/`unpackGrid`, `packAlphaNumeric50`/`unpackAlphaNumeric50`; port/adapt `pack28`/`unpack28` for the 28-bit callsign codec | new — analog of `message.c`'s STANDARD-type pack77/unpack77 | **1 session** — fully spec'd, no remaining design risk |
| **J3 — DSP/TX wiring** | Thread a `protocol` selector through `dsp_ft8_capture()`, `ft8_test.c` slot loop, `ft8_tx.c` GFSK+CAT burst | `e0a2e3f`/`ce13993`/`5ec9abe` (capture path → slot loop) | **0.5–1 session** |
| **J4 — QSO state machine** | Adapt `ft8_qso.c` ladder to Heartbeat-CQ + Directed-SNR/RR/73; adjust `fmt_report` range | `e94c5c3` (v0.13.0 state machine) | **1 session** |
| **J5 — UI** | Mode toggle (FT8/JS8) in settings drawer; label/vocabulary tweaks in `ft8_screen_view.c`/`ft8_tx_modal.c`/`ft8_status.c`. Decode list, row selection, CQ-filter reusable as-is | `6d4c1b8`/`cc7ce99`/`9d81ff6` (mode-toggle infra) | **0.5 session** |
| **J6 — On-air validation** | Needs a real JS8Call counterpart; debug LDPC/Costas/bit-order against a live reference. CQ-run/patient-retry timeouts likely reusable unchanged (same 15 s slots) | `v0.15.0`→`v0.15.1` (CQ-run + capture-window bugfix) | **1–2 sessions** |

**Total: ~5–7 sessions.**

---

## Risks (in order)

1. **LDPC(174,87) table transcription** (J1) — a transcription error looks identical to
   "never decodes," indistinguishable from a sync/Costas bug without a known-good
   reference codeword. Mitigate: hand-verify one JS8 test vector (JS8Call/`lib/js8`
   likely has a testmsg generator analogous to `ft8_testmsg.f90`) bit-exactly through
   encode→LDPC before touching the radio.
2. **On-air interop** (J6) — no second JS8 station to validate against today; needs a
   phone/PC running JS8Call on the same antenna or fed via SDR loopback.
3. ~~`FrameHeartbeat` layout~~ — resolved, see above.

---

## Source references

- JS8Call 2.3.1 source: `js8call-2.3.1.zip` (not in this repo — was extracted to a temp
  dir for this analysis). Key files: `varicode.{h,cpp}`, `JS8Submode.{hpp,cpp}`,
  `commons.h`, `lib/js8/*.f90`, `lib/ft8/ldpc_174_87_params.f90`, `lib/js8a_decode.f90`.
- This project: [`components/ft8_lib/ft8/`](../components/ft8_lib/ft8/) (constants.h,
  decode.{h,c}, encode.{h,c}, ldpc.{h,c}, message.{h,c}, crc.{h,c}), and the app layer in
  [`main/ft8_qso.c`](../main/ft8_qso.c), [`main/ft8_test.c`](../main/ft8_test.c),
  [`main/ft8_tx.c`](../main/ft8_tx.c).
