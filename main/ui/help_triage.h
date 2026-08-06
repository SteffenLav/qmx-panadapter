#pragma once

#include <stdbool.h>

// "What's wrong?" - the front door to the built-in manual for someone who is
// stuck and does not know what the chapter is called.
//
// Shows a short list of SYMPTOMS ranked from live device state (see
// help_triage_collect() in help_topics.h). Tapping one opens the manual at the
// section that explains that fault; leaving the manual returns to whatever screen
// the operator came from, because the Reader is an overlay.
//
// The device ranks, the operator chooses - this panel never navigates on its own.

void help_triage_open(void);

// Is the panel on screen? Anything that re-foregrounds itself on a timer (the
// QMX-wait prompt) must stand down while it is, or it draws over the choices.
bool help_triage_is_open(void);
