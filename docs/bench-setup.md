# Bench setup — four boards, two radios, one antenna, two CPU cores

Standing reference for how this development machine is organised. The registry
that the tooling actually reads is `tools/bench.json`; this file is the
reasoning behind it.

Everything here was measured on 2026-08-23 unless marked otherwise.

---

## 1. The four benches

| Bench | Board | Radio | Console | Tree / branch | Role |
|---|---|---|---|---|---|
| **dev** | Tab5 #1 `30:ED:A0:EA:DD:57` | QMX (small) | COM3 → **COM20** | `qmx-panadapter` / `main` | daily driver, always the newest `main` commit |
| **lab** | Tab5 #3 (new) | **QMX+** | **COM21** | `-wspr` / `-cw` | feature branch of the day, flashed freely |
| **field** | Tab5 #2 `80:F1:B2:D1:45:92` | none | **COM22** | — runs the last **release** | release + OTA verification, user simulator |
| **port** | Waveshare P4 7B | none | COM12 → **COM23** | `-p4` / `feat/board-hal-seam` | multi-board port, private remote |

### Why the units are assigned this way

**Tab5 #2 is the field unit, not the new one.** Its value is provenance: it has
never been USB-flashed, only updated over the air, and that is the entire reason
the v1.9.2 OTA result meant anything. A brand-new unit has to be USB-flashed
once to get onto the OTA ladder anyway, so it starts with no provenance to
protect. Spend the new one; keep the clean one clean. `tools/bench.json` marks
this bench `usb_flash: NO` and `bench flash field` refuses.

**The QMX+ lives on the lab bench**, and the side effect is worth more than the
feature work it was bought for: there is currently **zero QMX+ test coverage**,
while several open field items are QMX+-specific (Samuel W7STF's front-panel
Band-config reboot, the whole 1_04 line). A permanently connected QMX+ makes
those reproducible for the first time. ⚠ A result on QMX+ does **not**
automatically transfer to the small QMX — anything shipped needs one cross-check
on the dev bench.

**field and port borrow a radio** rather than owning one. Most port work is
display/touch and the 1024×600 UI reflow, which needs no radio; the USB-host and
parity phases do. Most release verification is boot/UI/OTA, which needs no
radio; a release that touches radio behaviour does.

---

## 2. The three shared resources

Everything that causes trouble on this bench is one of these three being used by
two people at once. Two of them are invisible in software, which is why they get
a lock file and a doc rather than good intentions.

### 2.1 CPU — one build at a time, all four trees

The machine is an **i7-7600U: 2 physical cores, 4 threads**. One `idf.py build`
already saturates it. Two concurrent builds put ~8 compile jobs on 2 cores, so
both slow by more than 2× and any bystander process starves — which is what
stalled `make_flasher_zip.ps1` for 3+ minutes during the v1.9.2 release. That was
originally written up as the pinned IDF Python environment not being safe for
concurrent invocation from different worktrees. **That explanation was wrong.**
Nothing was locking; it was queueing.

So serialising builds is not a workaround for an IDF limitation, it is correct
scheduling on a dual-core machine. `bench build` and `bench flash` take
`C:/dev/bench.lock` so the wait is explicit instead of a mystery.

Already in place and not worth re-litigating: **ccache is enabled and healthy** —
292k calls at an **87.9% hit rate**. Raise its ceiling from 5 GiB to ~15 GiB now
that four divergent trees share it, or they start evicting each other.

Not verified: whether Windows Defender exclusions are set for the IDF tree,
`~/.espressif` and the build dirs. `Get-MpPreference` needs admin. Typically the
single biggest Windows IDF speed-up — worth confirming.

### 2.2 Antenna — one, and invisible in software

One 20 m antenna, two radios, a 2-way switch, with the spare position on a dummy
load so a TX on the off-antenna bench is safe.

**Only one bench is on air at a time**, and the consequence is a daily version of
a trap that used to be occasional: **decode counts, SNR and noise floor from the
antenna-less bench are meaningless.** A lab bench showing zero WSPR decodes may
only mean the dev bench holds the antenna.

Two mitigations, both cheap: label the switch positions with the bench names, and
record the holder with `bench antenna <name>` (stored in `C:/dev/bench.antenna`,
shown by `bench list`). Ask which bench is on the antenna before believing any
receive measurement — including your own from ten minutes ago.

The dummy load also unblocks two long-standing shipped-but-unverified items:
**#103 SWR protection** and the **Antenna Tune W/SWR figures + prior-mode exit**
(§5 of `qmx-1_04-cat-comparison.md`). Both have been waiting on "a dummy load and
a deliberate mismatch".

### 2.3 Patch state — shared vs per-tree, and the difference matters

All four trees build against the **one** pinned IDF at `C:/esp/v5.4.4`.
(`v5.3.5` and `v5.5.4` also exist on this machine but neither project uses them.
The p4 clone inherited the same v5.4.4 pin, so the Waveshare BSP's preference for
IDF ≥5.5 is a *future* tension, not a current one.)

The nine standing patch scripts in `tools/patches/` fall into two groups, and
they are maintained completely differently:

**Six edit the shared IDF tree** — applied **once per machine**, and all four
trees inherit them:

| Script | Fixes |
|---|---|
| `apply_fatfs_exfat.ps1` | exFAT hardcoded off, no Kconfig |
| `apply_hcd_bulk_error_recovery.ps1` | #4 — bulk-error assert → reboot |
| `apply_hub_recover_tolerant.ps1` | #5 — `hcd_port_recover` in `ESP_ERROR_CHECK` |
| `apply_usb_dwc_hal_chan_error_tolerant.ps1` | #7 — channel error without CHHLTD |
| `apply_hcd_buffer_parse_error_tolerant.ps1` | #8 — "impossible" pipe event |
| `apply_httpd_ws_dead_socket_close.ps1` | dead WS socket not closed |

⚠ **One IDF reinstall silently breaks all four benches at once.**

**Three edit `managed_components/`** — per working tree, git-ignored, and wiped
by `fullclean`, any dependency change, and the release process's
`rm -r managed_components/`:

| Script | Fixes |
|---|---|
| `apply_esp_hosted_psram.ps1` | WiFi transport DMA pool → PSRAM |
| `apply_esp_hosted_sdio_recovery.ps1` | SDIO RX oversize drain + TX retry |
| `apply_cdc_acm_close_tolerant.ps1` | #6 — `cdc_acm_host_close()` abort |

These must be applied **four times, once per tree**, and re-applied per tree
after any of those events.

`tools/check_patches.py` reads `IDF_PATH` from the environment, so it validates
whichever tree is active. That is right as designed — leave it alone.

---

## 3. Ports and identity

**The COM number follows the socket, not the board.** All four boards enumerate
as `VID_303A&PID_1001` with **no serial number** — the device instance IDs are
port-path-derived. So Windows assigns COM by physical port, and a board moved to
a different socket becomes a different COM.

That is good news for a fixed bench: **one labelled physical port per bench, and
the numbers are permanent.**

### The renumber

The COM space on this machine is crowded and has drifted — memory recorded COM4
and COM6 as FlexRadio virtual ports; today the FlexRadio VSPs are gone, **COM6 is
a remembered Espressif device** and COM4 a remembered STMicro one. Rather than
inherit that, assign fresh numbers:

```
COM20  dev     COM21  lab     COM22  field     COM23  port
```

Procedure, per board, when you physically build the bench out:

1. Plug the board into its labelled hub port.
2. Device Manager → Ports → the new device → Properties → Port Settings →
   Advanced → COM Port Number → set it.
3. Update that bench's `com` field in `tools/bench.json`. Nothing else needs
   changing — the tooling reads the registry.

Do them one at a time, and **don't renumber the dev bench until last**: the
running capture and the old `qmx` helper both hold COM3, and there is no reason
to break a working bench before the new ones are up.

### The rules that don't change

- **Never auto-detect a flash port.** `idf.py flash` with no `-p` has already
  cost this bench one board: the Waveshare took a Tab5 binary to 94% and
  boot-looped, because `VID_303A` was read as "must be the Tab5".
- **Never identify a board by COM number alone.** After every flash, verify
  against the MAC the firmware prints in its own boot header
  (`serial(MAC)=…`). `bench flash` does this automatically; `bench verify
  <name>` re-runs it.
- **The QMX plugs directly into its Tab5's USB-A.** Never through a hub — that
  is the Transaction Translator wall (IDF 5.4.4 has no TT support, so every
  FS/LS device behind a HS hub gets disabled).
- A powered hub is for the four **console** links only.

---

## 4. Serial captures

One standing capture per bench, named for the bench, never timeboxed:

```
scratchpad/capture-dev.txt   capture-lab.txt   capture-field.txt   capture-port.txt
```

`bench capture <name>` starts one correctly (`tools/cap_serial_reboot.ps1`, **no
`-Reset`**, a 10-year deadline). The only legitimate reason to stop one is a
flash, and `bench flash` restarts it in a `finally` block — because the crash you
care about lands in the window where nobody was watching.

Before quoting anything from a capture, check the process is alive **and** that
the file's `LastWriteTime` is within seconds of now. An expired capture looks
exactly like a quiet, healthy device.

⚠ Historical note: until 2026-08-23 the standing capture covered the **dev**
bench but was named `wspr_standing.txt` and lived in the wspr worktree, because
that is what was being worked on when it started. Name captures for the board
they watch, not the feature.

---

## 5. Bringing the two new units online

- [ ] Mount the 2-way antenna switch; dummy load on the spare position; label
      both positions with bench names.
- [ ] Label the hub ports.
- [ ] Plug in Tab5 #3 → renumber to COM21 → record its MAC in `bench.json`
      (`bench verify lab` prints it after the first boot) → `bench capture lab`.
- [ ] Plug in Tab5 #2 → renumber to COM22 → record IP → **do not flash it**.
- [ ] Renumber the Waveshare to COM23 next time it is connected.
- [ ] Renumber dev to COM20 **last**, then update `bench.json`.
- [ ] Raise the ccache ceiling to ~15 GiB.
- [ ] Check the Defender exclusions (needs an admin shell).
- [ ] Apply the three `managed_components` patches in each of the four trees.

---

## 6. Planned: this all moves to the shack machine

Decided 2026-08-23, not yet done. The bench moves off this laptop onto a
**Ryzen 7 5800H** (8C/16T Zen 3, 4.4 GHz boost, 16 MB unified L3, 45 W, 64 GB
RAM, ~1.5 TB free) which lives in the shack and also runs PixInsight. The
laptop is released; a Ryzen 5 3550H takes over office duty.

**Why it is the right call:** the bench described in this document wants a
machine that never moves, is always capturing, and sits where the antenna is.
Build speed is the bonus, not the reason — expect **3.5–5× on clean/release
builds** and **~1.3–1.5×** on the everyday incremental loop, which stays bound
by link and the Python steps. It will *not* speed up the things that actually
gate this project: flash-boot-observe turnaround, a 15-second FT8 slot, or an
overnight soak.

**What changes in this document once it lands:**
- §2.1 is rewritten. On 8C/16T the **CPU rationale for the build lock
  disappears** — two builds at six jobs each is comfortable. Split the lock:
  let `bench build` run free, and keep it only on `bench flash` (one port, one
  board) and the antenna. That is a small edit to `bench.ps1`.
- The COM renumbering is per-machine and must be redone. Do it as part of the
  physical build-out, not twice.

**Migration notes:**
- ~30 GB to move: `C:\Espressif` 25.5 GB, `C:\esp` 1.6 GB, `C:\dev` 2.4 GB.
- **Keep the paths byte-identical.** Then `bench.json`, every patch script and
  all documented commands carry over untouched — and the ccache directory has a
  good chance of keeping its hit rate, since direct mode hashes the compiler
  path. *Likely, not certain* — copy it and check `--show-stats` after the
  first build rather than assuming.
- Re-apply the six shared-IDF patches (`check_patches.py` fails the build if
  you forget) and the three `managed_components` patches × four trees.
- Clone the private `qmx-panadapter-p4` repo separately.
- Don't judge it on day one: a cold ccache makes the first builds all-miss.

⚠ **PixInsight shares the machine**, and both hobbies run unattended overnight —
a WBPP run and an FT8 soak will collide. Steady-state capture is free (~40 B/s),
so the risk is narrow: a **burst** (boot log, decode storm, panic dump) at tens
of KB while the CPU is saturated. `cap_serial_reboot.ps1` therefore runs
`AboveNormal` with a 256 KB read buffer. A panic dump is unrepeatable; that is
the one thing this capture cannot afford to drop.

---

## 7. Quick reference

```
bench list                 what exists, what is plugged in, what is running
bench status <name>        ask the running firmware over the network
bench capture <name>       start the standing serial capture
bench build [<name>]       build that bench's tree, under the lock
bench flash <name>         lock, stop capture, flash, restart capture, verify
bench verify <name>        re-check the MAC in the latest boot header
bench antenna [<name>]     show / record which bench is on the antenna
bench who | lock | unlock  manual lock control
```
