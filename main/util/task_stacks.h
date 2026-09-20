// One-shot per-task stack headroom dump.
//
// ⛔ ON DEMAND ONLY - NEVER on a periodic path. uxTaskGetSystemState() byte-
// walks every task's stack inside a kernel critical section, which is the
// documented cyan-flash cause (see the "No long interrupts-off critical
// sections" section in CLAUDE.md). One invocation may cost a single dropped
// frame; that is the accepted price for a diagnostic the operator asked for.
//
// Exists because "did taskLVGL run out of stack?" was being ANSWERED BY
// INFERENCE three times running while chasing the ADIF-viewer crash
// (lv_event_mark_deleted reading a garbage event-chain link). A high-water
// mark turns that into a number.
//
// ⚠ A high-water mark is only evidence about the paths that ACTUALLY RAN.
// Take it after exercising the thing under suspicion, not at idle - see the
// tab5_kb lesson in CLAUDE.md, where 1,008 B measured with a keyboard attached
// hid a ~2,124 B path that only runs when there is none.
#pragma once

// Logs every task: name, core, priority, stack high-water mark in BYTES, and
// flags anything under 512 B free. Reached via /api/cmd {"action":"stacks"}.
void task_stacks_report(void);
