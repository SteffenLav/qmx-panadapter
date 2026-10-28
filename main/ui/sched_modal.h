#pragma once

// Scheduled events (T116) - what is on now and what is coming.
//
// One list, two kinds of thing: ?OTA activations somebody has announced, and
// ordinary CW/SSB contests. They share a pane because they share the question
// the operator is actually asking - "what is worth switching on for tonight" -
// and because they arrive from one endpoint (see sched_events_* in net/spots.h).
//
// Reached from the Activation window, which is where the operator already is
// when planning, and from a keyboard shortcut.

void sched_modal_show(void);
