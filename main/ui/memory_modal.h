#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Pre-build the modal at boot (call from ui_init). */
void memory_modal_init(void);
/* Show the memory channels modal. */
void memory_modal_show(void);
#ifdef __cplusplus
}
#endif
