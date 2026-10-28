#pragma once
#include <stdbool.h>

/* One-tap audio panel - Samuel W7STF, 2026-10-22.
 *
 * "I find getting to the audio-related parameters, RF gain, AGC mode, and
 * Volume gain etc is gesture intensive on the Tab5 ... a button or a 'hot'
 * area are IMHO sooooo much better (direct and natural) than a swipe gesture,
 * followed by a scroll gesture, followed by a gesture to adjust the parameter
 * or parameters, and another to dismiss the panel holding those controls."
 *
 * He counted four gestures to change the volume. This is one tap, on the
 * S-METER - the only thing in the top bar that is already about what you are
 * hearing, and the one part of that bar not already spoken for. The panel
 * carries the controls you reach for while listening and nothing else: mute,
 * the Tab5's own speaker volume, and the radio's AF gain.
 *
 * ⛔ IT IS NOT A SECOND HOME FOR EVERY AUDIO SETTING. The drawer keeps the
 * ones you set once and leave - the RX source, the filter, the AGC tuning.
 * Duplicating those here would make two places to change one thing, which is
 * how they come to disagree; this is the "while I am listening" panel, and
 * that is the whole test for what belongs in it.
 *
 * He also asked that his project not be designed around audio. It is not:
 * nothing here changes unless he opens it.
 */
void audio_modal_init(void);
void audio_modal_show(void);
bool audio_modal_is_open(void);
