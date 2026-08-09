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
