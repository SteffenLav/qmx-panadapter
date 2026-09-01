// spot_sig_harness.c - host-side verification of spot_sig_for()
// (main/net/spot_sig.c), the rule that decides the ADIF SIG of a logged chase.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main/net -o test/spot_sig.exe test/spot_sig_harness.c main/net/spot_sig.c
//   ./test/spot_sig.exe
//
// WHY THIS EXISTS
//   The SIG is not cosmetic: it is what the other station's log is matched
//   against. A summit filed as SIG=POTA earns neither side anything, and a WWFF
//   reference filed as POTA is a false claim in somebody's log. Nothing on the
//   device can catch that - a wrong-but-plausible SIG uploads cleanly to QRZ,
//   eQSL and LoTW and is only discovered by whoever fails to get the match.
//
//   The rule cannot be exercised from the device either: it needs a completed
//   QSO with a DX-cluster-sourced station whose reference belongs to a
//   programme other than the one you would guess. That is a rare accident to
//   wait for, and this is a pure string function, so it is testable here.
//
// WHY IT COMPILES THE REAL FILE
//   test/spots_dedup_harness.c mirrors its algorithm by hand and says in its own
//   header that it is "worse than useless" if the two ever drift - it would keep
//   passing. spot_sig.c is deliberately dependency-free precisely so this
//   harness can LINK it instead of copying it, which removes that failure mode
//   rather than warning about it.

#include <stdio.h>
#include <string.h>
#include "spot_sig.h"

static int fails;

static void expect(spot_source_t src, const char *ref, const char *want,
                   const char *why)
{
    const char *got = spot_sig_for(src, ref);
    static const char *srcname[] = { "POTA", "RBN", "CLUSTER", "SOTA" };
    if (strcmp(got, want) != 0) {
        printf("  FAIL  src=%-7s ref=%-12s want=%-4s got=%-4s  (%s)\n",
               srcname[src], ref ? ref : "(null)", want, got, why);
        fails++;
    } else {
        printf("  ok    src=%-7s ref=%-12s -> %-4s  (%s)\n",
               srcname[src], ref ? ref : "(null)", got, why);
    }
}

// The reference-only entry point (spot_sig_for_ref), which the web log editor
// calls when an operator types a Park-to-Park reference against an
// already-logged QSO - there is no spot, so there is no source to ask. It must
// agree with the cluster path on every input, because they are meant to be the
// same rule and not two copies of it.
static void expect_ref(const char *ref, const char *want, const char *why)
{
    const char *got  = spot_sig_for_ref(ref);
    const char *clus = spot_sig_for(SPOT_SRC_CLUSTER, ref);
    if (strcmp(got, want) != 0 || strcmp(got, clus) != 0) {
        printf("  FAIL  ref=%-12s want=%-4s got=%-4s cluster=%-4s  (%s)\n",
               ref ? ref : "(null)", want, got, clus, why);
        fails++;
    } else {
        printf("  ok    ref=%-12s -> %-4s  (%s)\n", ref ? ref : "(null)", got, why);
    }
}

int main(void)
{
    printf("spot_sig_for() - ADIF SIG selection\n\n");

    printf("A programme's own feed: the source decides, never the string.\n");
    // Captured live from spothole 2026-08-10: a station activating a park AND a
    // summit on one frequency. Whichever entry survives the store's dedupe must
    // be filed as its OWN programme, so the source has to win here.
    expect(SPOT_SRC_SOTA, "FL/NO-133", "SOTA", "F/HB9CDH/P, SOTA feed");
    expect(SPOT_SRC_POTA, "FR-8017",   "POTA", "F/HB9CDH/P, POTA feed");
    expect(SPOT_SRC_SOTA, "",          "SOTA", "SOTA feed, no reference given");
    expect(SPOT_SRC_SOTA, "ES-2081",   "SOTA", "source beats a POTA-shaped ref");
    expect(SPOT_SRC_POTA, "G/LD-049",  "POTA", "source beats a SOTA-shaped ref");

    printf("\nDX cluster: the spotter typed it, so the shape decides.\n");
    // Every SOTA reference below is a real one from the live feed.
    expect(SPOT_SRC_CLUSTER, "G/LD-049",   "SOTA", "real: G4IPB/P");
    expect(SPOT_SRC_CLUSTER, "EA1/AT-125", "SOTA", "real: EA2GM/P, the 10-char case");
    expect(SPOT_SRC_CLUSTER, "DM/BW-193",  "SOTA", "real: DH2ID/P");
    expect(SPOT_SRC_CLUSTER, "TF/SV-041",  "SOTA", "real: TF/W6CMY");
    expect(SPOT_SRC_CLUSTER, "ES-2081",    "POTA", "real: from dxcluster.c's own vector");
    expect(SPOT_SRC_CLUSTER, "US-1254",    "POTA", "real: KD3CZT");
    expect(SPOT_SRC_CLUSTER, "DLFF-0123",  "WWFF", "dxcluster.c's own WWFF example");
    expect(SPOT_SRC_CLUSTER, "ONFF-0259",  "WWFF", "WWFF, two-letter prefix");

    printf("\nEdges - each one a way the shape test could be fooled.\n");
    expect(SPOT_SRC_CLUSTER, "g/ld-049", "SOTA",
           "lowercase: '/' test is case-blind anyway");
    expect(SPOT_SRC_CLUSTER, "dlff-0123", "WWFF",
           "lowercase FF: the compare must be case-insensitive");
    expect(SPOT_SRC_CLUSTER, "IT-1083", "POTA",
           "prefix ends in T, not FF - must not read as WWFF");
    expect(SPOT_SRC_CLUSTER, "OFF-0001", "WWFF",
           "prefix ends in FF: WWFF");
    expect(SPOT_SRC_CLUSTER, "US-1254/2", "POTA",
           "'/' AFTER the dash is not a SOTA region - must stay POTA");
    // THE ORDER TEST. Both rules match this string: it has a '/' before the
    // dash (SOTA) and "FF" immediately before it (WWFF). A SOTA region is two
    // letters and could be FF; a WWFF reference never contains '/'. So SOTA must
    // win, which is only true if the '/' test runs FIRST.
    //
    // Added after a mutation check: swapping the two tests left every other
    // vector here passing, so spot_sig.c's claim that the order matters was
    // asserted in a comment and verified by nothing.
    expect(SPOT_SRC_CLUSTER, "DL/FF-060", "SOTA",
           "both rules match - '/' must be tested before FF");
    expect(SPOT_SRC_CLUSTER, "W7W/KG-001", "SOTA",
           "the 10-char US summit that motivated widening ref[]");
    expect(SPOT_SRC_CLUSTER, "G4ABC", "POTA",
           "no dash at all: falls through, never crashes");
    expect(SPOT_SRC_CLUSTER, "", "POTA",
           "empty ref: caller gates on ref[0], but must be total");
    expect(SPOT_SRC_CLUSTER, NULL, "POTA",
           "NULL ref must not dereference");
    expect(SPOT_SRC_CLUSTER, "-0001", "POTA",
           "leading dash: nothing before it to inspect");
    expect(SPOT_SRC_CLUSTER, "F-0001", "POTA",
           "one char before the dash: the FF compare must not read behind it");
    // RBN never carries a reference and spots_activation_for_call() skips it
    // outright, so this only pins down that the function stays total.
    expect(SPOT_SRC_RBN, "", "POTA", "RBN: unreachable in practice, still total");

    printf("\nBy reference alone - the web log editor's Park-to-Park path.\n");
    expect_ref("US-3787",   "POTA", "real: Don WB0LQW's own P2P, 2026-08-24");
    expect_ref("G/LD-049",  "SOTA", "a Summit-to-Summit typed by hand");
    expect_ref("DLFF-0123", "WWFF", "WWFF typed by hand");
    expect_ref("us-1241",   "POTA", "the handler uppercases, but this must not depend on it");
    expect_ref("",          "POTA", "cleared reference - the caller clears SIG too, but stay total");
    expect_ref(NULL,        "POTA", "NULL must not dereference on this path either");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
