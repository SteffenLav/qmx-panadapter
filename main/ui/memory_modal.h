#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Pre-build the modal at boot (call from ui_init). */
void memory_modal_init(void);
/* Show the memory channels modal. */
void memory_modal_show(void);
/* Close it, and whether it is up. Exposed for the physical keyboard's Esc:
   this page has no Close button (it dismisses by backdrop tap or swipe-down),
   so there is nothing for the button registry in ui.c to click. */
void memory_modal_close_now(void);
bool memory_modal_is_open(void);
#ifdef __cplusplus
}
#endif
