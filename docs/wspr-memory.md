# WSPR page memory - inventory and budget

Written 2026-11-11 after v1.26.0 decoded nothing on bench1 after the first cycle
(`NO MEMORY for the candidate search`, every cycle). This file is the reference for
what the WSPR receiver allocates. Change it in the same commit as any allocation.

## Failure series (bench1 capture, cycles / cycles with NO MEMORY)

| Period | Cycles | NO MEMORY |
|---|---|---|
| 2026-10-21 .. 10-31 | 1045 | 0 |
| 2026-11-03 (`b94841ec`) | 148 | 136 |
| 2026-11-04 | 336 | 90 |
| 2026-11-11 (v1.26.0) | 4 | 4 |

## Inventory before the fix (v1.26.0)

Sizes are exact for `CAP_SAMPLES` = 1,440,000 (120 s at 12 kHz).

| Buffer | Where | Bytes | Lifetime in v1.26.0 |
|---|---|---|---|
| `cap` float capture | `wspr_rx.c` slot loop | 5,760,000 | freed and re-allocated EVERY cycle (T150) |
| `s_pcm[0]` int16 window | `wspr_rx.c` slot loop | 2,880,000 | page entry to page exit |
| search `kiss_fftr` cfg, nfft 131072 | `wspr_find_candidates` | ~1,311,000 contiguous | allocated every pass, freed after it (`b94841ec`) |
| search `in` / `spec` / `mag` | `wspr_find_candidates` | 524,288 / 524,296 / 262,148 | same |
| search `score` | `wspr_find_candidates` | ~13,000 (calloc, internal RAM) | every call |
| stage-1 `s_s1i` / `s_s1q` | `build_stage1` | 2 x 720,000 | grown on demand, held to task exit |
| stage-2 `bb->i` / `bb->q` | `mix_decimate` | 2 x 180,000 | every candidate |
| Fano `gamma` / `enc_state` / `stage` | `wspr_fano.c` | ~1,300 (malloc, internal RAM) | every decode attempt |
| waterfall `s_wf`, `s_wf_mag`, 8192-pt cfg, in, spec | `wf_begin` | ~340,000 | first entry, never freed |
| `wspr_rx` + `wspr_dec` task stacks | `psram_task_create_reapable` | 2 x 32,768 | page entry to page exit |

## Why every earlier fix moved the failure instead of removing it

- The decode of window k runs while window k+1 is captured. So `cap`, `s_pcm`, and
  the decode scratch are live AT THE SAME TIME. The T150 note ("never resident at
  the same time") is wrong. The true peak is their sum.
- Every fix (09-19 cache, 11-03 release, T150 lend-back) changed WHEN a buffer was
  allocated, and kept at least one multi-MB allocation inside the cycle. Each cycle
  then depends on how fragmented PSRAM is at that moment.
- 2026-11-11: the largest free block was 2,359,296 B. `cfg` + `in` + `spec` need
  about 2,359,600 B. `spec` (524,296 B, the size in every `ALLOC FAILED`) missed by
  a few hundred bytes.

## Budget after the fix (all taken at page entry, held to page exit)

| Buffer | Bytes | Notes |
|---|---|---|
| `cap` float capture | 5,760,000 | no longer freed per cycle |
| `s_pcm[0]` int16 window | 2,880,000 | unchanged |
| decoder arena (`wspr_decode_scratch_init`) | 3,111,008 in 4 blocks, largest 1,311,008 | R0 = search cfg, built once; R1-R3 each shared by one search buffer and one stage buffer |
| waterfall | ~340,000 | now first allocated at entry |
| task stacks | 65,536 | unchanged |
| Fano work arrays | ~1,000 | static, PSRAM `.bss` - no runtime allocation |
| **Total** | **~12.15 MB** | measured free before entry on bench1, 2026-11-11: 12,869 KB |

The total is the same peak the old code needed whenever a decode overlapped a
capture. The difference: it is taken once, while the heap is clean, and either
fits or the page says so at entry with the numbers.

## Second version cost 11 s per search (28891b06)

28891b06 shared R0 between the search cfg and stage-1 I, so every search
rebuilt the 131072-point twiddles. Measured on bench1, 2026-11-11: search
14.9 s and 15.4 s, against 3.0-3.7 s in every earlier build. Earlier builds
took the same 14-17 s on their FIRST search after page entry, which is where
the figure comes from: about 11 s of double-precision cos/sin in software.
Two searches per cycle = about 22 s of the 115 s decode budget.

The memory part of that run was good: page entry allocated everything with
1256 KB PSRAM left (largest free 1184 KB), and cycle 1 decoded 2.

Fix: R0 holds the cfg alone, built once in `wspr_decode_scratch_init`.
Stage-1 I/Q move to R1/R2, stage-2 to R3. About 0.3 MB more arena, same
largest block. `test/wspr_noalloc_harness.c` now also fails on any
`kiss_fftr_alloc` build inside a cycle (mutation-tested: 6 builds, FAIL).

## First version failed on the bench (6d110df2)

The arena was one 2,621,776 B block. On bench1 it never fitted: 4.5 MB free after
capture and window, largest block 2304 KB - the same figure measured that morning,
so a fixed property of this heap. I had that number and did not check the design's
largest allocation against it. Now 4 blocks, largest 1,311,008 B, and the harness
fails if the largest block grows past 1,400,000 B.

## Proof

- `test/wspr_noalloc_harness.c`: real decoder, malloc/calloc/realloc/free
  and kiss_fftr_alloc wrapped, three full cycles on the WSJT reference
  recording. 0 allocations, 0 cfg builds, 6/6/6 decodes. Mutation-tested: one malloc added to the decode path gives
  120 counted allocations and FAIL.
- All 10 WSPR host programs produce output identical to v1.26.0 (timing
  figures excluded).
- Bench acceptance (not done yet): bench1 on WSPR, RX only, at least 30 cycles
  (1 hour), and the WHOLE series pulled from the capture: 0 lines with
  `NO MEMORY` or `ALLOC FAILED`, free PSRAM flat after the first cycle.

## The rule after the fix

1. Everything the receiver needs is allocated ONCE, at page entry, and freed at page
   exit. Nothing inside a cycle calls malloc, calloc or free.
2. The search scratch and the stage-1/stage-2 scratch are never used at the
   same moment, so they share blocks. The search's FFT cfg is NOT shared: its
   twiddles cost 11 s to build on the P4, so they are built once at entry.
3. If the page-entry allocation does not fit, the page says so once, with the
   numbers. It does not fail silently each cycle.
4. A host test fails if any allocation happens between page entry and page exit.
