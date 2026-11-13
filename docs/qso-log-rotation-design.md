# QSO log: fixed-size on the device, older records on SD

Design and risk review, 2026-11-13. **No code written yet.** Written before
implementation deliberately: the previous attempt at this problem
(`wip/adif-sd-archive`) was coded first and the review then found eight risks,
three of which were real defects.

Decided with the operator after Gyula HA3HZ described the same model on
groups.io: *"log applications keep only the last few hundred QSOs visible and
the rest in storage"*.

## Status 2026-11-13, later - core + harness done, firmware NOT wired

- `main/adif/adif_rotate.c` is the rotation core, portable, behind `rot_io_t`.
  Not in `main/CMakeLists.txt` yet. `tools/spiffs_host/rot.c` runs it with a
  power cut at every flash write: see that README's table. Rotation logic
  passes; 0.5-0.9 % of cuts lose qso.adi to SPIFFS itself.
- ⛔ The harness BASELINE (no rotation) loses qso.adi in ~9-10 % of power cuts
  landing in any SPIFFS write - boot `SPIFFS_check` deletes it. Waiting on the
  operator: does that change the plan before the firmware is wired?
- Decided while building (in the code's comments): cap 128 KB, rotate down to
  96 KB; the worked index takes archive pairs at rotation, not per QSO (the
  device log supplies its own pairs; a per-QSO SD write is #153 risk); upload
  cursors are shifted from the header's ARCHIVED count against a base in NVS,
  cursors written before the base, so a cut gives re-uploads, never skips.
- Found, not fixed: an upload in flight while a delete runs writes an absolute
  cursor computed before the delete shifted the records. Pre-existing.

## Status and where to start — 2026-11-13 (morning, superseded above)

**The design below is settled. Nothing of it is implemented.** Start here.

State of `main` (11 commits, **none pushed**, working tree clean):

| commit | what |
|---|---|
| `c8e1c55a` | the safety fix this builds on - every SPIFFS write admitted, no `truncate` |
| `35a9b0c5`, `82584acd` | T198: 19 KB of `.bss` to PSRAM, DMA pool back |
| `dd22a846` | `tools/dma_history.py` |
| `8f66ebf0`, `f28ed852` | this document |

First steps, in order:

1. Read `main/util/spiffs_guard.h` and `tools/spiffs_host/README.md` before
   writing a line. Every rule below came out of that harness rejecting three
   earlier designs.
2. Add the rotation to the harness FIRST (`tools/spiffs_host/sim.c`, a new
   mode): rotation at the cap, an interrupted rotation, and a full partition
   during the device-log rewrite, with the per-operation integrity check
   `STEP=1` already uses. The pass bar is the T197 one: zero corrupt logs,
   zero writes failing after a passed reserve check.
3. Only then the firmware.

⛔ Non-negotiable, each measured rather than reasoned:

- Never `truncate()` on SPIFFS. It reports success and the next append breaks
  files over ~330 KB. Copy to a temp file and rename.
- No SPIFFS write without `spiffs_guard_reserve()` under `spiffs_guard_take()`.
- Never report a QSO as logged unless `adif_log_record()` returned true.

Bench and safety:

- bench1 runs `35a9b0c5`, the release configuration. `ADIF_BENCH_FILL` is 0 and
  must stay 0 in any commit.
- Flashing bench1 costs the operator a QMX power cycle. Say so before flashing,
  and bundle flashes.
- bench1's QSO log is backed up at
  `scratchpad/bench1-qso-backup-20261113.adi`, sha256 `ec73cd98…84bb21`,
  36 records. Compare against it after any test that writes the log.
- ⛔ No release without the operator saying "wrap-up".

⚠ `82584acd` is **not flashed**: bundle it with the first bench1 flash and read
`app_main entry` and T+600s with `tools/dma_history.py`.

Still open elsewhere, not blocking this: a reply to Randy N4OPI is drafted but unsent at
`scratchpad/reply_randy_n4opi_logging_20261113.md`.

## The problem this solves

`/spiffs/qso.adi` lives on a 1 MB SPIFFS partition shared with the rolling
diagnostic log and the LoTW key. Measured on the real SPIFFS code
(`tools/spiffs_host`): writes start being refused with the device log at
405-459 KB, about 2,300 QSOs. Randy N4OPI reached it on 2026-11-12.

`c8e1c55a` made that safe - a refused QSO is reported, never lost silently,
and the file can no longer be damaged - but the ceiling is still there, and the
only way past it is "download the log, then clear it" by hand.

## The model

The device keeps a **fixed-size** log. Everything older lives on the card.

```
/spiffs/qso.adi                            the newest records, capped
/sdcard/qmx-panadapter/qso-archive.adi     everything rotated off, append-only
/sdcard/qmx-panadapter/qso.adi             archive + device log = the whole log
```

The card's `qso.adi` keeps its present meaning - the complete log, for anyone
who takes the card to a PC - so nothing changes for existing users or for
"Restore from SD".

## Decisions, with the reason

**Cap: 128 KB, about 700 QSOs.** Not a round number chosen by taste:

- the measured wall is 405-459 KB, so this stays at under a third of it and
  the firmware never enters the region where anything went wrong
- boot load is ~0.38 ms/record (2,715 records in 1,028 ms, Randy's log), so
  700 records costs ~0.27 s instead of ~1 s
- it is "the last few hundred QSOs" Gyula asked for

**Rotation runs at boot only**, in the window the boot mirror already uses
successfully (Randy's log: 512 KB mirrored at 12.0 s). Not after each QSO:
an SD write at an arbitrary moment with WiFi up is what trips the #153 EIO
wedge, and that is not a risk worth taking to save a reboot. If the operator
never reboots, the log simply grows past the cap - harmless, because the wall
is three times further away and `spiffs_guard` still protects.

**Order: append to the archive, verify its size, then rewrite the device log.**
A failure or power cut between the two leaves records in BOTH files, never in
neither. The next boot finds them already in the archive and finishes the move.

**The device log rewrite is copy + rename, never `truncate`** - measured to
corrupt files over ~330 KB (`tools/spiffs_host/trunc.c`) - and goes through
`spiffs_guard` like every other SPIFFS write.

**Never rotate a record a configured upload has not sent.** The QRZ, eQSL,
LoTW and Cloudlog cursors are indices into the device log; rotating N records
shifts all four down by N. The rotation count is therefore
`min(what the cap needs, what every configured upload has sent)`. If an upload
lags, the log grows past the cap and says so. A large log is a far smaller
problem than QSOs that never reach a logbook.

## Settled: worked-before reads an index, not the archive

Measured, not argued (`tools/spiffs_host/workedbench.c`, the real
`cache_add`/`call_hash` from `adif_log.c`):

| unique pairs | today's scan on insert | append to a unique index |
|---|---|---|
| 5,000 | 7.0 ms | 0.67 ms |
| 10,000 | 25.3 ms | 1.67 ms |
| 20,000 | 97.0 ms | 3.67 ms |
| 40,000 | 393.0 ms | 8.33 ms |

`cache_add()` scans every pair already held, so rebuilding from the archive is
quadratic: four times the cost for each doubling. Appending to an index that is
already unique needs no scan and is linear.

The I/O side says the same thing more loudly, and it is the larger term. The
one device figure that exists - 2,715 records loaded in 1,028 ms on N4OPI's
unit - is **not** this loop: the identical work takes 2.2 ms on the host, and
three orders of magnitude is not a CPU difference. Boot cost here is dominated
by reading and parsing the file, and the archive is the bigger file by a factor
of seven:

| unique pairs | archive to re-read | index to read |
|---|---|---|
| 10,000 | 1.79 MB | 254 KB |
| 40,000 | 7.17 MB | 1016 KB |

**So: `/sdcard/qmx-panadapter/qso-worked.idx`**, one fixed 26-byte record per
unique (call, band), appended the first time a pair is seen. Read at boot
straight into the cache with no deduplication pass. The archive itself is never
re-read at boot.

Consequences to carry into the implementation:

- `ADIF_WORKED_MAX` (8,192) becomes the binding limit long before storage does
  and must rise; at 26 B in PSRAM, 40,000 pairs is about 1 MB.
- The index is derived data. If it is missing or its header does not match the
  archive, rebuild it from the archive once and say so in the log - slow, but
  only on that boot.
- A pair must be appended to the index when it is first logged, not only at
  rotation, or a station worked and then rotated would be missing from it.

## Risks, and what each is answered with

| Risk | Answer |
|---|---|
| SD write trips the #153 WiFi wedge | boot window only, where the mirror already works |
| DMA pool too low for SD (T198) | `35a9b0c5` restored it to 5,695 B / 4,864 B largest block with the radio streaming; SD I/O uses `sd_io_buf.h` aligned buffers |
| Power cut mid-rotation | archive first, verify, then rewrite: duplicates, never loss |
| Card swapped for another station's | device log header carries `APP_QMXPAN_ARCHIVED`; if it disagrees with the archive's record count, warn and do not rotate |
| No card | no rotation; cap not enforced; `spiffs_guard` + the 300 KB warning behave exactly as they do today |
| Card removed between sessions | the card's `qso.adi` still holds everything it had; the device log is untouched |
| Archived records cannot be edited or deleted on the device | accepted and documented - the card's file is editable on a PC |
| Boot slower | measure it; that is what the worked-before choice turns on |
| The count the operator sees | device + archived, from the header when there is no card, so it never appears to drop |

## What must pass before it ships

1. `tools/spiffs_host`: rotation at the cap, interrupted rotation, full
   partition during the device-log rewrite, with the per-operation integrity
   check already used for T197.
2. bench1 with a real card: rotation happens, counts add up, `qso.adi` on the
   card still equals archive + device log byte for byte, boot time measured
   with a large archive.
3. A controlled before/after of the worked-before marking for an archived
   callsign.

## Not in this change

Archived records stay read-only on the device. No new UI beyond the count and
a line saying how many are on the card.
