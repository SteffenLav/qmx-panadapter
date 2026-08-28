#pragma once

#include <stdbool.h>

/* Publish WSPR reception reports to wsprnet.org.
 *
 * ⛔ THIS PUBLISHES UNDER THE OPERATOR'S CALLSIGN TO A PUBLIC SCIENTIFIC
 * DATABASE. wsprnet spots are what other people draw propagation conclusions
 * from, and an indexed spot cannot be taken back. So:
 *
 *   - it is OFF by default (settings wspr_net_en) and only the operator turns
 *     it on;
 *   - a spot is published only once its callsign has been heard MORE THAN
 *     ONCE. WSPR has no CRC, so repetition is the only confirmation available
 *     that needs neither a second decoder nor the internet. A real station
 *     transmits again; a false decode does not;
 *   - nothing is marked sent until the server has accepted it, so a failed
 *     upload is retried rather than silently dropped;
 *   - it refuses to send at all without a callsign and grid, because a report
 *     with no reporter is noise in someone else's dataset.
 *
 * THE WIRE FORMAT was taken from the parameter list WSJT-X and its forks send
 * to http://wsprnet.org/post (wsprnet's own documentation is not publicly
 * readable - the API owner's note says to contact the custodian). The VALUE
 * formats are wsprd's printed output, which this repository happens to carry
 * on disk: test/wav_reference/wspr/ *.txt are real wsprd runs, and every field
 * below was checked against them rather than assumed. See wsprnet.c.
 */
void wsprnet_init(void);

/* Compose the request for the oldest publishable spot and LOG it without
 * sending. The dev action behind {"action":"wspr_net_dryrun"} - so the exact
 * bytes can be eyeballed against a known-good reference before anything real
 * is published. Returns false if there is nothing eligible. */
bool wsprnet_dry_run(void);

/* One line for the WSPR page: what the uploader is doing, or why it is not. */
const char *wsprnet_status(void);
