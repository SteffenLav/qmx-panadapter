/* Host harness for cat_keying_classify() - T64.
 *
 *   gcc -I main/cat -o /tmp/ckc test/cat_keying_cmds_harness.c \
 *       main/cat/cat_keying_cmds.c
 *   /tmp/ckc
 *
 * The classifier decides whether CAT text arriving from outside the firmware
 * can put RF out, and a false pass there is the operator's licence. So every
 * case carries the answer it must give and the harness EXITS NON-ZERO on any
 * disagreement - a harness that only prints is one nobody reads twice.
 *
 * ⚠ The cases below are the specification. If the QMX gains another command
 * that keys, it belongs in cat_keying_cmds.c AND here, in the same change.
 */
#include <stdio.h>
#include <string.h>

#include "cat_keying_cmds.h"

struct tc {
    const char *text;
    bool        keys;         /* must be classified as able to emit */
    bool        audio_known;
    uint32_t    audio_hz;
    const char *note;
};

int main(void)
{
    static const struct tc cases[] = {
      /* --- the thing that shipped unguarded ------------------------------ */
      {"TX;",              true,  false, 0,    "the whole reason this exists"},
      {"tx;",              true,  false, 0,    "lower case still keys, or is refused for free"},
      {"TX0;",             true,  false, 0,    "Kenwood-style TX variants key too"},
      {"TX1;",             true,  false, 0,    ""},

      /* --- a keying command hidden behind a harmless one ------------------ */
      {"FA7074000;TX;",    true,  false, 0,    "refusing only the FIRST would let this through"},
      {"MM;TX;RX;",        true,  false, 0,    "discovery command in front of a key-down"},
      {"TX",               true,  false, 0,    "no terminator yet - the radio sees it when one arrives"},

      /* --- SWR Tune: the radio keys a carrier by itself ------------------- */
      {"MD8;",             true,  false, 0,    "SWR Tune carrier"},
      {"MD2;",             false, false, 0,    "USB is a mode change, not RF"},
      {"MD;",              false, false, 0,    "a read"},

      /* --- remote CW keying ---------------------------------------------- */
      {"KD1;",             true,  false, 0,    "key down"},
      {"KD0;",             false, false, 0,    "key UP emits nothing"},
      {"KD;",              false, false, 0,    "a read"},

      /* --- buffered CW text: the RADIO's keyer transmits it (added 10-22) -- */
      {"KY TEST;",         true,  false, 0,    "went through the T64 door unguarded until 10-22"},
      {"KYCQ CQ DE OZ1LAV;", true, false, 0,   "no space before the text either"},
      {"KY;",              false, false, 0,    "buffer-space read, emits nothing"},
      {"KY ;",             false, false, 0,    "whitespace only is not a message"},
      {"FA7074000;KY TEST;", true, false, 0,   "hidden behind a dial move"},

      /* --- the tone sets the frequency actually EMITTED ------------------- */
      {"TA1234.56;",       true,  true,  1234, "dial + audio is what the 60 m fault got wrong"},
      {"TA1500;",          true,  true,  1500, ""},
      {"TA0;",             false, false, 0,    "tone off emits nothing"},
      {"TA;",              false, false, 0,    "a read"},

      /* --- what must NOT be refused: this action exists for discovery ----- */
      {"FA;",              false, false, 0,    "the read that proved the hole was reachable"},
      {"FA7074000;",       false, false, 0,    "moving the dial emits nothing by itself"},
      {"MM;",              false, false, 0,    "menu discovery, the actual purpose"},
      {"MM/Band/60m;",     false, false, 0,    "an MM path with slashes"},
      {"RX;",              false, false, 0,    "stopping is never refused"},
      {"PC;SW;",           false, false, 0,    "power and SWR reads"},
      {"SR0;",             false, false, 0,    "SWR latch reset"},
      {"",                 false, false, 0,    "empty"},
      {";;;",              false, false, 0,    "stray terminators"},
    };

    int fails = 0;
    printf("%-18s %-6s %-12s %s\n", "text", "keys", "audio", "note");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct tc *c = &cases[i];
        cat_keying_t k;
        cat_keying_classify(c->text, &k);

        bool bad = (k.keys != c->keys) ||
                   (k.audio_known != c->audio_known) ||
                   (k.audio_known && k.audio_hz != c->audio_hz);
        char audio[24] = "-";
        if (k.audio_known) snprintf(audio, sizeof(audio), "%u Hz", (unsigned)k.audio_hz);
        printf("%-18s %-6s %-12s %s%s\n",
               c->text[0] ? c->text : "(empty)",
               k.keys ? "KEYS" : "no", audio, c->note,
               bad ? "   <<< WRONG" : "");
        if (bad) {
            fails++;
            printf("    expected keys=%s audio_known=%s audio=%u\n",
                   c->keys ? "KEYS" : "no", c->audio_known ? "yes" : "no",
                   (unsigned)c->audio_hz);
        }
    }

    printf("\n%u case(s), %d wrong\n",
           (unsigned)(sizeof(cases) / sizeof(cases[0])), fails);
    if (fails) {
        printf("A false PASS here is an out-of-band transmission; a false "
               "REFUSAL is one log line.\n");
    }
    return fails ? 1 : 0;
}
