#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ⛔ DOES THIS CAT TEXT PUT RF OUT? (T64)
 *
 * WHY IT EXISTS: `check_tx_guard.py` finds keying sites by grepping OUR SOURCE
 * for the literal "TX;". That works for the six sites the firmware decides on
 * its own, and it is blind to a command that arrives at RUNTIME - which is
 * exactly what `/api/cmd {"action":"cat_raw"}` accepted, with no band guard and
 * no gate, in every release up to v1.20.3. A browser on the network could key
 * the radio anywhere. Proven on the released build 2026-10-22 with a harmless
 * `FA;`.
 *
 * So runtime CAT text needs its own classifier, and it has to be judged by
 * reading its verdicts rather than by reasoning about it:
 * `test/cat_keying_cmds_harness.c` prints one line per case.
 *
 * ⚠ IT IS DELIBERATELY OVER-EAGER. A false refusal on a developer escape hatch
 * costs one log line; a false pass is the operator's licence. Where "does this
 * make RF" depends on radio state we cannot see, this says yes.
 */

typedef struct {
    bool     keys;         /* something in this text can cause RF */
    bool     audio_known;  /* a TA command set the audio offset */
    uint32_t audio_hz;     /* that offset in Hz, when audio_known */
    char     verb[12];     /* the first command that keys, for the refusal text */
} cat_keying_t;

/* text may hold SEVERAL commands ("FA7074000;TX;") - every one is classified,
 * because refusing only the first would let the second through.
 *
 * What keys a QMX, and why each is here:
 *
 *   TX     key down. TX0/TX1/TX2 key as well on a Kenwood-style set.
 *   KD<n>  key state, n non-zero = key down (remote CW keying).
 *   MD8    SWR Tune: the radio keys a carrier by itself and holds it.
 *   TA<f>  the audio tone of a keyed DiGi carrier. It makes no RF on its own,
 *          which is why check_tx_guard.py's comment rightly says it is not a
 *          keying SITE - but it SETS THE FREQUENCY ACTUALLY EMITTED, and the
 *          60 m fault (Wolfgang DF1KZ) was dial + audio landing 63 kHz below
 *          the band. From outside the firmware we cannot know whether a
 *          key-down is already standing, so a non-zero TA is treated as
 *          keying. TA0 (tone off) is not.
 *
 * Not here: FA/FB move the dial, and the dial is read at the key-down that
 * follows, so a frequency change on its own emits nothing.
 */
void cat_keying_classify(const char *text, cat_keying_t *out);
