#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Is a CQ aimed at a group we are demonstrably not in?
 *
 * Randy N4OPI, 2026-10-21: "someone calling 'CQ DX <theirUScall>' or
 * 'CQ <some state I don't live in> <theircall>' is looking for a specific group
 * of stations that I do not belong to. Neither of us probably want to waste
 * time on each other. And some would say it is bad form to do so."
 *
 * This is the HALF THAT DECIDES NOT TO ANSWER AT ALL, and it is a different
 * feature from ft8_qso_skip_exchange() - that one ends an exchange already
 * running. See project_t43_skip_exchange in the notes.
 *
 * ⛔ IT IS DELIBERATELY UNDER-CONFIDENT. Declining wrongly costs a contact
 * that was there for the taking; answering a CQ that was not for us costs one
 * slot and a little etiquette. So anything this cannot establish is reported
 * as FT8_CQ_MAYBE_FOR_US and the robot answers as it does today. Only two
 * token families are judged at all:
 *
 *   "CQ DX"    - declined when the caller is in OUR OWN DXCC entity, so we
 *                cannot be their DX. Not "outside our continent", which is
 *                what DX usually means in practice: we have no continent
 *                table, and same-country is the part that is certain.
 *   "CQ <st>"  - a US state code, declined when our own entity is known and
 *                is not the USA.
 *
 * Continent calls ("CQ EU", "CQ NA") are NOT judged: deciding them needs a
 * continent per DXCC entity, which util/geo_coords.c does not carry - it has
 * centroids, ISO-3 codes and names. Guessing a continent from a centroid was
 * not worth a wrong decline.
 *
 * ⚠ TWO-LETTER TOKENS ARE GENUINELY AMBIGUOUS and the guard matters: "OK" is
 * Oklahoma and also a Czech callsign prefix; "CA" is California and also a
 * Chilean one; "DE" is Delaware and also Germany's. A state code is therefore
 * only declined when it does NOT resolve, as a callsign prefix, to our own
 * entity - so a Czech operator is never told that "CQ OK" is not for them.
 */

typedef enum {
    FT8_CQ_NOT_DIRECTED = 0,   // a plain CQ, or a token this does not judge
    FT8_CQ_MAYBE_FOR_US,       // directed, and we cannot rule ourselves out
    FT8_CQ_NOT_FOR_US,         // directed at a group we are demonstrably not in
} ft8_cq_direction_t;

/* text:       the decoded message, e.g. "CQ DX K1ABC FN42"
 * their_call: the caller's callsign (needed for the DX test); may be NULL
 * my_call:    our own callsign; without it nothing can be judged
 * why:        optional, filled on FT8_CQ_NOT_FOR_US with a short reason for
 *             the log - e.g. "CQ DX but K1ABC is in our own entity (DNK)"
 */
ft8_cq_direction_t ft8_cq_direction(const char *text,
                                    const char *their_call,
                                    const char *my_call,
                                    char *why, size_t why_len);

// True for one of the 50 US state two-letter codes. Exposed for the host
// harness, which checks the table against the collisions above.
bool ft8_cq_token_is_us_state(const char *tok);
