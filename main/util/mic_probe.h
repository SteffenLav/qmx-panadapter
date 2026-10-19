#pragma once

/* A ONE-SHOT boot probe: what is actually wired to the ES7210's four ADC
 * channels?
 *
 * ⛔ WHY THIS CANNOT BE AN ORDINARY /api/cmd ACTION. bsp_audio_codec_microphone
 * _init() calls bsp_audio_init(), which claims BOTH I2S channels on the port -
 * and rx_audio.c has already taken that port with a deliberately TX-ONLY
 * channel, because creating the RX (mic, TDM 4-slot) channel too starves the
 * USB host's endpoint allocation and CDC-ACM/CAT cannot claim its endpoints
 * (see the long note at rx_audio.c's preopen). So the mic path and the QMX
 * cannot both exist in one boot. The probe therefore runs EARLY, before
 * rx_audio_preopen(), and reboots straight afterwards so the session that
 * follows is a normal one.
 *
 * WHAT IT IS FOR. Tony (GitHub #17) reports the Tab5's 3.5 mm socket is a
 * 4-pole headset jack with the microphone on the ring nearest the body, and
 * wants to feed a receiver into it instead of using a USB audio dongle.
 * ⚠ M5Stack's own documentation calls it a "headphone jack" and lists only
 * HP_DET for it, and the schematic is not published - so whether that contact
 * reaches an ADC channel is UNKNOWN, and a previous answer that guessed went
 * out publicly and was wrong. This measures it instead: feed a tone into the
 * mic ring, read all four channels, and see which one moves.
 *
 * Reading: four RMS and peak figures in the log. A channel carrying a signal
 * is unmistakable next to three that are not.
 */

#include <stdbool.h>

/* Arm the probe and reboot into it. ⛔ REBOOTS IMMEDIATELY, and the probe
 * reboots again when it finishes - so the QMX is wedged twice and needs a
 * power cycle afterwards (#74). */
void mic_probe_request(void);

/* Call EARLY in app_main, before rx_audio_preopen() claims the I2S port.
 * Does nothing unless the probe was armed. Never returns when it runs: it
 * restarts the board. */
void mic_probe_run_if_pending(void);
