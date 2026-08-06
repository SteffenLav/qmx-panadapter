#pragma once

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
