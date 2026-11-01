#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The web-radio player's control surface: the station list from the operator's
// grid, play/pause, stop, volume, and what is on air right now.
//
// Opened from the settings drawer. Closing it does NOT stop the radio - the
// player is a background task and the operator closes this to get back to the
// panadapter with the music still playing. Stop is a button.

void webradio_modal_init(void);
void webradio_modal_show(void);
void webradio_modal_hide(void);

#ifdef __cplusplus
}
#endif
