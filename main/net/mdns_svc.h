#pragma once
#include <stdbool.h>

// mDNS responder: makes the Tab5 reachable as http://qmx.local, and advertises
// its web server so it shows up in service browsers.
//
// WHY THIS EXISTS. The device chooses its own network: wifi.c remembers up to
// six and roams to whichever is on the air, so the IP address changes without
// anybody deciding it should - and the one place it is displayed is the Tab5's
// own bottom bar, which is no help when the Tab5 is in another room. A name that
// does not change removes the whole problem.
//
// Deliberately a FIXED hostname rather than a setting. It is the one piece of
// information an operator has to remember about the web UI, and something you
// have to look up is not much better than an IP. Two units on one network would
// collide - if that ever comes up, make it a setting then, and default it to
// this.
#define MDNS_SVC_HOSTNAME "qmx"

// Start the responder. Safe to call repeatedly - the got-IP path does, on every
// connect, reconnect and roam; only the first call does any work.
//
// Call once an IP exists: the responder needs an interface to announce on.
void mdns_svc_start(void);

// True once the responder is running (the name resolves). Reported in the boot
// diagnostics so a "qmx.local doesn't work" report can be answered from the log.
bool mdns_svc_is_up(void);
