// Directed-CQ classifier - see ft8_directed_cq.h for the rules and for why it
// is deliberately under-confident.
//
// Host-testable on purpose: it takes strings and returns a verdict, with the
// only outside dependency being util/geo_coords.c's prefix table. That is what
// test/directed_cq_harness.c exercises, and the collisions in the header
// ("OK" = Oklahoma and Czechia) were measured there, not reasoned about.

#include "ft8_directed_cq.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "util/geo_coords.h"

// The 50 states. Deliberately NOT including DC, PR, VI or the other
// territories: those are separate DXCC entities, so a station calling for them
// is making a different request and the prefix table already answers it.
static const char *const kUsStates[] = {
    "AL","AK","AZ","AR","CA","CO","CT","DE","FL","GA",
    "HI","ID","IL","IN","IA","KS","KY","LA","ME","MD",
    "MA","MI","MN","MS","MO","MT","NE","NV","NH","NJ",
    "NM","NY","NC","ND","OH","OK","OR","PA","RI","SC",
    "SD","TN","TX","UT","VT","VA","WA","WV","WI","WY",
};

bool ft8_cq_token_is_us_state(const char *tok)
{
    if (!tok || strlen(tok) != 2) return false;
    for (size_t i = 0; i < sizeof(kUsStates) / sizeof(kUsStates[0]); i++)
        if (strcmp(tok, kUsStates[i]) == 0) return true;
    return false;
}

/* The token between "CQ " and the next space, upper-cased, or "" when there
 * is no directed token.
 *
 * ⛔ A TOKEN CONTAINING A DIGIT IS THE CALLER'S OWN CALLSIGN, not a
 * destination: "CQ OZ1LAV JO65" is a plain CQ. Without this test every
 * ordinary CQ would be read as directed at a place called OZ1LAV, which the
 * prefix table would then happily resolve to Denmark. */
static void directed_token(const char *text, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!text || strncmp(text, "CQ ", 3) != 0) return;
    const char *p = text + 3;
    while (*p == ' ') p++;
    size_t n = 0;
    while (p[n] && p[n] != ' ') n++;
    if (n < 2 || n >= out_len) return;          // 1 char is never a destination
    for (size_t i = 0; i < n; i++) {
        if (isdigit((unsigned char)p[i])) return;   // a callsign, not a place
        if (!isalpha((unsigned char)p[i])) return;  // "/" etc - not judged
        out[i] = (char)toupper((unsigned char)p[i]);
    }
    out[n] = '\0';
}

ft8_cq_direction_t ft8_cq_direction(const char *text,
                                    const char *their_call,
                                    const char *my_call,
                                    const char *my_state,
                                    char *why, size_t why_len)
{
    if (why && why_len) why[0] = '\0';

    char tok[8];
    directed_token(text, tok, sizeof(tok));
    if (!tok[0]) return FT8_CQ_NOT_DIRECTED;

    /* Our own entity is the yardstick for both rules. Without it nothing can
     * be judged, so an operator who has not set a callsign simply gets today's
     * behaviour rather than a guess. */
    const char *mine = (my_call && my_call[0]) ? geo_coords_iso_for_call(my_call) : NULL;
    if (!mine) return FT8_CQ_MAYBE_FOR_US;

    if (strcmp(tok, "DX") == 0) {
        /* We are not DX to a station in our own country. That is the certain
         * part; "outside my continent", which is what DX usually means on the
         * air, needs a continent table we do not have. So a same-entity CQ DX
         * is declined and everything else is answered. */
        const char *theirs = (their_call && their_call[0])
                           ? geo_coords_iso_for_call(their_call) : NULL;
        if (theirs && strcmp(theirs, mine) == 0) {
            if (why && why_len)
                snprintf(why, why_len, "CQ DX, but %s is in our own entity (%s)",
                         their_call, mine);
            return FT8_CQ_NOT_FOR_US;
        }
        return FT8_CQ_MAYBE_FOR_US;
    }

    /* ⛔⛔ "DE" IS NEVER JUDGED AS A STATE. IT ALMOST ALWAYS MEANS "FROM".
     *
     * Randy N4OPI reported being auto-answered to "CQ DE WB4FIS" and read it
     * as a Delaware call. His own diagnostic log says otherwise: the decode is
     * "CQ DE WB4FIS EM70", and EM70 is FLORIDA. The same log carries 51 "CQ DE"
     * decodes - KD2UUC in FN20 (New York), AJ6CG in DM14 (California) and
     * others. Not one is from Delaware. They are all using DE in its ordinary
     * sense, the old abbreviation for "from", in a free-text CQ.
     *
     * So declining on DE would have cost him every one of those contacts while
     * appearing to do what he asked for. This is the cheap mistake the whole
     * file is built to avoid, and it is the one case where the header's
     * prefix-collision guard is not enough: the collision here is not with a
     * callsign prefix, it is with plain English.
     *
     * ⚠ A genuine Delaware call is therefore answered. That is the correct
     * trade - one unnecessary answer against 51 lost contacts - and it is the
     * same doctrine as everything else here. */
    if (strcmp(tok, "DE") == 0) return FT8_CQ_NOT_DIRECTED;

    if (ft8_cq_token_is_us_state(tok)) {
        if (strcmp(mine, "USA") == 0) {
            /* ⭐ INSIDE THE USA, DECIDE IT ONLY IF THE OPERATOR TOLD US WHERE
             * HE IS. The entity is known, the state is not - my_grid gives a
             * position but not a reliable state line, which is exactly why
             * Randy asked for a field of his own ("Like maybe a 'Your State'
             * field next to 'Your Grid' in FT8 settings"). With it set, a call
             * for another state is demonstrably not for us.
             *
             * ⛔ Unset is still MAYBE, not a guess. A Florida station who has
             * not filled the field in must never be told that "CQ FL" is not
             * for them. */
            if (my_state && my_state[0] && my_state[1]
                && strncmp(my_state, tok, 2) != 0) {
                if (why && why_len)
                    snprintf(why, why_len, "CQ %s, and we are in %s", tok, my_state);
                return FT8_CQ_NOT_FOR_US;
            }
            return FT8_CQ_MAYBE_FOR_US;
        }
        /* ⚠ The ambiguity guard. Several state codes are also callsign
         * prefixes elsewhere: OK (Czechia), CA (Chile), DE (Germany). If the
         * token resolves to OUR OWN entity as a prefix, the caller may well
         * have meant us, so say nothing. */
        const char *as_prefix = geo_coords_iso_for_call(tok);
        if (as_prefix && strcmp(as_prefix, mine) == 0) return FT8_CQ_MAYBE_FOR_US;

        if (why && why_len)
            snprintf(why, why_len, "CQ %s is a US state call and we are %s", tok, mine);
        return FT8_CQ_NOT_FOR_US;
    }

    /* Everything else - continents ("EU", "NA"), country tokens ("JA"),
     * contests and activities ("TEST", "POTA", "FD", "WW") - is not judged.
     * Under-declining is the cheap mistake. */
    return FT8_CQ_NOT_DIRECTED;
}
