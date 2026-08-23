#pragma once

/* On-device WSPR decoder self-test and timing probe.
 *
 * WHY THIS EXISTS, and why it is not just a unit test: the RX slot loop that
 * Phase 3's UI displays has a hard real-time budget nothing has ever measured.
 * A WSPR cycle is 120 s, the transmission inside it is 110.6 s, and the decode
 * must finish before the next cycle's capture needs the CPU. On a laptop the
 * host harness decodes 8 candidates in ~15 s. This board is a 360 MHz RISC-V
 * that is also running the panadapter, USB audio and LVGL - so the honest
 * answer is unknown until measured ON the P4, and building a slot loop around
 * an unmeasured budget is how you get a screen that mysteriously shows nothing.
 *
 * It also answers a second question the host harness structurally cannot: does
 * the decoder produce the SAME ANSWER on this silicon? Different FPU, different
 * libm, different float rounding. Synthesizing a known message and requiring it
 * back is the only way to know.
 *
 * Same shape and same reasoning as ft8_test.c's ft8_synth_and_decode() /
 * ft8_arrl_fd_e2e_selftest(): synthesize real audio, push it through the real
 * decoder, assert on the real answer.
 *
 * ⛔ Runs on its own task with a large stack. wspr_decode_candidate() puts an
 * `int mettab[2][256]` - 2 KB - on the caller's stack, and this board's task
 * stacks are small enough that CLAUDE.md keeps a list of crashes caused by
 * exactly that (sys_evt is 2808 B, settings_flush 3064 B). Do not call the
 * decoder from an arbitrary task.
 */

// Run the self-test on its own task and return immediately. Results are logged
// (tag "wspr_st"), including per-stage milliseconds and a pass/fail verdict.
// Safe to call with no radio, no antenna and no CAT link - it synthesizes its
// own audio. Refuses to start a second run while one is in flight.
void wspr_selftest_start(void);

// True while a run is in flight.
bool wspr_selftest_running(void);
