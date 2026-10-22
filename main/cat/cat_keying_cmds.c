/* Classifier for CAT text that did not come from our own code - see the header
 * for why it exists and why it errs toward refusing.
 *
 * No ESP dependencies on purpose: test/cat_keying_cmds_harness.c compiles this
 * file straight on the host.
 */
#include "cat_keying_cmds.h"

#include <ctype.h>
#include <string.h>

/* The QMX takes the argument in Hz with two decimals ("TA1234.56;"). Parsed by
 * hand rather than with strtod: the fraction is rounded away here anyway, and
 * a locale that reads '.' as a group separator would be a silent wrong answer
 * in the one place that must not have one. */
static uint32_t parse_hz(const char *arg, size_t len, bool *ok)
{
    uint32_t whole = 0;
    size_t i = 0;
    bool any = false;
    while (i < len && isdigit((unsigned char)arg[i])) {
        whole = whole * 10u + (uint32_t)(arg[i] - '0');
        any = true;
        i++;
        if (whole > 100000000u) break;        /* nonsense; stop accumulating */
    }
    /* A fractional part is accepted and dropped. Rounding to the nearest Hz
     * would move the emitted edge by at most 1 Hz, far inside any band edge
     * this guards, and truncating keeps the arithmetic exact. */
    *ok = any;
    return whole;
}

void cat_keying_classify(const char *text, cat_keying_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!text) return;

    const char *p = text;
    while (*p) {
        /* One command: letters, then the argument, up to ';' or the end. A
         * trailing command with no ';' is still classified - the radio would
         * see it the moment the terminator arrived, and refusing it costs
         * nothing. */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        char verb[4] = {0};
        size_t vn = 0;
        while (*p && isalpha((unsigned char)*p)) {
            if (vn < sizeof(verb) - 1) verb[vn++] = (char)toupper((unsigned char)*p);
            p++;
        }
        const char *arg = p;
        while (*p && *p != ';') p++;
        size_t arg_len = (size_t)(p - arg);
        if (*p == ';') p++;

        if (vn == 0) continue;               /* stray punctuation */

        bool keys_here = false;
        if (strcmp(verb, "TX") == 0) {
            /* TX; and TX0/TX1/TX2 all key. */
            keys_here = true;
        } else if (strcmp(verb, "KD") == 0) {
            /* KD; is a READ. KD0 is key-up. Anything else with a non-zero
             * digit is key-down. */
            for (size_t i = 0; i < arg_len; i++) {
                if (isdigit((unsigned char)arg[i]) && arg[i] != '0') { keys_here = true; break; }
            }
        } else if (strcmp(verb, "MD") == 0) {
            for (size_t i = 0; i < arg_len; i++) {
                if (arg[i] == '8') { keys_here = true; break; }
            }
        } else if (strcmp(verb, "TA") == 0) {
            bool ok = false;
            uint32_t hz = parse_hz(arg, arg_len, &ok);
            if (ok && hz > 0) {
                keys_here = true;
                out->audio_known = true;
                out->audio_hz = hz;
            }
            /* TA0; is the tone OFF, and TA; is a read. Neither emits. */
        }

        if (keys_here && !out->keys) {
            out->keys = true;
            size_t n = vn;
            if (n > sizeof(out->verb) - 1) n = sizeof(out->verb) - 1;
            memcpy(out->verb, verb, n);
            out->verb[n] = '\0';
        }
    }
}
