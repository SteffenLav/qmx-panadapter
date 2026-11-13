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
