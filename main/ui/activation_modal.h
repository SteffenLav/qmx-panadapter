#pragma once
#include <stdbool.h>
#include <stddef.h>

// Activation session editor (POTA / SOTA).
//
// An "activation" is the operator standing in a park or on a summit, working
// people who want to contact THAT reference. Every QSO logged while one is
// running carries ADIF MY_SIG/MY_SIG_INFO, which is what POTA and SOTA read to
// credit it - so getting the reference right, and remembering to stop when
// leaving, is the whole job of this screen.
//
// The session itself lives in settings (act_type/act_ref) and is applied inside
// adif_log_record(), not here; this is only the editor.

void activation_modal_init(void);
void activation_modal_show(void);

// True while an activation is running - the drawer button and the bottom bar
// use it to show the reference rather than a generic label.
bool activation_is_running(void);

// Copies "POTA DL-0123" (or "SOTA OZ/SJ-001") into out for display. Returns
// false and empties out when nothing is running.
bool activation_describe(char *out, size_t out_sz);

/* Stop the running activation AND write its own ADIF to the SD card (T110).
 * ⛔ Both the Tab5 button and the web's set_activation(type 0) must come
 * through here: the export used to live in the button handler, so stopping
 * from the browser skipped the file without saying so.
 * `note` receives the line to show the operator. Returns records exported,
 * or -1 when none were (no card, nothing logged today, or nothing running). */
int activation_stop(char *note, size_t note_sz);
