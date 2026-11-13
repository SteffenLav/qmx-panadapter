# SPIFFS host replay

Runs the real IDF v5.4.4 SPIFFS sources on the PC, with the Tab5 `storage`
partition's configuration (1 MB, 4 KB blocks, 256 B pages, write cache on,
`CONFIG_SPIFFS_GC_MAX_RUNS=32`), under the firmware's write pattern: the diag
persist task, QSO appends, reboots (mount, gc 64 KB, check).

It is the evidence behind `main/util/spiffs_guard.h` and the "never truncate"
rule in `main/adif/adif_log.c`. Re-run it before changing how anything writes
to SPIFFS.

## Build

```bash
S=/c/esp/v5.4.4/esp-idf/components/spiffs/spiffs/src
gcc -O2 -w -Iinc -I/c/esp/v5.4.4/esp-idf/components/spiffs/include -I$S -o sim.exe sim.c $S/*.c
gcc -O2 -w -Iinc -I/c/esp/v5.4.4/esp-idf/components/spiffs/include -I$S -o trunc.exe trunc.c $S/*.c
```

## Run

`sim.exe MODE ticks_per_qso boot_every_ticks attempts diag_bytes_per_tick compact yield_at`

- MODE 0: v1.26.2 append (fsync result ignored)
- MODE 4: checked commit only
- MODE 6: reserve check (`SPIFFS_gc(len)`) before every append
- MODE 7: the firmware rule as built, incl. the fragment cut (FRAG_AT=n injects one)

Env: `REC` record bytes, `STEP=1` integrity check after every operation.
`matrix6.sh` / `matrix7.sh` run the scenario matrices.

`trunc.exe` isolates `SPIFFS_ftruncate` vs an admitted copy + rename
(`COPYCUT=1`), with (`1`) or without (`0`) a remount in between.

## rot.exe - QSO log rotation under power cuts (2026-11-13)

Compiles the real `main/adif/adif_rotate.c`. The SD archive is a RAM file
with FAT semantics. Cuts power at every flash program/erase and archive write
(stride = first arg), then cold-boots: mount, gc 64 KB, check, qso.tmp
recovery, exactly as `adif_log_init()` does.

```bash
gcc -O2 -w -Iinc -I../../main/adif -I/c/esp/v5.4.4/esp-idf/components/spiffs/include -I$S \
    -o rot.exe rot.c ../../main/adif/adif_rotate.c $S/*.c
./rot.exe 7            # rotation scenarios
BASE=1 ./rot.exe 3     # baseline: the same sweep, no rotation
```

Env: `NOHALF=1` clean cut (default: the cut page is half programmed),
`NOCHECK=1` skip SPIFFS_check at boot, `START=k` first cut, `DBGB=1` print
file sizes at each boot step.

⛔ After a cut, never `SPIFFS_unmount()`: it flushes the write cache, i.e.
writes flash after the power died. The first version did, and counted its
own writes as losses.

| What (stride 7 / 3) | Result |
|---|---|
| rotation: 9 scenarios, 1,956 cuts | archive + device log exact after recovery in all cases where SPIFFS kept qso.adi; 0 writes failing after a passed reserve |
| rotation: qso.adi deleted by boot check | 9 cuts (torn page) / 17 (clean), 0.5-0.9 % |
| full partition at the rewrite | ROT_FULL, nothing lost, move finished when space returned |
| foreign archive / archive missing | MISMATCH, nothing touched |
| upload cursor 50 / 0 | 89 / 139 records held, not moved |
| **BASELINE, no rotation, 1,930 cuts** | **qso.adi missing after boot in 168 (torn) / 193 (clean), ~9-10 %** |
| baseline without boot check | 0 missing, 332 UNREADABLE |

⛔ The baseline is the finding: a power cut during ANY SPIFFS write (diag
tick, QSO append, their gc) leaves qso.adi unreadable in ~17 % of cuts; boot
`SPIFFS_check` repairs about half and DELETES the file in the rest. This is
the shipped firmware's behaviour, not the rotation's. Not yet seen in the
field; the model (writes stop at the cut, RAM state lost) is the standard one.

## Results (2026-11-13)

| What | Result |
|---|---|
| v1.26.2, 300 B records, full partition | file index corrupted in 8 of 8 scenarios |
| reserve check before every append (MODE 6) | 0 corrupt in 32 scenarios, 150-600 B records |
| `SPIFFS_ftruncate` then one append | file broken for 16-43 of 109 sizes, all >= ~330 KB |
| admitted copy + rename | 0 broken in 418 cuts |
| clear at the wall | full capacity again (3,276 more QSOs) |
| firmware rule as built (MODE 7), 72 scenarios incl. injected fragments | 0 corrupt, 0 writes failing after a passed check; 55 reached refusal |
| MODE 7 with the first, `truncate`-based fragment cut | 15 of 72 broke - all of them the cut |
