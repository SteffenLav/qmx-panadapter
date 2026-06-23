#pragma once
#include <stdbool.h>
#include <stdint.h>

// FT8 robot: unattended auto-answer of other stations' CQs.
//
// The hard part — running the QSO once a target is chosen (TX2/RR73/73/ADIF) —
// is already done by the ft8_qso state machine, and reply-on-immediate-slot
// (ft8_test.c) fires each step on the next slot. The robot only adds the very
// first decision a human normally makes by tapping a row: *which* CQ caller to
// answer. When enabled and the QSO machine is IDLE, ft8_robot_tick() scans the
// live decode list for CQ callers, drops anyone failing the operator's filters
// (the same ft8_filters_t used by CQ-run, plus an enforced worked-before skip),
// ranks the survivors by the chosen priority, builds a TX1 to the winner, and
// hands off to ft8_qso_start(). From there the existing machine takes over.
//
// Safety: only ever acts from IDLE (never interrupts a QSO in progress), keys
// up only through the normal armed-TX path, and is opt-in (settings.robot_en,
// default off).

// Called from ft8_qso_advance() after each RX slot's decode, with that slot's
// UTC second. No-op unless robot is enabled AND the QSO machine is IDLE. When
// it picks a target it calls ft8_qso_start() internally.
void ft8_robot_tick(int64_t slot_sec);
