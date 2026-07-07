#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MEM_SLOTS 32

typedef struct {
    uint32_t freq_hz;       /* dial frequency in Hz          */
    char     mode[8];       /* e.g. "USB", "CW", "FT8"      */
    char     label[16];     /* user label (15 chars + NUL)   */
    uint8_t  occupied;      /* 0 = empty slot                */
} mem_slot_t;

/* Call from main() after settings_init(). Loads blob from NVS. */
void mem_channels_init(void);

/* Copy slot idx into *out. Returns false if idx out of range. */
bool mem_channels_get(int idx, mem_slot_t *out);

/* Persist slot idx. Writes full blob to NVS immediately. */
void mem_channels_set(int idx, const mem_slot_t *slot);

/* Mark slot idx empty and persist. */
void mem_channels_clear(int idx);

/* True once the user has been shown the one-time "these can be moved"
 * intro animation on the Memory page. Persisted so it plays at most once
 * ever per device, regardless of how many times the page is reopened. */
bool mem_channels_demo_shown(void);
void mem_channels_mark_demo_shown(void);

#ifdef __cplusplus
}
#endif
