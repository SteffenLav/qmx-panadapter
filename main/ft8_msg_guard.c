#include "ft8_msg_guard.h"

#include <ctype.h>
#include <string.h>

void ft8_msg_normalize(char *s)
{
    if (!s) return;

    // One pass handles all three cases: a space is only ever emitted when
    // something has already been written (w != s), which drops a leading run,
    // and only when something follows it, which drops a trailing one.
    const char *r = s;
    char *w = s;
    bool pending_space = false;
    while (*r) {
        if (*r == ' ' || *r == '\t') {
            pending_space = true;              // remember, but emit at most one
        } else {
            if (pending_space && w != s) *w++ = ' ';
            pending_space = false;
            *w++ = *r;
        }
        r++;
    }
    *w = '\0';                                 // trailing run simply never emitted
}

// Case-insensitive search for `needle` as a whole token of `hay` (space
// delimited). A substring test would accept "WB0LQW" inside "<WB0LQW>" - which
// is exactly the hashed form we want to allow - but would also accept it inside
// an unrelated longer token, so the delimiters are checked explicitly and the
// angle brackets ft8_lib puts around a resolved hash are treated as delimiters
// too.
static bool has_token(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    size_t n = strlen(needle);

    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, n) != 0) continue;

        char before = (p == hay) ? ' ' : p[-1];
        char after  = p[n];
        bool lhs = (before == ' ' || before == '<');
        bool rhs = (after == '\0' || after == ' ' || after == '>');
        if (lhs && rhs) return true;
    }
    return false;
}

bool ft8_msg_roundtrip_ok(const char *typed, const char *decoded, const char *my_call)
{
    if (!typed || !decoded) return false;
    if (!*decoded) return false;

    // A CQ must still be a CQ. Losing this means the encoder reinterpreted the
    // message as something else entirely (Don's became a signal report).
    bool typed_cq   = (strncasecmp(typed, "CQ", 2) == 0) &&
                      (typed[2] == ' ' || typed[2] == '\0');
    bool decoded_cq = (strncasecmp(decoded, "CQ", 2) == 0) &&
                      (decoded[2] == ' ' || decoded[2] == '\0');
    if (typed_cq && !decoded_cq) return false;

    // Our callsign must survive. Only enforced when the operator actually put it
    // in the message - a free-text CQ that never named us is their business.
    if (my_call && *my_call && has_token(typed, my_call) &&
        !has_token(decoded, my_call)) {
        return false;
    }

    return true;
}
