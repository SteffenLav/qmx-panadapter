# QSO log: fixed-size on the device, older records on SD

Design and risk review, 2026-11-13. **No code written yet.** Written before
implementation deliberately: the previous attempt at this problem
(`wip/adif-sd-archive`) was coded first and the review then found eight risks,
three of which were real defects.

Decided with the operator after Gyula HA3HZ described the same model on
groups.io: *"log applications keep only the last few hundred QSOs visible and
the rest in storage"*.

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

## Open: worked-before

This is the part that needs measurement, not an opinion. The grey "worked
before" marking reads a cache of unique (call, band) pairs built at boot from
the device log. Archived calls must still count, or a station worked last year
reads as new.

Two candidates:

1. **Read the archive at boot.** Simple, and the parked branch did it. The
   cost grows forever: at 5,000 archived records it is an SD read plus
   ~25M uint32 compares in `cache_add`. `ADIF_WORKED_MAX` (8,192) would also
   have to rise.
2. **A worked-index on the card** - one short fixed-size record per unique
   (call, band), appended when first seen. ~22 B per pair, so 5,000 pairs is
   110 KB and the boot read is bounded and small.

Start with (1) because it is a tenth of the code, **but measure the boot cost
on bench1 with a large archive before shipping it**. (2) is the fallback and
the design must not make it hard to add.

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
