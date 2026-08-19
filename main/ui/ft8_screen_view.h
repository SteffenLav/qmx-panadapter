#pragma once
#include "lvgl.h"

// Step 4c.2 v0.10: FT8 screen LVGL view.
//
// Owns the middle-band container shown when ui_mode == UI_MODE_FT8.
// Top bar + bottom bar are shared with panadapter and remain
// visible across mode swaps. ft8_screen.c remains the data layer;
// this file only reads via ft8_screen_get_all().
//
// Refresh model:
//   - ft8_task on core 1 calls request_refresh() at slot end
//   - request_refresh() sets a volatile flag (no LVGL calls)
//   - An LVGL timer at 500 ms (runs on LVGL task / core 0) reads
//     the flag, snapshots the table, sorts by last_utc desc, and
//     rebuilds the lv_list.
//   - A second 1 Hz timer updates UTC clock + slot countdown +
//     dial freq label, independent of decode activity.

void ft8_screen_view_init(lv_obj_t *parent);
void ft8_screen_view_show(void);
void ft8_screen_view_hide(void);

// True while the FT8 screen is the visible one (vs. Panadapter). Used by the
// web server to know which screen is live, e.g. to gate the spectrum WS
// stream to Panadapter mode only. Safe to call from any task (plain bool read).
bool ft8_screen_view_is_active(void);

// Returns the FT8 screen's middle-band container, for slide animations
// driven by ui.c. May return NULL before ft8_screen_view_init().
lv_obj_t *ft8_screen_view_get_container(void);

// Safe to call from any task. Just sets a volatile flag.
void ft8_screen_view_request_refresh(void);

// Ask for a fresh CQ run to start, exactly as the Call CQ button does (same preset,
// same TX-hold tone choice, same EVEN/ODD parity). Safe to call from ANY task: it
// only raises a flag that the FT8 view's 1 Hz timer drains on the LVGL thread, since
// the QSO state machine belongs to that task. Ignored, with a log line, if FT8 mode
// is not up - there would be nothing to transmit on.
void ft8_screen_view_request_cq(void);

// Reply to a decoded station from the web UI. Thread-safe request; the LVGL
// timer builds the correct next message (WSJT-X double-click semantics) and
// either starts the full auto-QSO (their fresh CQ) or arms the one next message
// (mid-exchange). The outcome lands in ft8_screen_view_get_web_reply_result(),
// surfaced through /api/status - a Tab5 toast is invisible from another room.
void ft8_screen_view_request_reply(const char *call);

// Mid-QSO override from the browser (#205): 1=resend 2=RR73 3=73 4=cancel.
// Deferred to the LVGL task and consumed even when FT8 is not up, so a stale
// request cannot key the radio minutes later.
void ft8_screen_view_request_override(int what);
const char *ft8_screen_view_get_web_reply_result(void);

// Refresh the "Call CQ" button label to the currently-selected CQ preset.
// Called by the CQ preset modal after a save. LVGL-thread only.
void ft8_screen_view_refresh_cq_label(void);

// Re-activates the row adjacent (delta = +1 down / -1 up) to the one last
// confirmed via row_activate(), re-resolving and re-opening the TX
// confirmation modal for it. Lets the operator correct a mis-selected row
// from the confirm modal itself instead of re-doing the hold-and-drag
// gesture. No-op (silent) if there's no adjacent visible row or nothing was
// ever confirmed this session. LVGL-thread only.
void ft8_screen_view_nudge_confirm(int delta);
