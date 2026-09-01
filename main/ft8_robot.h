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
//
// DISCLAIMER: this keys the radio and runs full QSOs with zero per-exchange
// confirmation. Never leave it running unsupervised - the operator remains
// responsible for everything it transmits under their callsign. The Filter
// modal shows this warning every time the toggle is visible, not just once.

// Called from ft8_qso_advance() after each RX slot's decode, with that slot's
// UTC second. No-op unless robot is enabled AND the QSO machine is IDLE. When
// it picks a target it calls ft8_qso_start() internally.
void ft8_robot_tick(int64_t slot_sec);

// Switch auto-answer OFF and say why. Clears the stored setting, so the Filter
// modal's checkbox agrees with reality rather than claiming a mode that is no
// longer running.
//
// Called wherever continuing to transmit unattended would be a surprise:
//   - a band change, because the antenna is usually not tuned for the new band
//     and few operators have an auto-ATU (Roy KI0ER);
//   - the operator cancelling a transmission, because they are cancelling in
//     order to do something else - check an antenna, close the station - and a
//     re-arm a cycle later is exactly what they were preventing;
//   - startup, so unattended transmission is never the state the device powers
//     up in.
//
// `reason` is logged and shown to the operator; pass NULL for no toast (boot).
void ft8_robot_stand_down(const char *reason);

// True once BOTH transmit windows have been listened to recently enough for the
// tone-occupancy map to mean anything. The robot refuses to transmit until then:
// straight after a band change or a startup the map is empty, so a tone picked
// from it is picked from nothing at all (Roy KI0ER).
bool ft8_robot_occupancy_ready(void);
