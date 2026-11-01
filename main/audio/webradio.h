#pragma once

// Web-radio player: an HTTP MP3 stream, decoded on this board and played out
// of the same speaker the QMX's receive audio uses.
//
// ⭐ IT IS AN AMUSEMENT, and the design follows from that. One stream at a
// time, MP3 only, no playlist, no buffering cleverness - the operator asked
// for "a super simple web radio player", so the cost it is allowed to impose
// on a firmware whose real job is a panadapter is close to zero when it is
// not playing, and modest when it is.
//
// ⛔ IT CANNOT OPEN ITS OWN AUDIO CHANNEL. The codec is opened once at boot,
// before the USB host takes the DMA RAM (rx_audio_preopen), so there is one
// output and rx_audio.c owns it. The player BORROWS it through
// rx_audio_aux_claim()/_write(), which stops that file writing to the speaker
// while this one is playing. Two consequences the operator sees:
//   - Receive audio is silent while the radio plays. That is correct: one
//     speaker, and nobody wants an SSB passband mixed into Jazz.
//   - The channel is fixed at DSP_SAMPLE_RATE_HZ (48 kHz) and almost every
//     internet stream is 44.1 kHz, so this file resamples. Linear
//     interpolation, which is honest about what it is: good enough for a
//     speaker on a workbench, not a hi-fi path.
//
// ⛔ CORE 1, NEVER CORE 0. Core 0 carries USB host, the hosted-WiFi SDIO link
// and the display, and runs at 0.0% idle on a busy band - see the core-0
// budget note. An MP3 decode there would be paid for by the decoder.
//
// ⚠ NOTHING HERE TOUCHES THE RADIO. No CAT, no keying, no band change. The
// player is deaf to what the transceiver is doing and vice versa.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEBRADIO_STOPPED = 0,
    WEBRADIO_CONNECTING,
    WEBRADIO_PLAYING,
    WEBRADIO_ERROR,
} webradio_state_t;

// Start playing `url`, naming it `name` for the display. Replaces whatever is
// playing. Returns false only if the task could not be created; a stream that
// turns out to be unreachable fails later and shows up as WEBRADIO_ERROR.
bool webradio_play(const char *url, const char *name);

// Stop, release the speaker, and return receive audio to the operator.
void webradio_stop(void);

// Pause is a real stop of the stream, not a held connection: an internet
// stream has no pause, and holding a socket open for minutes to pretend
// otherwise wastes a connection the server did not agree to lend us. Resume
// reconnects to the same URL at the live edge.
void webradio_pause(void);
void webradio_resume(void);
bool webradio_is_paused(void);

webradio_state_t webradio_state(void);

// What to put on screen. `name` is the station from the directory; `title` is
// the ICY StreamTitle the server pushes - the programme or track playing - and
// is "" until one arrives. `err` carries the last failure for the operator.
// The URL of what is playing (or was last played), "" if there has never been
// one. Borrowed, not copied: valid until the next webradio_play().
//
// ⛔ THE PLAYER IS THE SOURCE OF TRUTH for "what am I hearing", not whichever
// row the operator last tapped. After a memory recall, a resume, or a search
// that reorders the list, those two are different things - and storing a
// station memory from the list selection would quietly save the wrong one.
const char *webradio_current_url(void);

void webradio_get_name(char *out, int cap);
void webradio_get_title(char *out, int cap);
void webradio_get_error(char *out, int cap);

// 0..100, shared with receive audio because it is one codec and one knob.
void webradio_set_volume(int vol);
int  webradio_get_volume(void);

#ifdef __cplusplus
}
#endif
